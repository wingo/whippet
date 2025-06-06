#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GC_IMPL 1

#include "gc-align.h"
#include "gc-api.h"
#include "gc-ephemeron.h"
#include "gc-trace.h"
#include "gc-tracepoint.h"

#include "gc-internal.h"

#include "bdw-attrs.h"

#if GC_PRECISE_ROOTS
#error bdw-gc is a conservative collector
#endif

#if !GC_CONSERVATIVE_ROOTS
#error bdw-gc is a conservative collector
#endif

#if !GC_CONSERVATIVE_TRACE
#error bdw-gc is a conservative collector
#endif

// When pthreads are used, let `libgc' know about it and redirect
// allocation calls such as `GC_MALLOC ()' to (contention-free, faster)
// thread-local allocation.

#define GC_THREADS 1
#define GC_REDIRECT_TO_LOCAL 1

// Don't #define pthread routines to their GC_pthread counterparts.
// Instead we will be careful inside the benchmarks to use API to
// register threads with libgc.
#define GC_NO_THREAD_REDIRECTS 1

#include <gc/gc.h>
#include <gc/gc_inline.h> /* GC_generic_malloc_many */
#include <gc/gc_mark.h> /* GC_generic_malloc */

#define GC_INLINE_GRANULE_WORDS 2
#define GC_INLINE_GRANULE_BYTES (sizeof(void *) * GC_INLINE_GRANULE_WORDS)

/* A freelist set contains GC_INLINE_FREELIST_COUNT pointers to singly
   linked lists of objects of different sizes, the ith one containing
   objects i + 1 granules in size.  This setting of
   GC_INLINE_FREELIST_COUNT will hold freelists for allocations of
   up to 256 bytes.  */
#define GC_INLINE_FREELIST_COUNT (256U / GC_INLINE_GRANULE_BYTES)

struct gc_heap {
  struct gc_heap_roots *roots;
  struct gc_mutator *mutators;
  struct gc_event_listener event_listener;
  struct gc_finalizer_state *finalizer_state;
  gc_finalizer_callback have_finalizers;
  void *event_listener_data;
  void* (*allocation_failure)(struct gc_heap *, size_t);
};

struct gc_mutator {
  void *freelists[GC_INLINE_FREELIST_COUNT];
  struct gc_heap *heap;
  struct gc_mutator_roots *roots;
  struct gc_mutator *next; // with global bdw lock
  struct gc_mutator **prev; // with global bdw lock
  void *event_listener_data;
};

