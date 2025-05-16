#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "background-thread.h"
#include "copy-space.h"
#include "debug.h"
#include "field-set.h"
#include "gc-align.h"
#include "gc-inline.h"
#include "gc-platform.h"
#include "gc-trace.h"
#include "gc-tracepoint.h"
#include "heap-sizer.h"
#include "large-object-space.h"
#if GC_PARALLEL
#include "parallel-tracer.h"
#else
#include "serial-tracer.h"
#endif
#include "spin.h"
#include "pcc-attrs.h"

struct gc_heap {
#if GC_GENERATIONAL
  struct copy_space new_space;
  struct copy_space old_space;
#else
  struct copy_space mono_space;
#endif
  struct large_object_space large_object_space;
  struct gc_extern_space *extern_space;
#if GC_GENERATIONAL
  struct gc_field_set remembered_set;
#endif
  size_t large_object_pages;
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  size_t size;
  size_t total_allocated_bytes_at_last_gc;
  int collecting;
#if GC_GENERATIONAL
  int is_minor_collection;
  size_t per_processor_nursery_size;
  size_t nursery_size;
#endif
  size_t processor_count;
  size_t max_active_mutator_count;
  int check_pending_ephemerons;
#if GC_GENERATIONAL
  struct gc_pending_ephemerons *nursery_pending_ephemerons;
#endif
  struct gc_pending_ephemerons *pending_ephemerons;
  struct gc_finalizer_state *finalizer_state;
  size_t mutator_count;
  size_t paused_mutator_count;
  size_t inactive_mutator_count;
  struct gc_heap_roots *roots;
  struct gc_mutator *mutators;
  long count;
  struct gc_tracer tracer;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
  struct gc_background_thread *background_thread;
  struct gc_heap_sizer sizer;
  struct gc_event_listener event_listener;
  void *event_listener_data;
  void* (*allocation_failure)(struct gc_heap *, size_t);
};

