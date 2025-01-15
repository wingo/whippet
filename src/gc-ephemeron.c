#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#define GC_IMPL 1

#include "address-hash.h"
#include "debug.h"
#include "gc-embedder-api.h"
#include "gc-ephemeron-internal.h"

// # Overview
//
// An ephemeron is a conjunction consisting of the ephemeron object
// itself, a "key" object, and a "value" object.  If the ephemeron and
// the key are live, then the value is kept live and can be looked up
// given the ephemeron object.
//
// Sometimes we write this as E×K⇒V, indicating that you need both E and
// K to get V.  We'll use this notation in these comments sometimes.
//
// The key and the value of an ephemeron are never modified, except
// possibly via forwarding during GC.
//
// If the key of an ephemeron ever becomes unreachable, the ephemeron
// object will be marked as dead by the collector, and neither key nor
// value will be accessible.  Users can also explicitly mark an
// ephemeron as dead.
//
// Users can build collections of ephemerons by chaining them together.
// If an ephemeron ever becomes dead, the ephemeron will be removed from
// the chain by the garbage collector.
//
// # Tracing algorithm
//
// Tracing ephemerons is somewhat complicated.  Tracing the live objects
// in a heap is usually a parallelizable fan-out kind of operation,
// requiring minimal synchronization between tracing worker threads.
// However with ephemerons, each worker thread may need to check if
// there is a pending ephemeron E for an object K, marking the
// associated V for later traversal by the tracer.  Doing this without
// introducing excessive global serialization points is the motivation
// for the complications that follow.
//
// From the viewpoint of the garbage collector, an ephemeron E×K⇒V has 4
// possible states:
//
//  - Traced: An E that was already fully traced as of a given GC epoch.
//
//  - Claimed: GC discovers E for the first time in a GC epoch
//
//  - Pending: K's liveness is unknown
//
//  - Resolved: K is live; V needs tracing
//
// The ephemeron state is kept in an atomic variable.  The pending and
// resolved states also have associated atomic list link fields as well;
// it doesn't appear possible to coalesce them into a single field
// without introducing serialization.  Finally, there is a bit to
// indicate whether a "traced" ephemeron is live or dead, and a field to
// indicate the epoch at which it was last traced.
//
// Here is a diagram of the state transitions:
//
//               ,----->Traced<-----.
//              ,          |   |     .
//             ,           v   /      .
//             |        Claimed        |
//             |  ,-----/     \---.    |
//             |  v               v    |
//             Pending--------->Resolved
//
// Ephemerons are born in the traced state, for the current GC epoch.
//
// When the tracer sees an ephemeron E in the traced state it checks the
// epoch.  If the epoch is up to date, E stays in the traced state and
// we are done.
//
// Otherwise, E transitions from traced to claimed.  The thread that
// claims E is then responsible for resetting E's pending and resolved
// links, updating E's epoch, and tracing E's user-controlled chain
// link.
//
// If the claiming thread sees that E was already marked dead by a
// previous GC, or explicitly by the user, the ephemeron then
// transitions from back to traced, ready for the next epoch.
//
// If the claiming thread sees K to already be known to be live, then E
// is added to the global resolved set and E's state becomes resolved.
//
// Otherwise the claiming thread publishes K⇒E to the global pending
// ephemeron table, via the pending link, and E transitions to pending.
//
// A pending ephemeron is a link in a buckets-of-chains concurrent hash
// table.  If its K is ever determined to be live, it becomes resolved,
// and is added to a global set of resolved ephemerons.  At the end of
// GC, any ephemerons still pending are marked dead, transitioning their
// states to traced.
//
// Note that the claiming thread -- the one that publishes K⇒E to the
// global pending ephemeron table -- needs to re-check that K is still
// untraced after adding K⇒E to the pending table, and move to resolved
// if so.
//
// A resolved ephemeron needs its V to be traced.  Incidentally its K
// also needs tracing, to relocate any forwarding pointer.  The thread
// that pops an ephemeron from the resolved set is responsible for
// tracing and for moving E's state to traced.
//
// # Concurrency
//
// All operations on ephemerons are wait-free.  Sometimes only one
// thread can make progress (for example for an ephemeron in the claimed
// state), but no thread will be stalled waiting on other threads to
// proceed.
//
// There is one interesting (from a concurrency point of view) data
// structure used by the implementation of ephemerons, the singly-linked
// list.  Actually there are three of these; one is used as a stack and
// the other two is used as sets.
//
// The resolved set is implemented via a global `struct gc_ephemeron
// *resolved` variable.  Resolving an ephemeron does an atomic push to
// this stack, via compare-and-swap (CAS); popping from the stack (also
// via CAS) yields an ephemeron for tracing.  Ephemerons are added to
// the resolved set at most once per GC cycle, and the resolved set is
// empty outside of GC.
//
// The operations that are supported on atomic stacks are:
//
//   push(LOC, E, OFFSET) -> void
//
// The user-visible chain link and the link for the pending ephemeron
// table are used to build atomic sets.  In these you can add an
// ephemeron to the beginning of the list, traverse the list link by
// link to the end (indicated by NULL), and remove any list item.
// Removing a list node proceeds in two phases: one, you mark the node
// for removal, by changing the ephemeron's state; then, possibly on a
// subsequent traversal, any predecessor may forward its link past
// removed nodes.  Because node values never change and nodes only go
// from live to dead, the live list tail can always be reached by any
// node, even from dead nodes.
//
// The operations that are supported on these atomic lists:
//
//   push(LOC, E, OFFSET) -> void
//   pop(LOC, OFFSET) -> ephemeron or null
//   follow(LOC, OFFSET, STATE_OFFSET, LIVE_STATE) -> ephemeron or null
//
// These operations are all wait-free.  The "push" operation is shared
// between stack and set use cases.  "pop" is for stack-like use cases.
// The "follow" operation traverses a list, opportunistically eliding
// nodes that have been marked dead, atomically updating the location
// storing the next item.
//
// There are also accessors on ephemerons to their fields:
//
//   key(E) -> value or null
//   value(E) -> value or null
//
// These operations retrieve the key and value, respectively, provided
// that the ephemeron is not marked dead.