struct gc_heap *__the_bdw_gc_heap;
#define HEAP_EVENT(event, ...) do {                                     \
    __the_bdw_gc_heap->event_listener.event(__the_bdw_gc_heap->event_listener_data, \
                                            ##__VA_ARGS__);             \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)
#define MUTATOR_EVENT(mut, event, ...) do {                             \
    __the_bdw_gc_heap->event_listener.event(mut->event_listener_data,   \
                                            ##__VA_ARGS__);             \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)
static inline size_t gc_inline_bytes_to_freelist_index(size_t bytes) {
  return (bytes - 1U) / GC_INLINE_GRANULE_BYTES;
}
static inline size_t gc_inline_freelist_object_size(size_t idx) {
  return (idx + 1U) * GC_INLINE_GRANULE_BYTES;
}

struct gc_heap* gc_mutator_heap(struct gc_mutator *mutator) {
  return __the_bdw_gc_heap;
}
uintptr_t gc_small_object_nursery_low_address(struct gc_heap *heap) {
  GC_CRASH();
}
uintptr_t gc_small_object_nursery_high_address(struct gc_heap *heap) {
  GC_CRASH();
}

// The values of these must match the internal POINTERLESS and NORMAL
// definitions in libgc, for which unfortunately there are no external
// definitions.  Alack.
enum gc_inline_kind {
  GC_INLINE_KIND_POINTERLESS,
  GC_INLINE_KIND_NORMAL
};

static inline void *
allocate_small(void **freelist, size_t idx, enum gc_inline_kind kind) {
  void *head = *freelist;

  if (!head) {
    size_t bytes = gc_inline_freelist_object_size(idx);
    GC_generic_malloc_many(bytes, kind, freelist);
    head = *freelist;
    if (GC_UNLIKELY (!head))
      return __the_bdw_gc_heap->allocation_failure(__the_bdw_gc_heap, bytes);
  }

  *freelist = *(void **)(head);
  *(void**)head = NULL;

  return head;
}

void* gc_allocate_slow(struct gc_mutator *mut, size_t size,
                       enum gc_allocation_kind kind) {
  GC_ASSERT(size != 0);
  if (size <= gc_allocator_large_threshold()) {
    switch (kind) {
      case GC_ALLOCATION_TAGGED:
      case GC_ALLOCATION_UNTAGGED_CONSERVATIVE: {
        size_t idx = gc_inline_bytes_to_freelist_index(size);
        return allocate_small(&mut->freelists[idx], idx, GC_INLINE_KIND_NORMAL);
      }
      case GC_ALLOCATION_TAGGED_POINTERLESS:
      case GC_ALLOCATION_UNTAGGED_POINTERLESS:
        break;
      default:
        GC_CRASH();
    }
  }
  switch (kind) {
    case GC_ALLOCATION_TAGGED:
    case GC_ALLOCATION_UNTAGGED_CONSERVATIVE: {
      void *ret = GC_malloc(size);
      if (GC_LIKELY (ret != NULL))
        return ret;
      return __the_bdw_gc_heap->allocation_failure(__the_bdw_gc_heap, size);
    }
    case GC_ALLOCATION_TAGGED_POINTERLESS:
    case GC_ALLOCATION_UNTAGGED_POINTERLESS: {
      void *ret = GC_malloc_atomic(size);
      if (GC_LIKELY (ret != NULL)) {
        memset(ret, 0, size);
        return ret;
      }
      return __the_bdw_gc_heap->allocation_failure(__the_bdw_gc_heap, size);
    }
    default:
      GC_CRASH();
  }
}

void gc_pin_object(struct gc_mutator *mut, struct gc_ref ref) {
  // Nothing to do.
}

struct gc_ref gc_resolve_conservative_ref(struct gc_heap *heap,
                                          struct gc_conservative_ref ref,
                                          int possibly_interior) {
  if (!gc_conservative_ref_might_be_a_heap_object(ref, possibly_interior))
    return gc_ref_null();

  uintptr_t start = align_down(gc_conservative_ref_value(ref),
                               GC_INLINE_GRANULE_BYTES);
  uintptr_t base = (uintptr_t)GC_base((void*)start);

  if (!base)
    return gc_ref_null();
  if (possibly_interior || start == base)
    return gc_ref(base);
  else
    return gc_ref_null();
}

void gc_collect(struct gc_mutator *mut,
                enum gc_collection_kind requested_kind) {
  switch (requested_kind) {
  case GC_COLLECTION_MINOR:
    GC_collect_a_little();
    break;
  case GC_COLLECTION_ANY:
  case GC_COLLECTION_MAJOR:
    GC_gcollect();
    break;
  case GC_COLLECTION_COMPACTING:
    GC_gcollect_and_unmap();
    break;
  default:
    GC_CRASH();
  }
}

int gc_heap_contains(struct gc_heap *heap, struct gc_ref ref) {
  GC_ASSERT(gc_ref_is_heap_object(ref));
  return GC_base(gc_ref_heap_object(ref)) != 0;
}

int gc_object_is_old_generation_slow(struct gc_mutator *mut,
                                     struct gc_ref obj) {
  return 0;
}

void gc_write_barrier_slow(struct gc_mutator *mut, struct gc_ref obj,
                           size_t obj_size, struct gc_edge edge,
                           struct gc_ref new_val) {
}

int* gc_safepoint_flag_loc(struct gc_mutator *mut) { GC_CRASH(); }
void gc_safepoint_slow(struct gc_mutator *mut) { GC_CRASH(); }

int gc_safepoint_signal_number(void) {
  return GC_get_suspend_signal();
}
void gc_safepoint_signal_inhibit(struct gc_mutator *mut) {
  GC_alloc_lock();
}
void gc_safepoint_signal_reallow(struct gc_mutator *mut) {
  GC_alloc_unlock();
}

struct bdw_mark_state {
  struct GC_ms_entry *mark_stack_ptr;
  struct GC_ms_entry *mark_stack_limit;
};

static void bdw_mark(struct gc_ref ref, struct gc_heap *heap,
                     void *visit_data) {
  struct bdw_mark_state *state = visit_data;
  state->mark_stack_ptr = GC_MARK_AND_PUSH ((void *) gc_ref_value(ref),
                                            state->mark_stack_ptr,
                                            state->mark_stack_limit,
                                            NULL);
}

static void bdw_mark_edge(struct gc_edge edge, struct gc_heap *heap,
                          void *visit_data) {
  bdw_mark(gc_edge_ref(edge), heap, visit_data);
}

static void bdw_mark_range(uintptr_t lo, uintptr_t hi, int possibly_interior,
                           struct gc_heap *heap, void *visit_data) {
  struct bdw_mark_state *state = visit_data;

  GC_ASSERT_EQ (lo, align_up (lo, sizeof(void*)));
  GC_ASSERT_EQ (hi, align_up (hi, sizeof(void*)));

  for (void **walk = (void**)lo, **end = (void**)hi; walk < end; walk++)
    state->mark_stack_ptr = GC_MARK_AND_PUSH (*walk,
                                              state->mark_stack_ptr,
                                              state->mark_stack_limit,
                                              NULL);
}

static int heap_gc_kind;
static int mutator_gc_kind;
static int ephemeron_gc_kind;
static int finalizer_gc_kind;

// In BDW-GC, we can't hook into the mark phase to call
// gc_trace_ephemerons_for_object, so the advertised ephemeron strategy
// doesn't really work.  The primitives that we have are mark functions,
// which run during GC and can't allocate; finalizers, which run after
// GC and can allocate but can't add to the connectivity graph; and
// disappearing links, which are cleared at the end of marking, in the
// stop-the-world phase.  It does not appear to be possible to implement
// ephemerons using these primitives.  Instead fall back to weak-key
// tables.

struct gc_ephemeron* gc_allocate_ephemeron(struct gc_mutator *mut) {
  return GC_generic_malloc(gc_ephemeron_size(), ephemeron_gc_kind);
}

unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return GC_get_gc_no();
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mut->heap, ephemeron, key, value);
  if (GC_base((void*)gc_ref_value(key))) {
    struct gc_ref *loc = gc_edge_loc(gc_ephemeron_key_edge(ephemeron));
    GC_general_register_disappearing_link((void**)loc,
                                          gc_ref_heap_object(key));
  }
}