#define HEAP_EVENT(heap, event, ...) do {                               \
    (heap)->event_listener.event((heap)->event_listener_data, ##__VA_ARGS__); \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)
#define MUTATOR_EVENT(mut, event, ...) do {                             \
    (mut)->heap->event_listener.event((mut)->event_listener_data,       \
                                      ##__VA_ARGS__);                   \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)

struct gc_mutator {
  struct copy_space_allocator allocator;
#if GC_GENERATIONAL
  struct gc_field_set_writer logger;
#endif
  struct gc_heap *heap;
  struct gc_mutator_roots *roots;
  void *event_listener_data;
  struct gc_mutator *next;
  struct gc_mutator *prev;
  int active;
};

struct gc_trace_worker_data {
#if GC_GENERATIONAL
  struct copy_space_allocator new_allocator;
  struct copy_space_allocator old_allocator;
  struct gc_field_set_writer logger;
#else
  struct copy_space_allocator allocator;
#endif
};

static inline struct copy_space* heap_mono_space(struct gc_heap *heap) {
#if GC_GENERATIONAL
  GC_CRASH();
#else
  return &heap->mono_space;
#endif
}

static inline struct copy_space* heap_new_space(struct gc_heap *heap) {
#if GC_GENERATIONAL
  return &heap->new_space;
#else
  GC_CRASH();
#endif
}

static inline struct copy_space* heap_old_space(struct gc_heap *heap) {
#if GC_GENERATIONAL
  return &heap->old_space;
#else
  GC_CRASH();
#endif
}

static inline struct gc_field_set* heap_remembered_set(struct gc_heap *heap) {
#if GC_GENERATIONAL
  return &heap->remembered_set;
#else
  GC_CRASH();
#endif
}

static inline struct copy_space_allocator*
trace_worker_mono_space_allocator(struct gc_trace_worker_data *data) {
#if GC_GENERATIONAL
  GC_CRASH();
#else
  return &data->allocator;
#endif
}

static inline struct copy_space_allocator*
trace_worker_new_space_allocator(struct gc_trace_worker_data *data) {
#if GC_GENERATIONAL
  return &data->new_allocator;
#else
  GC_CRASH();
#endif
}

static inline struct copy_space_allocator*
trace_worker_old_space_allocator(struct gc_trace_worker_data *data) {
#if GC_GENERATIONAL
  return &data->old_allocator;
#else
  GC_CRASH();
#endif
}

static inline struct gc_field_set_writer*
trace_worker_field_logger(struct gc_trace_worker_data *data) {
#if GC_GENERATIONAL
  return &data->logger;
#else
  GC_CRASH();
#endif
}

static inline struct gc_field_set_writer*
mutator_field_logger(struct gc_mutator *mut) {
#if GC_GENERATIONAL
  return &mut->logger;
#else
  GC_CRASH();
#endif
}

static int is_minor_collection(struct gc_heap *heap) {
#if GC_GENERATIONAL
  return heap->is_minor_collection;
#else
  GC_CRASH();
#endif
}

static inline struct copy_space* heap_allocation_space(struct gc_heap *heap) {
  return GC_GENERATIONAL ? heap_new_space(heap) : heap_mono_space(heap);
}

static inline struct copy_space* heap_resizable_space(struct gc_heap *heap) {
  return GC_GENERATIONAL ? heap_old_space(heap) : heap_mono_space(heap);
}

static inline struct large_object_space* heap_large_object_space(struct gc_heap *heap) {
  return &heap->large_object_space;
}

static inline struct gc_extern_space* heap_extern_space(struct gc_heap *heap) {
  return heap->extern_space;
}

static inline struct gc_heap* mutator_heap(struct gc_mutator *mutator) {
  return mutator->heap;
}

struct gc_heap* gc_mutator_heap(struct gc_mutator *mutator) {
  return mutator_heap(mutator);
}

uintptr_t gc_small_object_nursery_low_address(struct gc_heap *heap) {
  if (GC_GENERATIONAL)
    return copy_space_low_aligned_address(heap_new_space(heap));
  GC_CRASH();
}
uintptr_t gc_small_object_nursery_high_address(struct gc_heap *heap) {
  if (GC_GENERATIONAL)
    return copy_space_high_aligned_address(heap_new_space(heap));
  GC_CRASH();
}

static void
gc_trace_worker_call_with_data(void (*f)(struct gc_tracer *tracer,
                                         struct gc_heap *heap,
                                         struct gc_trace_worker *worker,
                                         struct gc_trace_worker_data *data),
                               struct gc_tracer *tracer,
                               struct gc_heap *heap,
                               struct gc_trace_worker *worker) {
  struct gc_trace_worker_data data;

  if (GC_GENERATIONAL) {
    copy_space_allocator_init(trace_worker_new_space_allocator(&data));
    copy_space_allocator_init(trace_worker_old_space_allocator(&data));
    gc_field_set_writer_init(trace_worker_field_logger(&data),
                             heap_remembered_set(heap));
  } else {
    copy_space_allocator_init(trace_worker_mono_space_allocator(&data));
  }

  f(tracer, heap, worker, &data);

  if (GC_GENERATIONAL) {
    copy_space_allocator_finish(trace_worker_new_space_allocator(&data),
                                heap_new_space(heap));
    copy_space_allocator_finish(trace_worker_old_space_allocator(&data),
                                heap_old_space(heap));
    gc_field_set_writer_release_buffer(trace_worker_field_logger(&data));
  } else {
    copy_space_allocator_finish(trace_worker_mono_space_allocator(&data),
                                heap_mono_space(heap));
  }
}

static int new_space_contains_addr(struct gc_heap *heap, uintptr_t addr) {
  return copy_space_contains_address_aligned(heap_new_space(heap), addr);
}

static int new_space_contains(struct gc_heap *heap, struct gc_ref ref) {
  return new_space_contains_addr(heap, gc_ref_value(ref));
}

static int old_space_contains(struct gc_heap *heap, struct gc_ref ref) {
  return copy_space_contains(heap_old_space(heap), ref);
}

static int remember_edge_to_survivor_object(struct gc_heap *heap,
                                            struct gc_edge edge) {
  GC_ASSERT(!new_space_contains_addr(heap, gc_edge_address(edge)));
  GC_ASSERT(new_space_contains(heap, gc_edge_ref(edge)));
  if (copy_space_contains_edge(heap_old_space(heap), edge))
    return copy_space_remember_edge(heap_old_space(heap), edge);
  struct gc_ref large_object =
    large_object_space_object_containing_edge(heap_large_object_space(heap),
                                              edge);
  if (!gc_ref_is_null(large_object))
    return large_object_space_remember_edge(heap_large_object_space(heap),
                                            large_object, edge);
  return 0;
}

static inline int edge_is_from_survivor(struct gc_heap *heap,
                                        struct gc_edge edge) {
  // Currently only the copy-space has survivors.  (A survivor is a live object
  // which stays in the nursery after collection).  If lospace gains a survivor
  // stage, we would need to augment this check.
  GC_ASSERT(is_minor_collection(heap));
  return copy_space_contains_edge_aligned(heap_new_space(heap), edge);
}

static inline int forward(struct copy_space *src_space,
                          struct copy_space *dst_space,
                          struct gc_edge edge,
                          struct gc_ref ref,
                          struct copy_space_allocator *dst_alloc) {
  switch (copy_space_forward(src_space, dst_space, edge, ref, dst_alloc)) {
  case COPY_SPACE_FORWARD_UPDATED:
    return 0;
  case COPY_SPACE_FORWARD_EVACUATED:
    return 1;
  case COPY_SPACE_FORWARD_FAILED:
    // If space is really tight and reordering of objects during evacuation
    // resulted in more end-of-block fragmentation and thus block use than
    // before collection started, we can actually run out of memory while
    // collecting.  We should probably attempt to expand the heap here, at
    // least by a single block; it's better than the alternatives.  For now,
    // abort.
    fprintf(stderr, "Out of memory\n");
    GC_CRASH();
    break;
  default:
    GC_CRASH();
  }
}

static inline int do_minor_trace(struct gc_heap *heap, struct gc_edge edge,
                                 struct gc_ref ref,
                                 struct gc_trace_worker_data *data) {
  // Trace EDGE for a minor GC.  We only need to trace edges to young objects.
  // Young objects are either in the nursery copy space, or in the large object
  // space.

  if (GC_LIKELY(new_space_contains(heap, ref))) {
    struct copy_space *new_space = heap_new_space(heap);
    struct copy_space *old_space = heap_old_space(heap);
    // We are visiting an edge into newspace.  Either the edge's target will be
    // promoted to oldspace, or it will stay in newspace as a survivor.
    //
    // After the scavenge, we need to preserve the invariant that all old-to-new
    // edges are part of the remembered set.  So depending on where the edge
    // comes from and where the object moves to, we may need to add or remove
    // the edge from the remembered set.  Concretely:
    //
    //                   |  survivor dst    |  promoted dst
    //   ----------------+------------------+-----------------
    //   survivor src    |    nothing       |    nothing
    //                   |                  |
    //   promoted src    |    log edge      |    nothing
    //                   |                  |
    //   oldspace src    |    nothing       |   clear log
    //                   |                  |
    //   root src        |    nothing       |    nothing
    //
    // However, clearing a logged field usually isn't possible, as it's not easy
    // to go from field address to position in a field set, so instead we lazily
    // remove old->old edges from the field set during the next minor GC.  (Or,
    // we will anyway; for now we ignore them.)  So really we only need to log
    // promoted-to-survivor edges.
    //
    // However however, it is hard to distinguish between edges from promoted
    // objects and edges from old objects, so we mostly just rely on an
    // idempotent "log if unlogged" operation instead.
    if (!copy_space_should_promote(new_space, ref)) {
      // Try to leave the object in newspace as a survivor.  If the edge is from
      // a promoted object, we will need to add it to the remembered set.
      if (!edge_is_from_survivor(heap, edge)
          && remember_edge_to_survivor_object(heap, edge)) {
        // Log the edge even though in rare conditions the referent could end up
        // being promoted by us (if we run out of newspace) or a remote
        // evacuation thread (if they run out of newspace).
        gc_field_set_writer_add_edge(trace_worker_field_logger(data), edge);
      }
      switch (copy_space_forward(new_space, new_space, edge, ref,
                                 trace_worker_new_space_allocator(data))) {
      case COPY_SPACE_FORWARD_UPDATED:
        return 0;
      case COPY_SPACE_FORWARD_EVACUATED:
        return 1;
      case COPY_SPACE_FORWARD_FAILED:
        // Ran out of newspace!  Fall through to promote instead.
        break;
      default:
        GC_CRASH();
      }
    }
    // Promote the object.
    return forward(new_space, old_space, edge, ref,
                   trace_worker_old_space_allocator(data));
  } else {
    // Note that although the target of the edge might not be in lospace, this
    // will do what we want and return 1 if and only if ref is was a young
    // object in lospace.
    return large_object_space_mark(heap_large_object_space(heap), ref);
  }
}


static inline int do_trace(struct gc_heap *heap, struct gc_edge edge,
                           struct gc_ref ref,
                           struct gc_trace_worker_data *data) {
  if (GC_GENERATIONAL) {
    if (GC_LIKELY(is_minor_collection(heap)))
      return do_minor_trace(heap, edge, ref, data);

    // Major trace: promote all copyspace objects to oldgen.
    struct copy_space *new_space = heap_new_space(heap);
    struct copy_space *old_space = heap_old_space(heap);
    if (new_space_contains(heap, ref))
      return forward(new_space, old_space, edge, ref,
                     trace_worker_old_space_allocator(data));
    if (old_space_contains(heap, ref))
      return forward(old_space, old_space, edge, ref,
                     trace_worker_old_space_allocator(data));
  } else {
    if (GC_LIKELY(copy_space_contains(heap_mono_space(heap), ref)))
      return forward(heap_mono_space(heap), heap_mono_space(heap),
                     edge, ref,
                     trace_worker_mono_space_allocator(data));
  }

  // Fall through for objects in large or extern spaces.
  if (large_object_space_contains_with_lock(heap_large_object_space(heap), ref))
    return large_object_space_mark(heap_large_object_space(heap), ref);
  else
    return gc_extern_space_visit(heap_extern_space(heap), edge, ref);
}

static inline int trace_edge(struct gc_heap *heap, struct gc_edge edge,
                             struct gc_trace_worker *worker) {
  struct gc_ref ref = gc_edge_ref(edge);
  if (gc_ref_is_null(ref) || gc_ref_is_immediate(ref))
    return 0;
  struct gc_trace_worker_data *data = gc_trace_worker_data(worker);
  int is_new = do_trace(heap, edge, ref, data);

  if (is_new &&
      GC_UNLIKELY(atomic_load_explicit(&heap->check_pending_ephemerons,
                                       memory_order_relaxed)))
    gc_resolve_pending_ephemerons(ref, heap);

  return is_new;
}

int gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  GC_ASSERT(!gc_ref_is_null(ref));
  if (gc_ref_is_immediate(ref))
    return 1;
  GC_ASSERT(gc_ref_is_heap_object(ref));

  if (GC_GENERATIONAL) {
    if (new_space_contains(heap, ref))
      return copy_space_forward_if_traced(heap_new_space(heap), edge, ref);
    if (old_space_contains(heap, ref))
      return is_minor_collection(heap) ||
        copy_space_forward_if_traced(heap_old_space(heap), edge, ref);
  } else {
    if (copy_space_contains(heap_mono_space(heap), ref))
      return copy_space_forward_if_traced(heap_mono_space(heap), edge, ref);
  }

  if (large_object_space_contains_with_lock(heap_large_object_space(heap), ref))
    return large_object_space_is_marked(heap_large_object_space(heap), ref);
  GC_CRASH();
}

static int mutators_are_stopping(struct gc_heap *heap) {
  return atomic_load_explicit(&heap->collecting, memory_order_relaxed);
}

static inline void heap_lock(struct gc_heap *heap) {
  pthread_mutex_lock(&heap->lock);
}
static inline void heap_unlock(struct gc_heap *heap) {
  pthread_mutex_unlock(&heap->lock);
}

// with heap lock
static inline int all_mutators_stopped(struct gc_heap *heap) {
  return heap->mutator_count ==
    heap->paused_mutator_count + heap->inactive_mutator_count;
}

// with heap lock
static void maybe_increase_max_active_mutator_count(struct gc_heap *heap) {
  size_t active_mutators = heap->mutator_count - heap->inactive_mutator_count;
  if (active_mutators > heap->max_active_mutator_count)
    heap->max_active_mutator_count = active_mutators;
}

static void add_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  mut->heap = heap;
  mut->event_listener_data =
    heap->event_listener.mutator_added(heap->event_listener_data);
  copy_space_allocator_init(&mut->allocator);
  if (GC_GENERATIONAL)
    gc_field_set_writer_init(mutator_field_logger(mut),
                             heap_remembered_set(heap));
  heap_lock(heap);
  // We have no roots.  If there is a GC currently in progress, we have
  // nothing to add.  Just wait until it's done.
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  mut->next = mut->prev = NULL;
  mut->active = 1;
  struct gc_mutator *tail = heap->mutators;
  if (tail) {
    mut->next = tail;
    tail->prev = mut;
  }
  heap->mutators = mut;
  heap->mutator_count++;
  maybe_increase_max_active_mutator_count(heap);
  heap_unlock(heap);
}