////////////////////////////////////////////////////////////////////////
// Concurrent operations on ephemeron lists
////////////////////////////////////////////////////////////////////////

static void
ephemeron_list_push(struct gc_ephemeron **loc,
                    struct gc_ephemeron *head,
                    struct gc_ephemeron** (*get_next)(struct gc_ephemeron*)) {
  struct gc_ephemeron *tail = atomic_load_explicit(loc, memory_order_acquire);
  while (1) {
    // There must be no concurrent readers of HEAD, a precondition that
    // we ensure by only publishing HEAD to LOC at most once per cycle.
    // Therefore we can use a normal store for the tail pointer.
    *get_next(head) = tail;
    if (atomic_compare_exchange_weak(loc, &tail, head))
      break;
  }
}

static struct gc_ephemeron*
ephemeron_list_pop(struct gc_ephemeron **loc,
                   struct gc_ephemeron** (*get_next)(struct gc_ephemeron*)) {
  struct gc_ephemeron *head = atomic_load_explicit(loc, memory_order_acquire);
  while (head) {
    // Precondition: the result of get_next on an ephemeron is never
    // updated concurrently; OK to load non-atomically.
    struct gc_ephemeron *tail = *get_next(head);
    if (atomic_compare_exchange_weak(loc, &head, tail))
      break;
  }
  return head;
}

static struct gc_ephemeron*
ephemeron_list_follow(struct gc_ephemeron **loc,
                      struct gc_ephemeron** (*get_next)(struct gc_ephemeron*),
                      int (*is_live)(struct gc_ephemeron*)) {
  struct gc_ephemeron *head = atomic_load_explicit(loc, memory_order_acquire);
  if (!head) return NULL;

  while (1) {
    struct gc_ephemeron *new_head = head;

    // Skip past any dead nodes.
    while (new_head && !is_live(new_head))
      new_head = atomic_load_explicit(get_next(new_head), memory_order_acquire);

    if (// If we didn't have to advance past any dead nodes, no need to
        // update LOC.
        (head == new_head)
        // Otherwise if we succeed in updating LOC, we're done.
        || atomic_compare_exchange_strong(loc, &head, new_head)
        // Someone else managed to advance LOC; that's fine too.
        || (head == new_head))
      return new_head;

    // Otherwise we lost a race; loop and retry.
  }
}

////////////////////////////////////////////////////////////////////////
// The ephemeron object type
////////////////////////////////////////////////////////////////////////

#ifndef GC_EMBEDDER_EPHEMERON_HEADER
#error Embedder should define GC_EMBEDDER_EPHEMERON_HEADER
#endif

enum {
  EPHEMERON_STATE_TRACED,
  EPHEMERON_STATE_CLAIMED,
  EPHEMERON_STATE_PENDING,
  EPHEMERON_STATE_RESOLVED,
};

struct gc_ephemeron {
  GC_EMBEDDER_EPHEMERON_HEADER
  uint8_t state;
  unsigned epoch;
  struct gc_ephemeron *chain;
  struct gc_ephemeron *pending;
  struct gc_ephemeron *resolved;
  struct gc_ref key;
  struct gc_ref value;
};

