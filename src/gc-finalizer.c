#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define GC_IMPL 1

#include "debug.h"
#include "embedder-api-impl.h"
#include "gc-ephemeron-internal.h" // for gc_visit_ephemeron_key
#include "gc-finalizer-internal.h"

// # Overview
//
// See gc-finalizer.h for a overview of finalizers from the user and
// embedder point of view.
//
// ## Tracing
//
// From the perspecive of the collector implementation, finalizers are
// GC-managed objects, allowing their size to be accounted for within
// the heap size.  They get traced during collection, allowing for
// relocation of their object references, and allowing the finalizer
// object itself to be evacuated if appropriate.
//
// The collector holds on to outstanding finalizers in a *finalizer
// state*, which holds one *finalizer table* for each priority.  We
// don't need to look up finalizers by object, so we could just hold
// them in a big list, but to facilitate parallelism we slice them
// across some number of shards, where the "next" pointer is part of the
// finalizer object.
//
// There are a number of ways you could imagine integrating finalizers
// into a system.  The way Whippet does it goes like this.  See
// https://wingolog.org/archives/2022/10/31/ephemerons-and-finalizers
// and
// https://wingolog.org/archives/2024/07/22/finalizers-guardians-phantom-references-et-cetera
// for some further discussion.
//
//   1. The collector should begin a cycle by adding all shards from all
//      priorities to the root set.  When the embedder comes across a
//      finalizer (as it will, because we added them to the root set),
//      it traces it via gc_trace_finalizer(), which will visit the
//      finalizer's closure and its "next" pointer.
//
//   2. After the full trace, and then the fix-point on pending
//      ephemerons, for each priority from 0 upwards:
//
//      i. Visit each finalizable object in the table.  If the object
//         was as-yet unvisited, then it is unreachable and thus
//         finalizable; the finalizer is added to the global "fired"
//         list, and changes state from "attached" to "fired".
//         Otherwise it is re-added to the finalizer table.
//
//     ii. If any finalizer was added to the fired list, then those
//         objects were also added to the grey worklist; run tracing
//         again until the grey set is empty, including ephemerons.
//
//   3. Finally, call the finalizer callback if the list of fired finalizers is
//      nonempty.
//
// ## Concurrency
//
// The finalizer table is wait-free.  It keeps a count of active finalizers, and
// chooses a bucket based on the count modulo the number of buckets.  Adding a
// finalizer to the table is an atomic push on a linked list.  The table is
// completely rebuilt during the GC pause, redistributing survivor entries
// across the buckets, and pushing all finalizable entries onto the single
// "fired" linked list.
//
// The fired list is also wait-free.  As noted above, it is built
// during the pause, and mutators pop items off of it atomically.
//
// ## Generations
//
// It would be ideal if a young generation had its own finalizer table.
// Promoting an object would require promoting its finalizer to the old
// finalizer table.  Not yet implemented (but would be nice).

#ifndef GC_EMBEDDER_FINALIZER_HEADER
#error Embedder should define GC_EMBEDDER_FINALIZER_HEADER
#endif

enum finalizer_state {
  FINALIZER_STATE_INIT = 0, // Finalizer is newborn.
  FINALIZER_STATE_ACTIVE,   // Finalizer is ours and in the finalizer table.
  FINALIZER_STATE_FIRED,    // Finalizer is handed back to mutator.
};

struct gc_finalizer {
  GC_EMBEDDER_FINALIZER_HEADER
  enum finalizer_state state;
  struct gc_ref object;
  struct gc_ref closure;
  struct gc_finalizer *next;
};

// Enough buckets to parallelize closure marking.  No need to look up a
// finalizer for a given object.
#define BUCKET_COUNT 32

struct gc_finalizer_table {
  size_t finalizer_count;
  struct gc_finalizer* buckets[BUCKET_COUNT];
};

struct gc_finalizer_state {
  gc_finalizer_callback have_finalizers;
  struct gc_finalizer *fired;
  size_t fired_this_cycle;
  size_t table_count;
  struct gc_finalizer_table tables[0];
};

// public
size_t gc_finalizer_size(void) { return sizeof(struct gc_finalizer); }
struct gc_ref gc_finalizer_object(struct gc_finalizer *f) { return f->object; }
struct gc_ref gc_finalizer_closure(struct gc_finalizer *f) { return f->closure; }

// internal
struct gc_finalizer_state* gc_make_finalizer_state(void) {
  size_t ntables = gc_finalizer_priority_count();
  size_t size = (sizeof(struct gc_finalizer_state) +
                 sizeof(struct gc_finalizer_table) * ntables);
  struct gc_finalizer_state *ret = malloc(size);
  if (!ret)
    return NULL;
  memset(ret, 0, size);
  ret->table_count = ntables;
  return ret;
}

static void finalizer_list_push(struct gc_finalizer **loc,
                                struct gc_finalizer *head) {
  struct gc_finalizer *tail = atomic_load_explicit(loc, memory_order_acquire);
  do {
    head->next = tail;
  } while (!atomic_compare_exchange_weak(loc, &tail, head));
}

static struct gc_finalizer* finalizer_list_pop(struct gc_finalizer **loc) {
  struct gc_finalizer *head = atomic_load_explicit(loc, memory_order_acquire);
  do {
    if (!head) return NULL;
  } while (!atomic_compare_exchange_weak(loc, &head, head->next));
  head->next = NULL;
  return head;
}