static void remove_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  copy_space_allocator_finish(&mut->allocator, heap_allocation_space(heap));
  if (GC_GENERATIONAL)
    gc_field_set_writer_release_buffer(mutator_field_logger(mut));
  MUTATOR_EVENT(mut, mutator_removed);
  mut->heap = NULL;
  heap_lock(heap);
  heap->mutator_count--;
  mut->active = 0;
  if (mut->next)
    mut->next->prev = mut->prev;
  if (mut->prev)
    mut->prev->next = mut->next;
  else
    heap->mutators = mut->next;
  // We have no roots.  If there is a GC stop currently in progress,
  // maybe tell the controller it can continue.
  if (mutators_are_stopping(heap) && all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
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
  heap->extern_space = space;
}

static inline void tracer_visit(struct gc_edge edge, struct gc_heap *heap,
                                void *trace_data) GC_ALWAYS_INLINE;
static inline void
tracer_visit(struct gc_edge edge, struct gc_heap *heap, void *trace_data) {
  struct gc_trace_worker *worker = trace_data;
  if (trace_edge(heap, edge, worker))
    gc_trace_worker_enqueue(worker, gc_edge_ref(edge));
}

static inline int
trace_remembered_edge(struct gc_edge edge, struct gc_heap *heap,
                      void *trace_data) {
  GC_ASSERT(is_minor_collection(heap));
  tracer_visit(edge, heap, trace_data);

  // Return 1 if the edge should be kept in the remset, which is the
  // case only for new objects that survive the minor GC, and only the
  // nursery copy space has survivors.
  if (new_space_contains(heap, gc_edge_ref(edge)))
    return 1; // Keep edge in remset.
  // Otherwise remove field-logging bit and return 0 to indicate that
  // the remembered field set should remove this edge.
  if (copy_space_contains_edge(heap_old_space(heap), edge))
    copy_space_forget_edge(heap_old_space(heap), edge);
  else
    large_object_space_forget_edge(heap_large_object_space(heap), edge);
  return 0;
}