struct gc_ref gc_ephemeron_swap_value(struct gc_mutator *mut,
                                      struct gc_ephemeron *e,
                                      struct gc_ref ref) {
  return gc_ephemeron_swap_value_internal(e, ref);
}

int gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  // Pretend the key is traced, to avoid adding this ephemeron to the
  // global table.
  return 1;
}

struct gc_finalizer* gc_allocate_finalizer(struct gc_mutator *mut) {
  return GC_generic_malloc(gc_finalizer_size(), finalizer_gc_kind);
}

static void finalize_object(void *obj, void *data) {
  struct gc_finalizer *f = data;
  gc_finalizer_externally_fired(__the_bdw_gc_heap->finalizer_state, f);
}

void gc_finalizer_attach(struct gc_mutator *mut, struct gc_finalizer *finalizer,
                         unsigned priority, struct gc_ref object,
                         struct gc_ref closure) {
  // Don't bother much about the actual finalizer; just delegate to BDW-GC.
  GC_finalization_proc prev = NULL;
  void *prev_data = NULL;
  gc_finalizer_init_internal(finalizer, object, closure);
  gc_finalizer_externally_activated(finalizer);
  GC_register_finalizer_no_order(gc_ref_heap_object(object), finalize_object,
                                 finalizer, &prev, &prev_data);
  // FIXME: Allow multiple finalizers per object.
  GC_ASSERT(prev == NULL);
  GC_ASSERT(prev_data == NULL);
}