static void add_finalizer_to_table(struct gc_finalizer_table *table,
                                   struct gc_finalizer *f) {
  size_t count = atomic_fetch_add_explicit(&table->finalizer_count, 1,
                                           memory_order_relaxed);
  struct gc_finalizer **loc = &table->buckets[count % BUCKET_COUNT];
  finalizer_list_push(loc, f);
}

// internal
void gc_finalizer_init_internal(struct gc_finalizer *f,
                                struct gc_ref object,
                                struct gc_ref closure) {
  // Caller responsible for any write barrier, though really the
  // assumption is that the finalizer is younger than the key and the
  // value.
  if (f->state != FINALIZER_STATE_INIT)
    GC_CRASH();
  GC_ASSERT(gc_ref_is_null(f->object));
  f->object = object;
  f->closure = closure;
}

// internal
void gc_finalizer_attach_internal(struct gc_finalizer_state *state,
                                  struct gc_finalizer *f,
                                  unsigned priority) {
  // Caller responsible for any write barrier, though really the
  // assumption is that the finalizer is younger than the key and the
  // value.
  if (f->state != FINALIZER_STATE_INIT)
    GC_CRASH();
  if (gc_ref_is_null(f->object))
    GC_CRASH();

  f->state = FINALIZER_STATE_ACTIVE;

  GC_ASSERT(priority < state->table_count);
  add_finalizer_to_table(&state->tables[priority], f);
}

// internal
struct gc_finalizer* gc_finalizer_state_pop(struct gc_finalizer_state *state) {
  return finalizer_list_pop(&state->fired);
}

static void
add_fired_finalizer(struct gc_finalizer_state *state,
                    struct gc_finalizer *f) {
  if (f->state != FINALIZER_STATE_ACTIVE)
    GC_CRASH();
  f->state = FINALIZER_STATE_FIRED;
  finalizer_list_push(&state->fired, f);
}

// internal
void
gc_finalizer_externally_activated(struct gc_finalizer *f) {
  if (f->state != FINALIZER_STATE_INIT)
    GC_CRASH();
  f->state = FINALIZER_STATE_ACTIVE;
}

// internal
void
gc_finalizer_externally_fired(struct gc_finalizer_state *state,
                              struct gc_finalizer *f) {
  add_fired_finalizer(state, f);
}

// internal
size_t gc_visit_finalizer_roots(struct gc_finalizer_state *state,
                                void (*visit)(struct gc_edge,
                                              struct gc_heap*,
                                              void *),
                                struct gc_heap *heap,
                                void *visit_data) {
  size_t count = 0;
  for (size_t tidx = 0; tidx < state->table_count; tidx++) {
    struct gc_finalizer_table *table = &state->tables[tidx];
    if (table->finalizer_count) {
      count += table->finalizer_count;
      for (size_t bidx = 0; bidx < BUCKET_COUNT; bidx++)
        visit(gc_edge(&table->buckets[bidx]), heap, visit_data);
    }
  }
  visit(gc_edge(&state->fired), heap, visit_data);
  return count;
}

// public
void gc_trace_finalizer(struct gc_finalizer *f,
                        void (*visit)(struct gc_edge edge,
                                      struct gc_heap *heap,
                                      void *visit_data),
                        struct gc_heap *heap,
                        void *trace_data) {
  if (f->state != FINALIZER_STATE_ACTIVE)
    visit(gc_edge(&f->object), heap, trace_data);
  visit(gc_edge(&f->closure), heap, trace_data);
  visit(gc_edge(&f->next), heap, trace_data);
}

// Sweeping is currently serial.  It could run in parallel but we want to
// resolve all finalizers before shading any additional node.  Perhaps we should
// relax this restriction though; if the user attaches two finalizers to the
// same object, it's probably OK to only have one finalizer fire per cycle.

// internal
size_t gc_resolve_finalizers(struct gc_finalizer_state *state,
                             size_t priority,
                             void (*visit)(struct gc_edge edge,
                                           struct gc_heap *heap,
                                           void *visit_data),
                             struct gc_heap *heap,
                             void *visit_data) {
  GC_ASSERT(priority < state->table_count);
  struct gc_finalizer_table *table = &state->tables[priority];
  size_t finalizers_fired = 0;
  // Visit each finalizer in the table.  If its object was already visited,
  // re-add the finalizer to the table.  Otherwise enqueue its object edge for
  // tracing and mark the finalizer as fired.
  if (table->finalizer_count) {
    struct gc_finalizer_table scratch = { 0, };
    for (size_t bidx = 0; bidx < BUCKET_COUNT; bidx++) {
      struct gc_finalizer *next;
      for (struct gc_finalizer *f = table->buckets[bidx]; f; f = next) {
        next = f->next;
        f->next = NULL;
        struct gc_edge edge = gc_edge(&f->object);
        if (gc_visit_ephemeron_key(edge, heap)) {
          add_finalizer_to_table(&scratch, f);
        } else {
          finalizers_fired++;
          visit(edge, heap, visit_data);
          add_fired_finalizer(state, f);
        }
      }
    }
    memcpy(table, &scratch, sizeof(*table));
  }
  state->fired_this_cycle += finalizers_fired;
  return finalizers_fired;
}

// internal
void gc_notify_finalizers(struct gc_finalizer_state *state,
                          struct gc_heap *heap) {
  if (state->fired_this_cycle && state->have_finalizers) {
    state->have_finalizers(heap, state->fired_this_cycle);
    state->fired_this_cycle = 0;
  }
}

// internal
void gc_finalizer_state_set_callback(struct gc_finalizer_state *state,
                                     gc_finalizer_callback callback) {
  state->have_finalizers = callback;
}