static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             struct gc_trace_worker *worker) {
#ifdef DEBUG
  if (GC_GENERATIONAL) {
    if (new_space_contains(heap, ref))
      GC_ASSERT_EQ(copy_space_object_region(ref),
                   heap_new_space(heap)->active_region);
    else if (old_space_contains(heap, ref))
      GC_ASSERT_EQ(copy_space_object_region(ref),
                   heap_old_space(heap)->active_region);
  } else {
    if (copy_space_contains(heap_mono_space(heap), ref))
      GC_ASSERT_EQ(copy_space_object_region(ref),
                   heap_mono_space(heap)->active_region);
  }
#endif

  gc_trace_object(ref, tracer_visit, heap, worker, NULL);
}

static inline void trace_root(struct gc_root root, struct gc_heap *heap,
                              struct gc_trace_worker *worker) {
  switch (root.kind) {
  case GC_ROOT_KIND_HEAP:
    gc_trace_heap_roots(root.heap->roots, tracer_visit, heap, worker);
    break;
  case GC_ROOT_KIND_MUTATOR:
    gc_trace_mutator_roots(root.mutator->roots, tracer_visit, heap, worker);
    break;
  case GC_ROOT_KIND_RESOLVED_EPHEMERONS:
    gc_trace_resolved_ephemerons(root.resolved_ephemerons, tracer_visit,
                                 heap, worker);
    break;
  case GC_ROOT_KIND_EDGE:
    tracer_visit(root.edge, heap, worker);
    break;
  case GC_ROOT_KIND_EDGE_BUFFER:
    gc_field_set_visit_edge_buffer(heap_remembered_set(heap), root.edge_buffer,
                                   trace_remembered_edge, heap, worker);
    break;
  default:
    GC_CRASH();
  }
}

static void request_mutators_to_stop(struct gc_heap *heap) {
  GC_ASSERT(!mutators_are_stopping(heap));
  atomic_store_explicit(&heap->collecting, 1, memory_order_relaxed);
}

static void allow_mutators_to_continue(struct gc_heap *heap) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(all_mutators_stopped(heap));
  heap->paused_mutator_count--;
  atomic_store_explicit(&heap->collecting, 0, memory_order_relaxed);
  GC_ASSERT(!mutators_are_stopping(heap));
  pthread_cond_broadcast(&heap->mutator_cond);
}

static void heap_reset_large_object_pages(struct gc_heap *heap, size_t npages) {
  size_t previous = heap->large_object_pages;
  heap->large_object_pages = npages;
  GC_ASSERT(npages <= previous);
  size_t bytes = (previous - npages) <<
    heap_large_object_space(heap)->page_size_log2;
  copy_space_reacquire_memory(heap_resizable_space(heap), bytes);
}

static void wait_for_mutators_to_stop(struct gc_heap *heap) {
  heap->paused_mutator_count++;
  while (!all_mutators_stopped(heap))
    pthread_cond_wait(&heap->collector_cond, &heap->lock);
}

static enum gc_collection_kind
pause_mutator_for_collection(struct gc_heap *heap,
                             struct gc_mutator *mut) GC_NEVER_INLINE;
static enum gc_collection_kind
pause_mutator_for_collection(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(!all_mutators_stopped(heap));
  MUTATOR_EVENT(mut, mutator_stopping);
  MUTATOR_EVENT(mut, mutator_stopped);
  heap->paused_mutator_count++;
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);

  enum gc_collection_kind collection_kind = GC_COLLECTION_MINOR;
  do {
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
    // is_minor_collection is reset before requesting mutators to stop, so this
    // will pick up either whether the last collection was minor, or whether the
    // next one will be minor.
    if (!GC_GENERATIONAL || !is_minor_collection(heap))
      collection_kind = GC_COLLECTION_COMPACTING;
  } while (mutators_are_stopping(heap));
  heap->paused_mutator_count--;

  MUTATOR_EVENT(mut, mutator_restarted);
  return collection_kind;
}

static void resize_heap(struct gc_heap *heap, size_t new_size) {
  if (new_size == heap->size)
    return;
  DEBUG("------ resizing heap\n");
  DEBUG("------ old heap size: %zu bytes\n", heap->size);
  DEBUG("------ new heap size: %zu bytes\n", new_size);
  if (new_size < heap->size)
    copy_space_shrink(heap_resizable_space(heap), heap->size - new_size);
  else
    copy_space_expand(heap_resizable_space(heap), new_size - heap->size);

  heap->size = new_size;
  HEAP_EVENT(heap, heap_resized, new_size);
}

static size_t heap_nursery_size(struct gc_heap *heap) {
#if GC_GENERATIONAL
  return heap->nursery_size;
#else
  GC_CRASH();
#endif
}

static void heap_set_nursery_size(struct gc_heap *heap, size_t size) {
#if GC_GENERATIONAL
  GC_ASSERT(size);
  heap->nursery_size = size;
#else
  GC_CRASH();
#endif
}

static size_t heap_nursery_size_for_mutator_count(struct gc_heap *heap,
                                                  size_t count) {
#if GC_GENERATIONAL
  return heap->per_processor_nursery_size * count;
#else
  GC_CRASH();
#endif
}

static void resize_nursery(struct gc_heap *heap, size_t size) {
  size_t prev_size = heap_nursery_size(heap);
  if (size < prev_size)
    copy_space_shrink(heap_new_space(heap), prev_size - size);
  else
    copy_space_reacquire_memory(heap_new_space(heap), size - prev_size);
  heap_set_nursery_size(heap, size);
}