struct gc_finalizer* gc_pop_finalizable(struct gc_mutator *mut) {
  GC_invoke_finalizers();
  return gc_finalizer_state_pop(mut->heap->finalizer_state);
}

void gc_set_finalizer_callback(struct gc_heap *heap,
                               gc_finalizer_callback callback) {
  heap->have_finalizers = callback;
}

static void have_finalizers(void) {
  struct gc_heap *heap = __the_bdw_gc_heap;
  if (heap->have_finalizers)
    heap->have_finalizers(heap, 1);
}

static struct GC_ms_entry *
mark_ephemeron(GC_word *addr, struct GC_ms_entry *mark_stack_ptr,
               struct GC_ms_entry *mark_stack_limit, GC_word env) {

  struct bdw_mark_state state = {
    mark_stack_ptr,
    mark_stack_limit,
  };
  
  struct gc_ephemeron *ephemeron = (struct gc_ephemeron*) addr;

  // If this ephemeron is on a freelist, its first word will be a possibly-null
  // freelist link and everything else will be NULL.
  if (!gc_ref_value(gc_edge_ref(gc_ephemeron_value_edge(ephemeron)))) {
    bdw_mark_edge(gc_edge(addr), NULL, &state);
    return state.mark_stack_ptr;
  }

  if (!gc_ref_value(gc_edge_ref(gc_ephemeron_key_edge(ephemeron)))) {
    // If the key died in a previous collection, the disappearing link
    // will have been cleared.  Mark the ephemeron as dead.
    gc_ephemeron_mark_dead(ephemeron);
  }

  gc_trace_ephemeron(ephemeron, bdw_mark_edge, NULL, &state);

  return state.mark_stack_ptr;
}

static struct GC_ms_entry *
mark_finalizer(GC_word *addr, struct GC_ms_entry *mark_stack_ptr,
               struct GC_ms_entry *mark_stack_limit, GC_word env) {

  struct bdw_mark_state state = {
    mark_stack_ptr,
    mark_stack_limit,
  };
  
  struct gc_finalizer *finalizer = (struct gc_finalizer*) addr;

  // If this ephemeron is on a freelist, its first word will be a possibly-null
  // freelist link and everything else will be NULL.
  if (!gc_ref_value(gc_finalizer_object(finalizer))) {
    bdw_mark_edge(gc_edge(addr), NULL, &state);
    return state.mark_stack_ptr;
  }

  gc_trace_finalizer(finalizer, bdw_mark_edge, NULL, &state);

  return state.mark_stack_ptr;
}

static struct GC_ms_entry *
mark_heap(GC_word *addr, struct GC_ms_entry *mark_stack_ptr,
          struct GC_ms_entry *mark_stack_limit, GC_word env) {
  struct bdw_mark_state state = {
    mark_stack_ptr,
    mark_stack_limit,
  };
  
  struct gc_heap *heap = (struct gc_heap*) addr;

  // If this heap is not __the_bdw_gc_heap, either it is on a freelist, or the
  // heap object is still under construction.  In either case, ignore it.
  if (heap != __the_bdw_gc_heap)
    return state.mark_stack_ptr;

  if (heap->roots) {
    gc_trace_heap_pinned_roots(heap->roots, bdw_mark, bdw_mark_range,
                               heap, &state);
    gc_trace_heap_roots(heap->roots, bdw_mark_edge, heap, &state);
  }

  gc_visit_finalizer_roots(heap->finalizer_state, bdw_mark_edge, heap, &state);

  state.mark_stack_ptr = GC_MARK_AND_PUSH (heap->mutators,
                                           state.mark_stack_ptr,
                                           state.mark_stack_limit,
                                           NULL);

  return state.mark_stack_ptr;
}