size_t gc_ephemeron_size(void) { return sizeof(struct gc_ephemeron); }

struct gc_edge gc_ephemeron_key_edge(struct gc_ephemeron *e) {
  return gc_edge(&e->key);
}
struct gc_edge gc_ephemeron_value_edge(struct gc_ephemeron *e) {
  return gc_edge(&e->value);
}

////////////////////////////////////////////////////////////////////////
// Operations on the user-controlled chain field
////////////////////////////////////////////////////////////////////////

static struct gc_ephemeron** ephemeron_chain(struct gc_ephemeron *e) {
  return &e->chain;
}
static int ephemeron_is_dead(struct gc_ephemeron *e) {
  return !atomic_load_explicit(&e->key.value, memory_order_acquire);
}
static int ephemeron_is_not_dead(struct gc_ephemeron *e) {
  return !ephemeron_is_dead(e);
}

void gc_ephemeron_chain_push(struct gc_ephemeron **loc,
                             struct gc_ephemeron *e) {
  ephemeron_list_push(loc, e, ephemeron_chain);
}  
static struct gc_ephemeron* follow_chain(struct gc_ephemeron **loc) {
  return ephemeron_list_follow(loc, ephemeron_chain, ephemeron_is_not_dead);
}  
struct gc_ephemeron* gc_ephemeron_chain_head(struct gc_ephemeron **loc) {
  return follow_chain(loc);
}
struct gc_ephemeron* gc_ephemeron_chain_next(struct gc_ephemeron *e) {
  return follow_chain(ephemeron_chain(e));
}
void gc_ephemeron_mark_dead(struct gc_ephemeron *e) {
  atomic_store_explicit(&e->key.value, 0, memory_order_release);
}

////////////////////////////////////////////////////////////////////////
// Operations on the GC-managed pending link
////////////////////////////////////////////////////////////////////////

static struct gc_ephemeron** ephemeron_pending(struct gc_ephemeron *e) {
  return &e->pending;
}
static uint8_t ephemeron_state(struct gc_ephemeron *e) {
  return atomic_load_explicit(&e->state, memory_order_acquire);
}
static int ephemeron_is_pending(struct gc_ephemeron *e) {
  return ephemeron_state(e) == EPHEMERON_STATE_PENDING;
}

static void push_pending(struct gc_ephemeron **loc, struct gc_ephemeron *e) {
  ephemeron_list_push(loc, e, ephemeron_pending);
}  
static struct gc_ephemeron* follow_pending(struct gc_ephemeron **loc) {
  return ephemeron_list_follow(loc, ephemeron_pending, ephemeron_is_pending);
}  

////////////////////////////////////////////////////////////////////////
// Operations on the GC-managed resolved link
////////////////////////////////////////////////////////////////////////

static struct gc_ephemeron** ephemeron_resolved(struct gc_ephemeron *e) {
  return &e->resolved;
}
static void push_resolved(struct gc_ephemeron **loc, struct gc_ephemeron *e) {
  ephemeron_list_push(loc, e, ephemeron_resolved);
}  
static struct gc_ephemeron* pop_resolved(struct gc_ephemeron **loc) {
  return ephemeron_list_pop(loc, ephemeron_resolved);
}  

////////////////////////////////////////////////////////////////////////
// Access to the association
////////////////////////////////////////////////////////////////////////

struct gc_ref gc_ephemeron_key(struct gc_ephemeron *e) {
  return gc_ref(atomic_load_explicit(&e->key.value, memory_order_acquire));
}

struct gc_ref gc_ephemeron_value(struct gc_ephemeron *e) {
  return ephemeron_is_dead(e) ? gc_ref_null() : e->value;
}

////////////////////////////////////////////////////////////////////////
// Tracing ephemerons
////////////////////////////////////////////////////////////////////////

struct gc_pending_ephemerons {
  struct gc_ephemeron* resolved;
  size_t nbuckets;
  double scale;
  struct gc_ephemeron* buckets[0];
};

static const size_t MIN_PENDING_EPHEMERONS_SIZE = 32;

static size_t pending_ephemerons_byte_size(size_t nbuckets) {
  return sizeof(struct gc_pending_ephemerons) +
    sizeof(struct gc_ephemeron*) * nbuckets;
}