static void resize_nursery_for_active_mutator_count(struct gc_heap *heap,
                                                    size_t count) {
  if (count > heap->processor_count)
    count = heap->processor_count;
  size_t prev_size = heap_nursery_size(heap);
  size_t size = heap_nursery_size_for_mutator_count(heap, count);
  // If there were more mutator processors this cycle than in the previous,
  // increase the nursery size.  Otherwise shrink, but with an exponential decay
  // factor.
  if (size < prev_size)
    size = (prev_size + size) / 2;
  resize_nursery(heap, size);
}

static void resize_for_active_mutator_count(struct gc_heap *heap) {
  size_t mutators = heap->max_active_mutator_count;
  GC_ASSERT(mutators);
  heap->max_active_mutator_count = 1;
  maybe_increase_max_active_mutator_count(heap);

  if (GC_GENERATIONAL)
    resize_nursery_for_active_mutator_count(heap, mutators);
}

static void visit_root_edge(struct gc_edge edge, struct gc_heap *heap,
                            void *unused) {
  gc_tracer_add_root(&heap->tracer, gc_root_edge(edge));
}

static void add_roots(struct gc_heap *heap, int is_minor_gc) {
  for (struct gc_mutator *mut = heap->mutators; mut; mut = mut->next)
    gc_tracer_add_root(&heap->tracer, gc_root_mutator(mut));
  gc_tracer_add_root(&heap->tracer, gc_root_heap(heap));
  gc_visit_finalizer_roots(heap->finalizer_state, visit_root_edge, heap, NULL);
  if (is_minor_gc)
    gc_field_set_add_roots(heap_remembered_set(heap), &heap->tracer);
}

static void
clear_remembered_set(struct gc_heap *heap) {
  gc_field_set_clear(heap_remembered_set(heap), NULL, NULL);
  large_object_space_clear_remembered_edges(heap_large_object_space(heap));
}

static void resolve_ephemerons_lazily(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 0,
                        memory_order_release);
}

static void resolve_ephemerons_eagerly(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 1,
                        memory_order_release);
  gc_scan_pending_ephemerons(gc_heap_pending_ephemerons(heap), heap, 0, 1);
}

static void trace_resolved_ephemerons(struct gc_heap *heap) {
  for (struct gc_ephemeron *resolved = gc_pop_resolved_ephemerons(heap);
       resolved;
       resolved = gc_pop_resolved_ephemerons(heap)) {
    gc_tracer_add_root(&heap->tracer, gc_root_resolved_ephemerons(resolved));
    gc_tracer_trace(&heap->tracer);
  }
}

static void resolve_finalizers(struct gc_heap *heap) {
  for (size_t priority = 0;
       priority < gc_finalizer_priority_count();
       priority++) {
    if (gc_resolve_finalizers(heap->finalizer_state, priority,
                              visit_root_edge, heap, NULL)) {
      gc_tracer_trace(&heap->tracer);
      trace_resolved_ephemerons(heap);
    }
  }
  gc_notify_finalizers(heap->finalizer_state, heap);
}

static void sweep_ephemerons(struct gc_heap *heap) {
  return gc_sweep_pending_ephemerons(gc_heap_pending_ephemerons(heap), 0, 1);
}

static int
heap_can_minor_gc(struct gc_heap *heap) {
  if (!GC_GENERATIONAL) return 0;
  // Invariant: the oldgen always has enough free space to accomodate promoted
  // objects from the nursery.  This is a precondition for minor GC of course,
  // but it is also a post-condition: after potentially promoting all nursery
  // objects, we still need an additional nursery's worth of space in oldgen to
  // satisfy the invariant.  We ensure the invariant by only doing minor GC if
  // the copy space can allocate as many bytes as the nursery, which is already
  // twice the allocatable size because of the copy reserve.
  struct copy_space *new_space = heap_new_space(heap);
  struct copy_space *old_space = heap_old_space(heap);
  size_t nursery_size = heap_nursery_size(heap);
  return copy_space_can_allocate(old_space, nursery_size) >= nursery_size;
}

static enum gc_collection_kind
determine_collection_kind(struct gc_heap *heap,
                          enum gc_collection_kind requested) {
  if (requested == GC_COLLECTION_MINOR && heap_can_minor_gc(heap))
    return GC_COLLECTION_MINOR;
  return GC_COLLECTION_COMPACTING;
}

static void
copy_spaces_start_gc(struct gc_heap *heap, int is_minor_gc) {
  if (GC_GENERATIONAL) {
    copy_space_flip(heap_new_space(heap));
    if (!is_minor_gc)
      copy_space_flip(heap_old_space(heap));
  } else {
    copy_space_flip(heap_mono_space(heap));
  }
}

static void
copy_spaces_finish_gc(struct gc_heap *heap, int is_minor_gc) {
  if (GC_GENERATIONAL) {
    copy_space_finish_gc(heap_new_space(heap), is_minor_gc);
    if (!is_minor_gc)
      copy_space_finish_gc(heap_old_space(heap), 0);
  } else {
    GC_ASSERT(!is_minor_gc);
    copy_space_finish_gc(heap_mono_space(heap), 0);
  }
}

static size_t
copy_spaces_allocated_bytes(struct gc_heap *heap) 
{
  return GC_GENERATIONAL
    ? (heap_new_space(heap)->allocated_bytes_at_last_gc +
       heap_old_space(heap)->allocated_bytes_at_last_gc)
    : heap_mono_space(heap)->allocated_bytes_at_last_gc;
}

static int
resolve_pending_large_allocation_and_compute_success(struct gc_heap *heap,
                                                     int is_minor_gc) {
  struct copy_space *space = heap_resizable_space(heap);
  ssize_t deficit = copy_space_page_out_blocks_until_memory_released(space);
  if (is_minor_gc)
    return 1;
  if (deficit <= 0)
    return copy_space_can_allocate(space, gc_allocator_large_threshold());
  deficit = align_up(deficit, COPY_SPACE_BLOCK_SIZE);
  if (heap->sizer.policy == GC_HEAP_SIZE_FIXED)
    return 0;
  resize_heap(heap, heap->size + deficit);
  return 1;
}