static struct GC_ms_entry *
mark_mutator(GC_word *addr, struct GC_ms_entry *mark_stack_ptr,
             struct GC_ms_entry *mark_stack_limit, GC_word env) {
  struct bdw_mark_state state = {
    mark_stack_ptr,
    mark_stack_limit,
  };
  
  struct gc_mutator *mut = (struct gc_mutator*) addr;

  // A mutator is valid and initialized if its "heap" member points to
  // __the_bdw_gc_heap.  Otherwise it could be on a freelist, in which case its
  // first word will be a possibly-null freelist link, or it could be under
  // construction, or it could be exited already.  In any case, mark the free
  // list link and finish.
  if (mut->heap != __the_bdw_gc_heap) {
    bdw_mark_edge(gc_edge(addr), NULL, &state);
    return state.mark_stack_ptr;
  }

  memset(mut->freelists, 0, sizeof(void*) * GC_INLINE_FREELIST_COUNT);

  if (mut->roots) {
    gc_trace_mutator_pinned_roots(mut->roots, bdw_mark, bdw_mark_range,
                                  mut->heap, &state);
    gc_trace_mutator_roots(mut->roots, bdw_mark_edge, mut->heap, &state);
  }

  state.mark_stack_ptr = GC_MARK_AND_PUSH (mut->next,
                                           state.mark_stack_ptr,
                                           state.mark_stack_limit,
                                           NULL);

  return state.mark_stack_ptr;
}

static inline struct gc_mutator *add_mutator(struct gc_heap *heap) {
  struct gc_mutator *ret =
    GC_generic_malloc(sizeof(struct gc_mutator), mutator_gc_kind);
  ret->event_listener_data =
    heap->event_listener.mutator_added(heap->event_listener_data);

  GC_alloc_lock();
  ret->next = heap->mutators;
  ret->prev = &heap->mutators;
  if (ret->next)
    ret->next->prev = &ret->next;
  heap->mutators = ret;
  ret->heap = heap;
  GC_alloc_unlock();

  return ret;
}

struct gc_options {
  struct gc_common_options common;
};
int gc_option_from_string(const char *str) {
  return gc_common_option_from_string(str);
}
struct gc_options* gc_allocate_options(void) {
  struct gc_options *ret = malloc(sizeof(struct gc_options));
  gc_init_common_options(&ret->common);
  return ret;
}
int gc_options_set_int(struct gc_options *options, int option, int value) {
  return gc_common_options_set_int(&options->common, option, value);
}
int gc_options_set_size(struct gc_options *options, int option,
                        size_t value) {
  return gc_common_options_set_size(&options->common, option, value);
}
int gc_options_set_double(struct gc_options *options, int option,
                          double value) {
  return gc_common_options_set_double(&options->common, option, value);
}
int gc_options_parse_and_set(struct gc_options *options, int option,
                             const char *value) {
  return gc_common_options_parse_and_set(&options->common, option, value);
}

struct gc_pending_ephemerons *
gc_heap_pending_ephemerons(struct gc_heap *heap) {
  GC_CRASH();
  return NULL;
}

static void on_collection_event(GC_EventType event) {
  switch (event) {
  case GC_EVENT_START: {
    HEAP_EVENT(requesting_stop);
    HEAP_EVENT(waiting_for_stop);
    break;
  }
  case GC_EVENT_MARK_START:
    HEAP_EVENT(mutators_stopped);
    HEAP_EVENT(prepare_gc, GC_COLLECTION_MAJOR, GC_get_total_bytes());
    break;
  case GC_EVENT_MARK_END:
    HEAP_EVENT(roots_traced);
    HEAP_EVENT(heap_traced);
    break;
  case GC_EVENT_RECLAIM_START:
    break;
  case GC_EVENT_RECLAIM_END:
    // Sloppily attribute finalizers and eager reclamation to
    // ephemerons.
    HEAP_EVENT(ephemerons_traced);
    // FIXME: This overestimates the live data size, as blocks that have at
    // least one live object will be lazily swept, and free space discovered in
    // those objects will be added to GC_bytes_found, which would need to be
    // subtracted from this value.
    HEAP_EVENT(live_data_size, GC_get_heap_size() - GC_get_free_bytes());
    break;
  case GC_EVENT_END:
    HEAP_EVENT(restarting_mutators);
    break;
  case GC_EVENT_PRE_START_WORLD:
  case GC_EVENT_POST_STOP_WORLD:
    // Can't rely on these, as they are only fired when threads are
    // enabled.
    break;
  case GC_EVENT_THREAD_SUSPENDED:
  case GC_EVENT_THREAD_UNSUSPENDED:
    // No nice way to map back to the mutator.
    break;
  default:
    break;
  }
}