static struct gc_pending_ephemerons*
gc_make_pending_ephemerons(size_t byte_size) {
  size_t nbuckets = byte_size / sizeof(struct gc_ephemeron*);
  if (nbuckets < MIN_PENDING_EPHEMERONS_SIZE)
    nbuckets = MIN_PENDING_EPHEMERONS_SIZE;

  struct gc_pending_ephemerons *ret =
    malloc(pending_ephemerons_byte_size(nbuckets));
  if (!ret)
    return NULL;

  ret->resolved = NULL;
  ret->nbuckets = nbuckets;
  ret->scale = nbuckets / pow(2.0, sizeof(uintptr_t) * 8);
  for (size_t i = 0; i < nbuckets; i++)
    ret->buckets[i] = NULL;

  return ret;
}

struct gc_pending_ephemerons*
gc_prepare_pending_ephemerons(struct gc_pending_ephemerons *state,
                              size_t target_byte_size, double slop) {
  size_t existing =
    state ? pending_ephemerons_byte_size(state->nbuckets) : 0;
  slop += 1.0;
  if (existing * slop > target_byte_size && existing < target_byte_size * slop)
    return state;

  struct gc_pending_ephemerons *new_state =
    gc_make_pending_ephemerons(target_byte_size);

  if (!new_state)
    return state;

  free(state);
  return new_state;
}

static struct gc_ephemeron**
pending_ephemeron_bucket(struct gc_pending_ephemerons *state,
                         struct gc_ref ref) {
  uintptr_t hash = hash_address(gc_ref_value(ref));
  size_t idx = hash * state->scale;
  GC_ASSERT(idx < state->nbuckets);
  return &state->buckets[idx];
}

static void
add_pending_ephemeron(struct gc_pending_ephemerons *state,
                      struct gc_ephemeron *e) {
  struct gc_ephemeron **bucket = pending_ephemeron_bucket(state, e->key);
  atomic_store_explicit(&e->state, EPHEMERON_STATE_PENDING,
                        memory_order_release);
  push_pending(bucket, e);
}

static void maybe_resolve_ephemeron(struct gc_pending_ephemerons *state,
                                    struct gc_ephemeron *e) {
  uint8_t expected = EPHEMERON_STATE_PENDING;
  if (atomic_compare_exchange_strong(&e->state, &expected,
                                     EPHEMERON_STATE_RESOLVED))
    push_resolved(&state->resolved, e);
}

// Precondition: OBJ has already been copied to tospace, but OBJ is a
// fromspace ref.
void gc_resolve_pending_ephemerons(struct gc_ref obj, struct gc_heap *heap) {
  struct gc_pending_ephemerons *state = gc_heap_pending_ephemerons(heap);
  struct gc_ephemeron **bucket = pending_ephemeron_bucket(state, obj);
  for (struct gc_ephemeron *link = follow_pending(bucket);
       link;
       link = follow_pending(&link->pending)) {
    if (gc_ref_value(obj) == gc_ref_value(link->key)) {
      gc_visit_ephemeron_key(gc_ephemeron_key_edge(link), heap);
      // PENDING -> RESOLVED, if it was pending.
      maybe_resolve_ephemeron(state, link);
    }
  }
}

void gc_trace_ephemeron(struct gc_ephemeron *e,
                        void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                      void *visit_data),
                        struct gc_heap *heap,
                        void *trace_data) {
  unsigned epoch = gc_heap_ephemeron_trace_epoch(heap);
  uint8_t expected = EPHEMERON_STATE_TRACED;
  // TRACED[_] -> CLAIMED[_].
  if (!atomic_compare_exchange_strong(&e->state, &expected,
                                      EPHEMERON_STATE_CLAIMED))
    return;


  if (e->epoch == epoch) {
    // CLAIMED[epoch] -> TRACED[epoch].
    atomic_store_explicit(&e->state, EPHEMERON_STATE_TRACED,
                          memory_order_release);
    return;
  }

  // CLAIMED[!epoch] -> CLAIMED[epoch].
  e->epoch = epoch;
  e->pending = NULL;
  e->resolved = NULL;

  // Trace chain successors, eliding any intermediate dead links.  Note
  // that there is a race between trace-time evacuation of the next link
  // in the chain and any mutation of that link pointer by the mutator
  // (which can only be to advance the chain forward past dead links).
  // Collectors using this API have to eliminate this race, for example
  // by not evacuating while the mutator is running.
  follow_chain(&e->chain);
  visit(gc_edge(&e->chain), heap, trace_data);

  // Similarly there is a race between the mutator marking an ephemeron
  // as dead and here; the consequence would be that we treat an
  // ephemeron as live when it's not, but only for this cycle.  No big
  // deal.
  if (ephemeron_is_dead(e)) {
    // CLAIMED[epoch] -> TRACED[epoch].
    atomic_store_explicit(&e->state, EPHEMERON_STATE_TRACED,
                          memory_order_release);
    return;
  }
    
  // If K is live, trace V and we are done.
  if (gc_visit_ephemeron_key(gc_ephemeron_key_edge(e), heap)) {
    visit(gc_ephemeron_value_edge(e), heap, trace_data);
    // CLAIMED[epoch] -> TRACED[epoch].
    atomic_store_explicit(&e->state, EPHEMERON_STATE_TRACED,
                          memory_order_release);
    return;
  }

  // Otherwise K is not yet traced, so we don't know if it is live.
  // Publish the ephemeron to a global table.
  struct gc_pending_ephemerons *state = gc_heap_pending_ephemerons(heap);
  // CLAIMED[epoch] -> PENDING.
  add_pending_ephemeron(state, e);

  // Given an ephemeron E×K⇒V, there is a race between marking K and E.
  // One thread could go to mark E and see that K is unmarked, so we get
  // here.  Meanwhile another thread could go to mark K and not see E in
  // the global table yet.  Therefore after publishing E, we have to
  // check the mark on K again.
  if (gc_visit_ephemeron_key(gc_ephemeron_key_edge(e), heap))
    // K visited by another thread while we published E; PENDING ->
    // RESOLVED, if still PENDING.
    maybe_resolve_ephemeron(state, e);
}