static int
collect(struct gc_mutator *mut,
        enum gc_collection_kind requested_kind) GC_NEVER_INLINE;
static int
collect(struct gc_mutator *mut, enum gc_collection_kind requested_kind) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *lospace = heap_large_object_space(heap);
  struct gc_extern_space *exspace = heap_extern_space(heap);
  uint64_t start_ns = gc_platform_monotonic_nanoseconds();
  MUTATOR_EVENT(mut, mutator_cause_gc);
  DEBUG("start collect #%ld:\n", heap->count);
  HEAP_EVENT(heap, requesting_stop);
  request_mutators_to_stop(heap);
  HEAP_EVENT(heap, waiting_for_stop);
  wait_for_mutators_to_stop(heap);
  HEAP_EVENT(heap, mutators_stopped);
  enum gc_collection_kind gc_kind =
    determine_collection_kind(heap, requested_kind);
  int is_minor_gc =
#if GC_GENERATIONAL
    heap->is_minor_collection =
#endif
    GC_GENERATIONAL ? gc_kind == GC_COLLECTION_MINOR : 0;
  uint64_t *counter_loc = &heap->total_allocated_bytes_at_last_gc;
  copy_space_add_to_allocation_counter(heap_allocation_space(heap),
                                       counter_loc);
  large_object_space_add_to_allocation_counter(lospace, counter_loc);
  HEAP_EVENT(heap, prepare_gc, gc_kind, *counter_loc);
  copy_spaces_start_gc(heap, is_minor_gc);
  large_object_space_start_gc(lospace, is_minor_gc);
  gc_extern_space_start_gc(exspace, is_minor_gc);
  resolve_ephemerons_lazily(heap);
  gc_tracer_prepare(&heap->tracer);
  add_roots(heap, is_minor_gc);
  HEAP_EVENT(heap, roots_traced);
  gc_tracer_trace(&heap->tracer);
  HEAP_EVENT(heap, heap_traced);
  resolve_ephemerons_eagerly(heap);
  trace_resolved_ephemerons(heap);
  HEAP_EVENT(heap, ephemerons_traced);
  resolve_finalizers(heap);
  HEAP_EVENT(heap, finalizers_traced);
  sweep_ephemerons(heap);
  gc_tracer_release(&heap->tracer);
  copy_spaces_finish_gc(heap, is_minor_gc);
  large_object_space_finish_gc(lospace, is_minor_gc);
  gc_extern_space_finish_gc(exspace, is_minor_gc);
  if (GC_GENERATIONAL && !is_minor_gc)
    clear_remembered_set(heap);
  heap->count++;
  resize_for_active_mutator_count(heap);
  heap_reset_large_object_pages(heap, lospace->live_pages_at_last_collection);
  size_t live_size = (copy_spaces_allocated_bytes(heap) +
                      large_object_space_size_at_last_collection(lospace));
  uint64_t pause_ns = gc_platform_monotonic_nanoseconds() - start_ns;
  HEAP_EVENT(heap, live_data_size, live_size);
  gc_heap_sizer_on_gc(heap->sizer, heap->size, live_size, pause_ns,
                      resize_heap);
  int success =
    resolve_pending_large_allocation_and_compute_success(heap, is_minor_gc);
  HEAP_EVENT(heap, restarting_mutators);
  allow_mutators_to_continue(heap);
  return success;
}

static int trigger_collection(struct gc_mutator *mut,
                              enum gc_collection_kind requested_kind) {
  struct gc_heap *heap = mutator_heap(mut);
  copy_space_allocator_finish(&mut->allocator, heap_allocation_space(heap));
  if (GC_GENERATIONAL)
    gc_field_set_writer_release_buffer(mutator_field_logger(mut));
  heap_lock(heap);
  int prev_kind = -1;
  int success = 1;
  while (mutators_are_stopping(heap))
    prev_kind = pause_mutator_for_collection(heap, mut);
  if (prev_kind < (int)requested_kind)
    success = collect(mut, requested_kind);
  heap_unlock(heap);
  return success;
}

void gc_collect(struct gc_mutator *mut, enum gc_collection_kind kind) {
  trigger_collection(mut, kind);
}

int gc_heap_contains(struct gc_heap *heap, struct gc_ref ref) {
  GC_ASSERT(gc_ref_is_heap_object(ref));
  return (GC_GENERATIONAL
          ? (new_space_contains(heap, ref) || old_space_contains(heap, ref))
          : copy_space_contains(heap_mono_space(heap), ref))
    || large_object_space_contains(heap_large_object_space(heap), ref);
}

static void* allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);
  struct copy_space *copy_space = heap_resizable_space(heap);

  size_t npages = large_object_space_npages(space, size);
  size_t page_bytes = npages << space->page_size_log2;

  copy_space_request_release_memory(copy_space, page_bytes);
  if (copy_space_page_out_blocks_until_memory_released(copy_space) > 0
      && !trigger_collection(mut, GC_COLLECTION_COMPACTING)) {
    copy_space_maybe_reacquire_memory(copy_space, page_bytes);
    return heap->allocation_failure(heap, size);
  }

  atomic_fetch_add(&heap->large_object_pages, npages);

  void *ret = large_object_space_alloc(space, npages, GC_TRACE_PRECISELY);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    GC_CRASH();
  }

  return ret;
}

void* gc_allocate_slow(struct gc_mutator *mut, size_t size,
                       enum gc_allocation_kind kind) {
  if (GC_UNLIKELY(kind != GC_ALLOCATION_TAGGED
                  && kind != GC_ALLOCATION_TAGGED_POINTERLESS)) {
    fprintf(stderr, "pcc collector cannot make allocations of kind %d\n",
            (int)kind);
    GC_CRASH();
  }
  GC_ASSERT(size > 0); // allocating 0 bytes would be silly

  if (size > gc_allocator_large_threshold())
    return allocate_large(mut, size);

  struct gc_ref ret;
  while (1) {
    ret = copy_space_allocate(&mut->allocator,
                              heap_allocation_space(mutator_heap(mut)),
                              size);
    if (!gc_ref_is_null(ret))
      break;
    if (trigger_collection(mut, GC_COLLECTION_MINOR))
      continue;
    return mutator_heap(mut)->allocation_failure(mutator_heap(mut), size);
  }

  return gc_ref_heap_object(ret);
}