static void on_heap_resize(GC_word size) {
  HEAP_EVENT(heap_resized, size);
}

uint64_t gc_allocation_counter(struct gc_heap *heap) {
  return GC_get_total_bytes();
}

static void* allocation_failure(struct gc_heap *heap, size_t size) {
  fprintf(stderr, "ran out of space, heap size %zu\n", GC_get_heap_size());
  GC_CRASH();
  return NULL;
}

static void* oom_fn(size_t nbytes) {
  return NULL;
}

static void warn_fn(char *fmt, GC_word arg) {
  /* FIXME: Do something better with this.  */
  fprintf (stderr, fmt, arg);
}

void gc_heap_set_allocation_failure_handler(struct gc_heap *heap,
                                            void* (*handler)(struct gc_heap*,
                                                             size_t)) {
  heap->allocation_failure = handler;
}

int gc_init(const struct gc_options *options, struct gc_stack_addr stack_base,
            struct gc_heap **heap, struct gc_mutator **mutator,
            struct gc_event_listener event_listener,
            void *event_listener_data) {
  // Root the heap, which will also cause all mutators to be marked.
  GC_ASSERT_EQ(gc_allocator_small_granule_size(), GC_INLINE_GRANULE_BYTES);
  GC_ASSERT_EQ(gc_allocator_large_threshold(),
               GC_INLINE_FREELIST_COUNT * GC_INLINE_GRANULE_BYTES);

  GC_ASSERT_EQ(__the_bdw_gc_heap, NULL);

  if (!options) options = gc_allocate_options();

  // Ignore stack base for main thread.

  switch (options->common.heap_size_policy) {
    case GC_HEAP_SIZE_FIXED:
      GC_set_max_heap_size(options->common.heap_size);
      break;
    case GC_HEAP_SIZE_GROWABLE: {
      if (options->common.maximum_heap_size)
        GC_set_max_heap_size(options->common.maximum_heap_size);
      // BDW uses a pretty weird heap-sizing heuristic:
      //
      // heap-size = live-data * (1 + (2 / GC_free_space_divisor))
      // heap-size-multiplier = heap-size/live-data = 1 + 2/GC_free_space_divisor
      // GC_free_space_divisor = 2/(heap-size-multiplier-1)
      //
      // (Assumption: your heap is mostly "composite", i.e. not
      // "atomic".  See bdw's alloc.c:min_bytes_allocd.)
      double fsd = 2.0/(options->common.heap_size_multiplier - 1);
      // But, the divisor is an integer.  WTF.  This caps the effective
      // maximum heap multiplier at 3.  Oh well.
      GC_set_free_space_divisor(fsd + 0.51);
      break;
    }
    case GC_HEAP_SIZE_ADAPTIVE:
    default:
      fprintf(stderr, "adaptive heap sizing unsupported by bdw-gc\n");
      return 0;
  }

  GC_set_all_interior_pointers (0);
  GC_set_finalize_on_demand (1);
  GC_set_finalizer_notifier(have_finalizers);

  // Not part of 7.3, sigh.  Have to set an env var.
  // GC_set_markers_count(options->common.parallelism);
  char markers[21] = {0,}; // 21 bytes enough for 2**64 in decimal + NUL.
  snprintf(markers, sizeof(markers), "%d", options->common.parallelism);
  setenv("GC_MARKERS", markers, 1);
  GC_init();
  GC_set_warn_proc (warn_fn);
  size_t current_heap_size = GC_get_heap_size();
  if (options->common.heap_size > current_heap_size)
    GC_expand_hp(options->common.heap_size - current_heap_size);
  GC_allow_register_threads();

  {
    int add_size_to_descriptor = 0;
    int clear_memory = 1;

    heap_gc_kind = GC_new_kind(GC_new_free_list(),
                               GC_MAKE_PROC(GC_new_proc(mark_heap), 0),
                               add_size_to_descriptor, clear_memory);
    mutator_gc_kind = GC_new_kind(GC_new_free_list(),
                                  GC_MAKE_PROC(GC_new_proc(mark_mutator), 0),
                                  add_size_to_descriptor, clear_memory);
    ephemeron_gc_kind = GC_new_kind(GC_new_free_list(),
                                    GC_MAKE_PROC(GC_new_proc(mark_ephemeron), 0),
                                    add_size_to_descriptor, clear_memory);
    finalizer_gc_kind = GC_new_kind(GC_new_free_list(),
                                    GC_MAKE_PROC(GC_new_proc(mark_finalizer), 0),
                                    add_size_to_descriptor, clear_memory);
  }

  *heap = GC_generic_malloc(sizeof(struct gc_heap), heap_gc_kind);

  (*heap)->event_listener = event_listener;
  (*heap)->event_listener_data = event_listener_data;
  (*heap)->finalizer_state = gc_make_finalizer_state();

  __the_bdw_gc_heap = *heap;
  HEAP_EVENT(init, GC_get_heap_size());
  GC_set_on_collection_event(on_collection_event);
  GC_set_on_heap_resize(on_heap_resize);
  GC_set_oom_fn (oom_fn);
  (*heap)->allocation_failure = allocation_failure;

  *mutator = add_mutator(*heap);

  // Sanity check.
  if (!GC_is_visible (&__the_bdw_gc_heap))
    abort ();

  return 1;
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr stack_base,
                                      struct gc_heap *heap) {
  struct GC_stack_base base = { gc_stack_addr_as_pointer (stack_base) };
  GC_register_my_thread(&base);
  return add_mutator(heap);
}
void gc_finish_for_thread(struct gc_mutator *mut) {
  GC_alloc_lock();
  MUTATOR_EVENT(mut, mutator_removed);
  *mut->prev = mut->next;
  if (mut->next)
    mut->next->prev = mut->prev;
  memset(mut, 0, sizeof(*mut));
  GC_alloc_unlock();

  GC_unregister_my_thread();
}