void
gc_scan_pending_ephemerons(struct gc_pending_ephemerons *state,
                           struct gc_heap *heap, size_t shard,
                           size_t nshards) {
  GC_ASSERT(shard < nshards);
  size_t start = state->nbuckets * 1.0 * shard / nshards;
  size_t end = state->nbuckets * 1.0 * (shard + 1) / nshards;
  for (size_t idx = start; idx < end; idx++) {
    for (struct gc_ephemeron *e = follow_pending(&state->buckets[idx]);
         e;
         e = follow_pending(&e->pending)) {
      if (gc_visit_ephemeron_key(gc_ephemeron_key_edge(e), heap))
        // PENDING -> RESOLVED, if PENDING.
        maybe_resolve_ephemeron(state, e);
    }
  }
}

struct gc_ephemeron*
gc_pop_resolved_ephemerons(struct gc_heap *heap) {
  struct gc_pending_ephemerons *state = gc_heap_pending_ephemerons(heap);
  return atomic_exchange(&state->resolved, NULL);
}    

void
gc_trace_resolved_ephemerons(struct gc_ephemeron *resolved,
                             void (*visit)(struct gc_edge edge,
                                           struct gc_heap *heap,
                                           void *visit_data),
                             struct gc_heap *heap,
                             void *trace_data) {
  for (; resolved; resolved = resolved->resolved) {
    visit(gc_ephemeron_value_edge(resolved), heap, trace_data);
    // RESOLVED -> TRACED.
    atomic_store_explicit(&resolved->state, EPHEMERON_STATE_TRACED,
                          memory_order_release);
  }
}    

void
gc_sweep_pending_ephemerons(struct gc_pending_ephemerons *state,
                            size_t shard, size_t nshards) {
  GC_ASSERT(shard < nshards);
  size_t start = state->nbuckets * 1.0 * shard / nshards;
  size_t end = state->nbuckets * 1.0 * (shard + 1) / nshards;
  for (size_t idx = start; idx < end; idx++) {
    struct gc_ephemeron **bucket = &state->buckets[idx];
    for (struct gc_ephemeron *e = follow_pending(bucket);
         e;
         e = follow_pending(&e->pending)) {
      // PENDING -> TRACED, but dead.
      atomic_store_explicit(&e->key.value, 0, memory_order_release);
      atomic_store_explicit(&e->state, EPHEMERON_STATE_TRACED,
                            memory_order_release);
    }
    atomic_store_explicit(bucket, NULL, memory_order_release);
  }
}

////////////////////////////////////////////////////////////////////////
// Allocation & initialization
////////////////////////////////////////////////////////////////////////

void gc_ephemeron_init_internal(struct gc_heap *heap,
                                struct gc_ephemeron *ephemeron,
                                struct gc_ref key, struct gc_ref value) {
  // Caller responsible for any write barrier, though really the
  // assumption is that the ephemeron is younger than the key and the
  // value.
  ephemeron->state = EPHEMERON_STATE_TRACED;
  ephemeron->epoch = gc_heap_ephemeron_trace_epoch(heap) - 1;
  ephemeron->chain = NULL;
  ephemeron->pending = NULL;
  ephemeron->resolved = NULL;
  ephemeron->key = key;
  ephemeron->value = value;
}