void gc_pin_object(struct gc_mutator *mut, struct gc_ref ref) {
  GC_CRASH();
}

int gc_object_is_old_generation_slow(struct gc_mutator *mut,
                                     struct gc_ref obj) {
  if (!GC_GENERATIONAL)
    return 0;

  struct gc_heap *heap = mutator_heap(mut);

  if (copy_space_contains(heap_new_space(heap), obj))
    return 0;
  if (copy_space_contains(heap_old_space(heap), obj))
    return 1;

  struct large_object_space *lospace = heap_large_object_space(heap);
  if (large_object_space_contains(lospace, obj))
    return large_object_space_is_survivor(lospace, obj);

  return 0;
}

void gc_write_barrier_slow(struct gc_mutator *mut, struct gc_ref obj,
                           size_t obj_size, struct gc_edge edge,
                           struct gc_ref new_val) {
  GC_ASSERT(!gc_ref_is_null(new_val));
  if (!GC_GENERATIONAL) return;
  if (gc_object_is_old_generation_slow(mut, new_val))
    return;
  struct gc_heap *heap = mutator_heap(mut);
  if ((obj_size <= gc_allocator_large_threshold())
      ? copy_space_remember_edge(heap_old_space(heap), edge)
      : large_object_space_remember_edge(heap_large_object_space(heap),
                                         obj, edge))
    gc_field_set_writer_add_edge(mutator_field_logger(mut), edge);
}

int* gc_safepoint_flag_loc(struct gc_mutator *mut) {
  return &mutator_heap(mut)->collecting;
}

void gc_safepoint_slow(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  copy_space_allocator_finish(&mut->allocator, heap_allocation_space(heap));
  if (GC_GENERATIONAL)
    gc_field_set_writer_release_buffer(mutator_field_logger(mut));
  heap_lock(heap);
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection(heap, mut);
  heap_unlock(heap);
}
  
int gc_safepoint_signal_number(void) { GC_CRASH(); }
void gc_safepoint_signal_inhibit(struct gc_mutator *mut) { GC_CRASH(); }
void gc_safepoint_signal_reallow(struct gc_mutator *mut) { GC_CRASH(); }

struct gc_ephemeron* gc_allocate_ephemeron(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_ephemeron_size(), GC_ALLOCATION_TAGGED);
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
}

struct gc_ref
gc_ephemeron_swap_value(struct gc_mutator *mut, struct gc_ephemeron *e,
                        struct gc_ref ref) {
  gc_write_barrier(mut, gc_ref_from_heap_object(e), gc_ephemeron_size(),
                   gc_ephemeron_value_edge(e), ref);
  return gc_ephemeron_swap_value_internal(e, ref);
}

struct gc_pending_ephemerons *gc_heap_pending_ephemerons(struct gc_heap *heap) {
#if GC_GENERATIONAL
  if (is_minor_collection(heap))
    return heap->nursery_pending_ephemerons;
#endif
  return heap->pending_ephemerons;
}

unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return heap->count;
}

struct gc_finalizer* gc_allocate_finalizer(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_finalizer_size(), GC_ALLOCATION_TAGGED);
}

void gc_finalizer_attach(struct gc_mutator *mut, struct gc_finalizer *finalizer,
                         unsigned priority, struct gc_ref object,
                         struct gc_ref closure) {
  gc_finalizer_init_internal(finalizer, object, closure);
  gc_finalizer_attach_internal(mutator_heap(mut)->finalizer_state,
                               finalizer, priority);
  // No write barrier.
}

struct gc_finalizer* gc_pop_finalizable(struct gc_mutator *mut) {
  return gc_finalizer_state_pop(mutator_heap(mut)->finalizer_state);
}

void gc_set_finalizer_callback(struct gc_heap *heap,
                               gc_finalizer_callback callback) {
  gc_finalizer_state_set_callback(heap->finalizer_state, callback);
}

static int
heap_do_prepare_pending_ephemerons(struct gc_heap *heap,
                                   struct gc_pending_ephemerons **loc,
                                   size_t size) {
  size_t target = size * heap->pending_ephemerons_size_factor;
  double slop = heap->pending_ephemerons_size_slop;

  return !!(*loc = gc_prepare_pending_ephemerons(*loc, target, slop));
}