struct call_with_mutator_data {
  void* (*proc) (struct gc_mutator*, void*);
  struct gc_mutator *mutator;
  void *data;
};

static void* call_with_mutator (void *p) {
  struct call_with_mutator_data *data = p;
  return data->proc(data->mutator, data->data);
}

void gc_deactivate(struct gc_mutator *mut) {};
void gc_reactivate(struct gc_mutator *mut) {};

void* gc_deactivate_for_call(struct gc_mutator *mut,
                             void* (*f)(struct gc_mutator *, void*),
                             void *data) {
  struct call_with_mutator_data d = { f, mut, data };
  return GC_do_blocking(call_with_mutator, &d);
}

void* gc_reactivate_for_call(struct gc_mutator *mut,
                             void* (*f)(struct gc_mutator *, void*),
                             void *data) {
  struct call_with_mutator_data d = { f, mut, data };
  return GC_call_with_gc_active(call_with_mutator, &d);
}

void gc_mutator_set_roots(struct gc_mutator *mut,
                          struct gc_mutator_roots *roots) {
  mut->roots = roots;
}
void gc_heap_set_roots(struct gc_heap *heap, struct gc_heap_roots *roots) {
  heap->roots = roots;
}
void gc_heap_set_extern_space(struct gc_heap *heap,
                              struct gc_extern_space *space) {
}