static int heap_prepare_pending_ephemerons(struct gc_heap *heap) {
  return heap_do_prepare_pending_ephemerons(heap, &heap->pending_ephemerons,
                                             heap->size)
#if GC_GENERATIONAL
    && heap_do_prepare_pending_ephemerons(heap,
                                          &heap->nursery_pending_ephemerons,
                                          heap->per_processor_nursery_size * 2)
#endif
    ;
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

// with heap lock
static uint64_t allocation_counter(struct gc_heap *heap) {
  uint64_t ret = heap->total_allocated_bytes_at_last_gc;
  copy_space_add_to_allocation_counter(heap_allocation_space(heap), &ret);
  large_object_space_add_to_allocation_counter(heap_large_object_space(heap),
                                               &ret);
  return ret;
}

uint64_t gc_allocation_counter(struct gc_heap *heap) {
  pthread_mutex_lock(&heap->lock);
  uint64_t ret = allocation_counter(heap);
  pthread_mutex_unlock(&heap->lock);
  return ret;
}

static uint64_t allocation_counter_from_thread(struct gc_heap *heap) {
  if (pthread_mutex_trylock(&heap->lock)) return 0;
  uint64_t ret = allocation_counter(heap);
  pthread_mutex_unlock(&heap->lock);
  return ret;
}

static void set_heap_size_from_thread(struct gc_heap *heap, size_t size) {
  if (pthread_mutex_trylock(&heap->lock)) return;
  resize_heap(heap, size);
  pthread_mutex_unlock(&heap->lock);
}

static void* allocation_failure(struct gc_heap *heap, size_t size) {
  fprintf(stderr, "ran out of space, heap size %zu\n", heap->size);
  GC_CRASH();
  return NULL;
}

void gc_heap_set_allocation_failure_handler(struct gc_heap *heap,
                                            void* (*handler)(struct gc_heap*,
                                                             size_t)) {
  heap->allocation_failure = handler;
}

static int heap_init(struct gc_heap *heap, const struct gc_options *options) {
  // *heap is already initialized to 0.

  if (GC_GENERATIONAL)
    gc_field_set_init(heap_remembered_set(heap));
  pthread_mutex_init(&heap->lock, NULL);
  pthread_cond_init(&heap->mutator_cond, NULL);
  pthread_cond_init(&heap->collector_cond, NULL);
  heap->size = options->common.heap_size;
  heap->processor_count = gc_platform_processor_count();
  // max_active_mutator_count never falls below 1 after this point.
  heap->max_active_mutator_count = 1;

#if GC_GENERATIONAL
  // We should add an option to set this, but for now, 2 MB per processor.
  heap->per_processor_nursery_size = 2 * 1024 * 1024;
#endif

  if (!gc_tracer_init(&heap->tracer, heap, options->common.parallelism))
    GC_CRASH();

  heap->pending_ephemerons_size_factor = 0.005;
  heap->pending_ephemerons_size_slop = 0.5;

  if (!heap_prepare_pending_ephemerons(heap))
    GC_CRASH();

  heap->finalizer_state = gc_make_finalizer_state();
  if (!heap->finalizer_state)
    GC_CRASH();

  heap->background_thread = gc_make_background_thread();
  heap->sizer = gc_make_heap_sizer(heap, &options->common,
                                   allocation_counter_from_thread,
                                   set_heap_size_from_thread,
                                   heap->background_thread);
  heap->allocation_failure = allocation_failure;

  return 1;
}

int gc_init(const struct gc_options *options, struct gc_stack_addr stack_base,
            struct gc_heap **heap, struct gc_mutator **mut,
            struct gc_event_listener event_listener,
            void *event_listener_data) {
  GC_ASSERT_EQ(gc_allocator_small_granule_size(), GC_ALIGNMENT);
  GC_ASSERT_EQ(gc_allocator_large_threshold(), GC_LARGE_OBJECT_THRESHOLD);
  GC_ASSERT_EQ(0, offsetof(struct gc_mutator, allocator));
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct copy_space_allocator, hp));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct copy_space_allocator, limit));
  if (GC_GENERATIONAL) {
    GC_ASSERT_EQ(gc_write_barrier_field_table_alignment(),
                 COPY_SPACE_SLAB_SIZE);
    GC_ASSERT_EQ(gc_write_barrier_field_table_offset(),
                 offsetof(struct copy_space_slab, blocks));
  }

  *heap = calloc(1, sizeof(struct gc_heap));
  if (!*heap) GC_CRASH();

  if (!heap_init(*heap, options))
    GC_CRASH();

  (*heap)->event_listener = event_listener;
  (*heap)->event_listener_data = event_listener_data;
  HEAP_EVENT(*heap, init, (*heap)->size);

  {
    uint32_t flags = 0;
    if (options->common.parallelism > 1)
      flags |= COPY_SPACE_ATOMIC_FORWARDING;
    if (GC_GENERATIONAL) {
      size_t nursery_size =
        heap_nursery_size_for_mutator_count(*heap, (*heap)->processor_count);
      heap_set_nursery_size(*heap, nursery_size);
      if (!copy_space_init(heap_new_space(*heap), nursery_size,
                           flags | COPY_SPACE_ALIGNED,
                           (*heap)->background_thread)) {
        free(*heap);
        *heap = NULL;
        return 0;
      }
      // Initially dimension the nursery for one mutator.
      resize_nursery(*heap, heap_nursery_size_for_mutator_count(*heap, 1));

      if (!copy_space_init(heap_old_space(*heap), (*heap)->size,
                           flags | COPY_SPACE_HAS_FIELD_LOGGING_BITS,
                           (*heap)->background_thread)) {
        free(*heap);
        *heap = NULL;
        return 0;
      }
    } else {
      if (!copy_space_init(heap_mono_space(*heap), (*heap)->size, flags,
                           (*heap)->background_thread)) {
        free(*heap);
        *heap = NULL;
        return 0;
      }
    }
  }
  
  if (!large_object_space_init(heap_large_object_space(*heap), *heap,
                               (*heap)->background_thread))
    GC_CRASH();

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  add_mutator(*heap, *mut);

  gc_background_thread_start((*heap)->background_thread);

  return 1;
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr stack_base,
                                      struct gc_heap *heap) {
  struct gc_mutator *ret = calloc(1, sizeof(struct gc_mutator));
  if (!ret)
    GC_CRASH();
  add_mutator(heap, ret);
  return ret;
}

void gc_finish_for_thread(struct gc_mutator *mut) {
  remove_mutator(mutator_heap(mut), mut);
  free(mut);
}

static void deactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(mut->next == NULL);
  GC_ASSERT(mut->active);
  copy_space_allocator_finish(&mut->allocator, heap_allocation_space(heap));
  if (GC_GENERATIONAL)
    gc_field_set_writer_release_buffer(mutator_field_logger(mut));
  heap_lock(heap);
  heap->inactive_mutator_count++;
  mut->active = 0;
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void reactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(!mut->active);
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  mut->active = 1;
  heap->inactive_mutator_count--;
  maybe_increase_max_active_mutator_count(heap);
  heap_unlock(heap);
}

void gc_deactivate(struct gc_mutator *mut) {
  GC_ASSERT(mut->active);
  deactivate_mutator(mutator_heap(mut), mut);
}

void gc_reactivate(struct gc_mutator *mut) {
  GC_ASSERT(!mut->active);
  reactivate_mutator(mutator_heap(mut), mut);
}

void* gc_deactivate_for_call(struct gc_mutator *mut,
                             void* (*f)(struct gc_mutator*, void*),
                             void *data) {
  struct gc_heap *heap = mutator_heap(mut);
  deactivate_mutator(heap, mut);
  void *ret = f(mut, data);
  reactivate_mutator(heap, mut);
  return ret;
}

void* gc_reactivate_for_call(struct gc_mutator *mut,
                             void* (*f)(struct gc_mutator*, void*),
                             void *data) {
  struct gc_heap *heap = mutator_heap(mut);
  int reactivate = !mut->active;
  if (reactivate)
    reactivate_mutator(heap, mut);
  void *ret = f(mut, data);
  if (reactivate)
    deactivate_mutator(heap, mut);
  return ret;
}
