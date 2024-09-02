#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "debug.h"
#include "gc-align.h"
#include "gc-inline.h"
#include "gc-platform.h"
#include "gc-stack.h"
#include "gc-trace.h"
#include "large-object-space.h"
#include "nofl-space.h"
#if GC_PARALLEL
#include "parallel-tracer.h"
#else
#include "serial-tracer.h"
#endif
#include "spin.h"
#include "mmc-attrs.h"

#define LARGE_OBJECT_THRESHOLD 8192

struct gc_heap {
  struct nofl_space nofl_space;
  struct large_object_space large_object_space;
  struct gc_extern_space *extern_space;
  size_t large_object_pages;
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  size_t size;
  int collecting;
  int mark_while_stopping;
  int check_pending_ephemerons;
  struct gc_pending_ephemerons *pending_ephemerons;
  struct gc_finalizer_state *finalizer_state;
  enum gc_collection_kind gc_kind;
  int multithreaded;
  size_t mutator_count;
  size_t paused_mutator_count;
  size_t inactive_mutator_count;
  struct gc_heap_roots *roots;
  struct gc_mutator *mutator_trace_list;
  long count;
  uint8_t last_collection_was_minor;
  struct gc_mutator *inactive_mutators;
  struct gc_tracer tracer;
  double fragmentation_low_threshold;
  double fragmentation_high_threshold;
  double minor_gc_yield_threshold;
  double major_gc_yield_threshold;
  double minimum_major_gc_yield_threshold;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
  struct gc_event_listener event_listener;
  void *event_listener_data;
};

#define HEAP_EVENT(heap, event, ...)                                    \
  (heap)->event_listener.event((heap)->event_listener_data, ##__VA_ARGS__)
#define MUTATOR_EVENT(mut, event, ...)                                  \
  (mut)->heap->event_listener.event((mut)->event_listener_data, ##__VA_ARGS__)

struct gc_mutator_mark_buf {
  size_t size;
  size_t capacity;
  struct gc_ref *objects;
};

struct gc_mutator {
  struct nofl_allocator allocator;
  struct gc_heap *heap;
  struct gc_stack stack;
  struct gc_mutator_roots *roots;
  struct gc_mutator_mark_buf mark_buf;
  void *event_listener_data;
  // Three uses for this in-object linked-list pointer:
  //  - inactive (blocked in syscall) mutators
  //  - grey objects when stopping active mutators for mark-in-place
  //  - untraced mutators when stopping active mutators for evacuation
  struct gc_mutator *next;
};

struct gc_trace_worker_data {
  struct nofl_allocator allocator;
};

static inline struct nofl_space*
heap_nofl_space(struct gc_heap *heap) {
  return &heap->nofl_space;
}
static inline struct large_object_space*
heap_large_object_space(struct gc_heap *heap) {
  return &heap->large_object_space;
}
static inline struct gc_extern_space*
heap_extern_space(struct gc_heap *heap) {
  return heap->extern_space;
}
static inline struct gc_heap*
mutator_heap(struct gc_mutator *mutator) {
  return mutator->heap;
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
  nofl_allocator_reset(&data.allocator);
  f(tracer, heap, worker, &data);
  nofl_allocator_finish(&data.allocator, heap_nofl_space(heap));
}

static void collect(struct gc_mutator *mut,
                    enum gc_collection_kind requested_kind) GC_NEVER_INLINE;

static inline int
do_trace(struct gc_heap *heap, struct gc_edge edge, struct gc_ref ref,
         struct gc_trace_worker *worker) {
  if (!gc_ref_is_heap_object(ref))
    return 0;
  if (GC_LIKELY(nofl_space_contains(heap_nofl_space(heap), ref))) {
    struct nofl_allocator *alloc =
      worker ? &gc_trace_worker_data(worker)->allocator : NULL;
    return nofl_space_evacuate_or_mark_object(heap_nofl_space(heap), edge, ref,
                                              alloc);
  } else if (large_object_space_contains(heap_large_object_space(heap), ref))
    return large_object_space_mark_object(heap_large_object_space(heap),
                                          ref);
  else
    return gc_extern_space_visit(heap_extern_space(heap), edge, ref);
}

static inline int trace_edge(struct gc_heap *heap,
                             struct gc_edge edge,
                             struct gc_trace_worker *worker) GC_ALWAYS_INLINE;

static inline int
trace_edge(struct gc_heap *heap, struct gc_edge edge, struct gc_trace_worker *worker) {
  struct gc_ref ref = gc_edge_ref(edge);
  int is_new = do_trace(heap, edge, ref, worker);

  if (is_new &&
      GC_UNLIKELY(atomic_load_explicit(&heap->check_pending_ephemerons,
                                       memory_order_relaxed)))
    gc_resolve_pending_ephemerons(ref, heap);

  return is_new;
}

int
gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  if (!gc_ref_is_heap_object(ref))
    return 0;

  struct nofl_space *nofl_space = heap_nofl_space(heap);
  if (GC_LIKELY(nofl_space_contains(nofl_space, ref)))
    return nofl_space_forward_or_mark_if_traced(nofl_space, edge, ref);

  struct large_object_space *lospace = heap_large_object_space(heap);
  if (large_object_space_contains(lospace, ref))
    return large_object_space_is_copied(lospace, ref);

  GC_CRASH();
}

static inline struct gc_ref
do_trace_conservative_ref(struct gc_heap *heap, struct gc_conservative_ref ref,
                          int possibly_interior) {
  if (!gc_conservative_ref_might_be_a_heap_object(ref, possibly_interior))
    return gc_ref_null();

  struct nofl_space *nofl_space = heap_nofl_space(heap);
  if (GC_LIKELY(nofl_space_contains_conservative_ref(nofl_space, ref)))
    return nofl_space_mark_conservative_ref(nofl_space, ref, possibly_interior);

  struct large_object_space *lospace = heap_large_object_space(heap);
  return large_object_space_mark_conservative_ref(lospace, ref,
                                                  possibly_interior);
}

static inline struct gc_ref
trace_conservative_ref(struct gc_heap *heap, struct gc_conservative_ref ref,
                       int possibly_interior) {
  struct gc_ref ret = do_trace_conservative_ref(heap, ref, possibly_interior);

  if (gc_ref_is_heap_object(ret) &&
      GC_UNLIKELY(atomic_load_explicit(&heap->check_pending_ephemerons,
                                       memory_order_relaxed)))
    gc_resolve_pending_ephemerons(ret, heap);

  return ret;
}

static int
mutators_are_stopping(struct gc_heap *heap) {
  return atomic_load_explicit(&heap->collecting, memory_order_relaxed);
}

static inline void
heap_lock(struct gc_heap *heap) {
  pthread_mutex_lock(&heap->lock);
}
static inline void
heap_unlock(struct gc_heap *heap) {
  pthread_mutex_unlock(&heap->lock);
}

// with heap lock
static inline int
all_mutators_stopped(struct gc_heap *heap) {
  return heap->mutator_count ==
    heap->paused_mutator_count + heap->inactive_mutator_count;
}

static void
add_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  mut->heap = heap;
  mut->event_listener_data =
    heap->event_listener.mutator_added(heap->event_listener_data);
  heap_lock(heap);
  // We have no roots.  If there is a GC currently in progress, we have
  // nothing to add.  Just wait until it's done.
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  if (heap->mutator_count == 1)
    heap->multithreaded = 1;
  heap->mutator_count++;
  heap_unlock(heap);
}

static void
remove_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  nofl_allocator_finish(&mut->allocator, heap_nofl_space(heap));
  MUTATOR_EVENT(mut, mutator_removed);
  mut->heap = NULL;
  heap_lock(heap);
  heap->mutator_count--;
  // We have no roots.  If there is a GC stop currently in progress,
  // maybe tell the controller it can continue.
  if (mutators_are_stopping(heap) && all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void
request_mutators_to_stop(struct gc_heap *heap) {
  GC_ASSERT(!mutators_are_stopping(heap));
  atomic_store_explicit(&heap->collecting, 1, memory_order_relaxed);
}

static void
allow_mutators_to_continue(struct gc_heap *heap) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(all_mutators_stopped(heap));
  heap->paused_mutator_count = 0;
  atomic_store_explicit(&heap->collecting, 0, memory_order_relaxed);
  GC_ASSERT(!mutators_are_stopping(heap));
  pthread_cond_broadcast(&heap->mutator_cond);
}

static void
heap_reset_large_object_pages(struct gc_heap *heap, size_t npages) {
  size_t previous = heap->large_object_pages;
  heap->large_object_pages = npages;
  GC_ASSERT(npages <= previous);
  size_t bytes = (previous - npages) <<
    heap_large_object_space(heap)->page_size_log2;
  nofl_space_reacquire_memory(heap_nofl_space(heap), bytes);
}

static void
mutator_mark_buf_grow(struct gc_mutator_mark_buf *buf) {
  size_t old_capacity = buf->capacity;
  size_t old_bytes = old_capacity * sizeof(struct gc_ref);

  size_t new_bytes = old_bytes ? old_bytes * 2 : getpagesize();
  size_t new_capacity = new_bytes / sizeof(struct gc_ref);

  void *mem = mmap(NULL, new_bytes, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("allocating mutator mark buffer failed");
    GC_CRASH();
  }
  if (old_bytes) {
    memcpy(mem, buf->objects, old_bytes);
    munmap(buf->objects, old_bytes);
  }
  buf->objects = mem;
  buf->capacity = new_capacity;
}

static void
mutator_mark_buf_push(struct gc_mutator_mark_buf *buf, struct gc_ref ref) {
  if (GC_UNLIKELY(buf->size == buf->capacity))
    mutator_mark_buf_grow(buf);
  buf->objects[buf->size++] = ref;
}

static void
mutator_mark_buf_release(struct gc_mutator_mark_buf *buf) {
  size_t bytes = buf->size * sizeof(struct gc_ref);
  if (bytes >= getpagesize())
    madvise(buf->objects, align_up(bytes, getpagesize()), MADV_DONTNEED);
  buf->size = 0;
}

static void
mutator_mark_buf_destroy(struct gc_mutator_mark_buf *buf) {
  size_t bytes = buf->capacity * sizeof(struct gc_ref);
  if (bytes)
    munmap(buf->objects, bytes);
}

static void
enqueue_mutator_for_tracing(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mut->next == NULL);
  struct gc_mutator *next =
    atomic_load_explicit(&heap->mutator_trace_list, memory_order_acquire);
  do {
    mut->next = next;
  } while (!atomic_compare_exchange_weak(&heap->mutator_trace_list,
                                         &next, mut));
}

static int
heap_should_mark_while_stopping(struct gc_heap *heap) {
  return atomic_load_explicit(&heap->mark_while_stopping, memory_order_acquire);
}

static int
mutator_should_mark_while_stopping(struct gc_mutator *mut) {
  return heap_should_mark_while_stopping(mutator_heap(mut));
}

void
gc_mutator_set_roots(struct gc_mutator *mut, struct gc_mutator_roots *roots) {
  mut->roots = roots;
}
void
gc_heap_set_roots(struct gc_heap *heap, struct gc_heap_roots *roots) {
  heap->roots = roots;
}
void
gc_heap_set_extern_space(struct gc_heap *heap, struct gc_extern_space *space) {
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

static void
trace_and_enqueue_locally(struct gc_edge edge, struct gc_heap *heap,
                          void *data) {
  struct gc_mutator *mut = data;
  if (trace_edge(heap, edge, NULL))
    mutator_mark_buf_push(&mut->mark_buf, gc_edge_ref(edge));
}

static inline void
do_trace_conservative_ref_and_enqueue_locally(struct gc_conservative_ref ref,
                                              struct gc_heap *heap,
                                              void *data,
                                              int possibly_interior) {
  struct gc_mutator *mut = data;
  struct gc_ref object = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(object))
    mutator_mark_buf_push(&mut->mark_buf, object);
}

static void
trace_possibly_interior_conservative_ref_and_enqueue_locally(struct gc_conservative_ref ref,
                                                             struct gc_heap *heap,
                                                             void *data) {
  return do_trace_conservative_ref_and_enqueue_locally(ref, heap, data, 1);
}

static void
trace_conservative_ref_and_enqueue_locally(struct gc_conservative_ref ref,
                                           struct gc_heap *heap,
                                           void *data) {
  return do_trace_conservative_ref_and_enqueue_locally(ref, heap, data, 0);
}

static void
trace_and_enqueue_globally(struct gc_edge edge, struct gc_heap *heap,
                           void *unused) {
  if (trace_edge(heap, edge, NULL))
    gc_tracer_enqueue_root(&heap->tracer, gc_edge_ref(edge));
}

static inline void
do_trace_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
                                               struct gc_heap *heap,
                                               void *data,
                                               int possibly_interior) {
  struct gc_ref object = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(object))
    gc_tracer_enqueue_root(&heap->tracer, object);
}

static void
trace_possibly_interior_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
                                                              struct gc_heap *heap,
                                                              void *data) {
  return do_trace_conservative_ref_and_enqueue_globally(ref, heap, data, 1);
}

static void
trace_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
                                            struct gc_heap *heap,
                                            void *data) {
  return do_trace_conservative_ref_and_enqueue_globally(ref, heap, data, 0);
}

static inline struct gc_conservative_ref
load_conservative_ref(uintptr_t addr) {
  GC_ASSERT((addr & (sizeof(uintptr_t) - 1)) == 0);
  uintptr_t val;
  memcpy(&val, (char*)addr, sizeof(uintptr_t));
  return gc_conservative_ref(val);
}

static inline void
trace_conservative_edges(uintptr_t low,
                         uintptr_t high,
                         void (*trace)(struct gc_conservative_ref,
                                       struct gc_heap *, void *),
                         struct gc_heap *heap,
                         void *data) {
  GC_ASSERT(low == align_down(low, sizeof(uintptr_t)));
  GC_ASSERT(high == align_down(high, sizeof(uintptr_t)));
  for (uintptr_t addr = low; addr < high; addr += sizeof(uintptr_t))
    trace(load_conservative_ref(addr), heap, data);
}

static inline void
tracer_trace_conservative_ref(struct gc_conservative_ref ref,
                              struct gc_heap *heap, void *data) {
  struct gc_trace_worker *worker = data;
  int possibly_interior = 0;
  struct gc_ref resolved = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(resolved))
    gc_trace_worker_enqueue(worker, resolved);
}

static inline void
trace_one_conservatively(struct gc_ref ref, struct gc_heap *heap,
                         struct gc_trace_worker *worker) {
  size_t bytes;
  if (GC_LIKELY(nofl_space_contains(heap_nofl_space(heap), ref))) {
    // Generally speaking we trace conservatively and don't allow much
    // in the way of incremental precise marking on a
    // conservative-by-default heap.  But, we make an exception for
    // ephemerons.
    if (GC_UNLIKELY(nofl_is_ephemeron(ref))) {
      gc_trace_ephemeron(gc_ref_heap_object(ref), tracer_visit, heap,
                         worker);
      return;
    }
    bytes = nofl_space_object_size(heap_nofl_space(heap), ref);
  } else {
    bytes = large_object_space_object_size(heap_large_object_space(heap), ref);
  }
  trace_conservative_edges(gc_ref_value(ref),
                           gc_ref_value(ref) + bytes,
                           tracer_trace_conservative_ref, heap,
                           worker);
}

static inline void
trace_one(struct gc_ref ref, struct gc_heap *heap,
          struct gc_trace_worker *worker) {
  if (gc_has_conservative_intraheap_edges())
    trace_one_conservatively(ref, heap, worker);
  else
    gc_trace_object(ref, tracer_visit, heap, worker, NULL);
}

static inline void
trace_root(struct gc_root root, struct gc_heap *heap,
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
  default:
    GC_CRASH();
  }
}

static void
visit_root_edge(struct gc_edge edge, struct gc_heap *heap, void *unused) {
  gc_tracer_add_root(&heap->tracer, gc_root_edge(edge));
}

static void
mark_and_globally_enqueue_mutator_conservative_roots(uintptr_t low,
                                                     uintptr_t high,
                                                     struct gc_heap *heap,
                                                     void *data) {
  trace_conservative_edges(low, high,
                           gc_mutator_conservative_roots_may_be_interior()
                           ? trace_possibly_interior_conservative_ref_and_enqueue_globally
                           : trace_conservative_ref_and_enqueue_globally,
                           heap, data);
}

static void
mark_and_globally_enqueue_heap_conservative_roots(uintptr_t low,
                                                  uintptr_t high,
                                                  struct gc_heap *heap,
                                                  void *data) {
  trace_conservative_edges(low, high,
                           trace_conservative_ref_and_enqueue_globally,
                           heap, data);
}

static void
mark_and_locally_enqueue_mutator_conservative_roots(uintptr_t low,
                                                    uintptr_t high,
                                                    struct gc_heap *heap,
                                                    void *data) {
  trace_conservative_edges(low, high,
                           gc_mutator_conservative_roots_may_be_interior()
                           ? trace_possibly_interior_conservative_ref_and_enqueue_locally
                           : trace_conservative_ref_and_enqueue_locally,
                           heap, data);
}

static inline void
trace_mutator_conservative_roots(struct gc_mutator *mut,
                                 void (*trace_range)(uintptr_t low,
                                                     uintptr_t high,
                                                     struct gc_heap *heap,
                                                     void *data),
                                 struct gc_heap *heap,
                                 void *data) {
  if (gc_has_mutator_conservative_roots())
    gc_stack_visit(&mut->stack, trace_range, heap, data);
}

// Mark the roots of a mutator that is stopping for GC.  We can't
// enqueue them directly, so we send them to the controller in a buffer.
static void
trace_stopping_mutator_roots(struct gc_mutator *mut) {
  GC_ASSERT(mutator_should_mark_while_stopping(mut));
  struct gc_heap *heap = mutator_heap(mut);
  trace_mutator_conservative_roots(mut,
                                   mark_and_locally_enqueue_mutator_conservative_roots,
                                   heap, mut);
  gc_trace_mutator_roots(mut->roots, trace_and_enqueue_locally, heap, mut);
}

static void
trace_mutator_conservative_roots_with_lock(struct gc_mutator *mut) {
  trace_mutator_conservative_roots(mut,
                                   mark_and_globally_enqueue_mutator_conservative_roots,
                                   mutator_heap(mut),
                                   NULL);
}

static void
trace_mutator_roots_with_lock(struct gc_mutator *mut) {
  trace_mutator_conservative_roots_with_lock(mut);
  gc_trace_mutator_roots(mut->roots, trace_and_enqueue_globally,
                         mutator_heap(mut), NULL);
}

static void
trace_mutator_roots_with_lock_before_stop(struct gc_mutator *mut) {
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    trace_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
}

static void
release_stopping_mutator_roots(struct gc_mutator *mut) {
  mutator_mark_buf_release(&mut->mark_buf);
}

static void
wait_for_mutators_to_stop(struct gc_heap *heap) {
  heap->paused_mutator_count++;
  while (!all_mutators_stopped(heap))
    pthread_cond_wait(&heap->collector_cond, &heap->lock);
}

static void
trace_mutator_conservative_roots_after_stop(struct gc_heap *heap) {
  int active_mutators_already_marked = heap_should_mark_while_stopping(heap);
  if (!active_mutators_already_marked)
    for (struct gc_mutator *mut = atomic_load(&heap->mutator_trace_list);
         mut;
         mut = mut->next)
      trace_mutator_conservative_roots_with_lock(mut);

  for (struct gc_mutator *mut = heap->inactive_mutators;
       mut;
       mut = mut->next)
    trace_mutator_conservative_roots_with_lock(mut);
}

static void
trace_mutator_roots_after_stop(struct gc_heap *heap) {
  struct gc_mutator *mut = atomic_load(&heap->mutator_trace_list);
  int active_mutators_already_marked = heap_should_mark_while_stopping(heap);
  while (mut) {
    // Also collect any already-marked grey objects and put them on the
    // global trace queue.
    if (active_mutators_already_marked)
      gc_tracer_enqueue_roots(&heap->tracer, mut->mark_buf.objects,
                              mut->mark_buf.size);
    else
      trace_mutator_roots_with_lock(mut);
    // Also unlink mutator_trace_list chain.
    struct gc_mutator *next = mut->next;
    mut->next = NULL;
    mut = next;
  }
  atomic_store(&heap->mutator_trace_list, NULL);

  for (struct gc_mutator *mut = heap->inactive_mutators; mut; mut = mut->next)
    trace_mutator_roots_with_lock(mut);
}

static void
trace_global_conservative_roots(struct gc_heap *heap) {
  if (gc_has_global_conservative_roots())
    gc_platform_visit_global_conservative_roots
      (mark_and_globally_enqueue_heap_conservative_roots, heap, NULL);
}

static void
enqueue_generational_root(struct gc_ref ref, struct gc_heap *heap) {
  gc_tracer_enqueue_root(&heap->tracer, ref);
}

void
gc_write_barrier_extern(struct gc_ref obj, size_t obj_size,
                        struct gc_edge edge, struct gc_ref new_val) {
  GC_ASSERT(obj_size > gc_allocator_large_threshold());
  gc_object_set_remembered(obj);
}

static void
trace_generational_roots(struct gc_heap *heap) {
  // TODO: Add lospace nursery.
  if (atomic_load(&heap->gc_kind) == GC_COLLECTION_MINOR) {
    nofl_space_trace_remembered_set(heap_nofl_space(heap),
                                    enqueue_generational_root,
                                    heap);
    large_object_space_trace_remembered_set(heap_large_object_space(heap),
                                            enqueue_generational_root,
                                            heap);
  } else {
    nofl_space_clear_remembered_set(heap_nofl_space(heap));
    large_object_space_clear_remembered_set(heap_large_object_space(heap));
  }
}

static enum gc_collection_kind
pause_mutator_for_collection(struct gc_heap *heap,
                             struct gc_mutator *mut) GC_NEVER_INLINE;
static enum gc_collection_kind
pause_mutator_for_collection(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(!all_mutators_stopped(heap));
  MUTATOR_EVENT(mut, mutator_stopped);
  heap->paused_mutator_count++;
  enum gc_collection_kind collection_kind = heap->gc_kind;
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);

  // Go to sleep and wake up when the collector is done.  Note,
  // however, that it may be that some other mutator manages to
  // trigger collection before we wake up.  In that case we need to
  // mark roots, not just sleep again.  To detect a wakeup on this
  // collection vs a future collection, we use the global GC count.
  // This is safe because the count is protected by the heap lock,
  // which we hold.
  long epoch = heap->count;
  do
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  while (mutators_are_stopping(heap) && heap->count == epoch);

  MUTATOR_EVENT(mut, mutator_restarted);
  return collection_kind;
}

static enum gc_collection_kind
pause_mutator_for_collection_with_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static enum gc_collection_kind
pause_mutator_for_collection_with_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  MUTATOR_EVENT(mut, mutator_stopping);
  nofl_allocator_finish(&mut->allocator, heap_nofl_space(heap));
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    // No need to collect results in mark buf; we can enqueue roots directly.
    trace_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
  return pause_mutator_for_collection(heap, mut);
}

static void pause_mutator_for_collection_without_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static void
pause_mutator_for_collection_without_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  MUTATOR_EVENT(mut, mutator_stopping);
  nofl_finish_sweeping(&mut->allocator, heap_nofl_space(heap));
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    trace_stopping_mutator_roots(mut);
  enqueue_mutator_for_tracing(mut);
  heap_lock(heap);
  pause_mutator_for_collection(heap, mut);
  heap_unlock(heap);
  release_stopping_mutator_roots(mut);
}

static inline void
maybe_pause_mutator_for_collection(struct gc_mutator *mut) {
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static int maybe_grow_heap(struct gc_heap *heap) {
  return 0;
}

static double
heap_last_gc_yield(struct gc_heap *heap) {
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  size_t nofl_yield = nofl_space_yield(nofl_space);
  size_t evacuation_reserve = nofl_space_evacuation_reserve_bytes(nofl_space);
  // FIXME: Size nofl evacuation reserve based on size of nofl space,
  // not heap size.
  size_t minimum_evacuation_reserve =
    heap->size * nofl_space->evacuation_minimum_reserve;
  if (evacuation_reserve > minimum_evacuation_reserve)
    nofl_yield += evacuation_reserve - minimum_evacuation_reserve;
  struct large_object_space *lospace = heap_large_object_space(heap);
  size_t lospace_yield = lospace->pages_freed_by_last_collection;
  lospace_yield <<= lospace->page_size_log2;

  double yield = nofl_yield + lospace_yield;
  return yield / heap->size;
}

static double
heap_fragmentation(struct gc_heap *heap) {
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  size_t fragmentation = nofl_space_fragmentation(nofl_space);
  return ((double)fragmentation) / heap->size;
}

static void
detect_out_of_memory(struct gc_heap *heap) {
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);

  if (heap->count == 0)
    return;

  double last_yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);

  double yield_epsilon = NOFL_BLOCK_SIZE * 1.0 / heap->size;
  double fragmentation_epsilon = LARGE_OBJECT_THRESHOLD * 1.0 / NOFL_BLOCK_SIZE;

  if (last_yield - fragmentation > yield_epsilon)
    return;

  if (fragmentation > fragmentation_epsilon
      && atomic_load(&nofl_space->evacuation_targets.count))
    return;

  // No yield in last gc and we do not expect defragmentation to
  // be able to yield more space: out of memory.
  fprintf(stderr, "ran out of space, heap size %zu (%zu slabs)\n",
          heap->size, nofl_space->nslabs);
  GC_CRASH();
}

static double
clamp_major_gc_yield_threshold(struct gc_heap *heap, double threshold) {
  if (threshold < heap->minimum_major_gc_yield_threshold)
    threshold = heap->minimum_major_gc_yield_threshold;
  double one_block = NOFL_BLOCK_SIZE * 1.0 / heap->size;
  if (threshold < one_block)
    threshold = one_block;
  return threshold;
}

static enum gc_collection_kind
determine_collection_kind(struct gc_heap *heap,
                          enum gc_collection_kind requested) {
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  enum gc_collection_kind previous_gc_kind = atomic_load(&heap->gc_kind);
  enum gc_collection_kind gc_kind;
  int mark_while_stopping = 1;
  double yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);
  ssize_t pending = atomic_load_explicit(&nofl_space->pending_unavailable_bytes,
                                         memory_order_acquire);

  if (heap->count == 0) {
    DEBUG("first collection is always major\n");
    gc_kind = GC_COLLECTION_MAJOR;
  } else if (requested != GC_COLLECTION_ANY) {
    DEBUG("user specifically requested collection kind %d\n", (int)requested);
    gc_kind = requested;
  } else if (pending > 0) {
    DEBUG("evacuating due to need to reclaim %zd bytes\n", pending);
    // During the last cycle, a large allocation could not find enough
    // free blocks, and we decided not to expand the heap.  Let's do an
    // evacuating major collection to maximize the free block yield.
    gc_kind = GC_COLLECTION_COMPACTING;

    // Generally speaking, we allow mutators to mark their own stacks
    // before pausing.  This is a limited form of concurrent marking, as
    // other mutators might be running, not having received the signal
    // to stop yet.  In a compacting collection, this results in pinned
    // roots, because we haven't started evacuating yet and instead mark
    // in place.  However as in this case we are trying to reclaim free
    // blocks, try to avoid any pinning caused by the ragged-stop
    // marking.  Of course if the mutator has conservative roots we will
    // have pinning anyway and might as well allow ragged stops.
    mark_while_stopping = gc_has_conservative_roots();
  } else if (previous_gc_kind == GC_COLLECTION_COMPACTING
             && fragmentation >= heap->fragmentation_low_threshold) {
    DEBUG("continuing evacuation due to fragmentation %.2f%% > %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_low_threshold * 100.);
    // For some reason, we already decided to compact in the past,
    // and fragmentation hasn't yet fallen below a low-water-mark.
    // Keep going.
    gc_kind = GC_COLLECTION_COMPACTING;
  } else if (fragmentation > heap->fragmentation_high_threshold) {
    // Switch to evacuation mode if the heap is too fragmented.
    DEBUG("triggering compaction due to fragmentation %.2f%% > %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_high_threshold * 100.);
    gc_kind = GC_COLLECTION_COMPACTING;
  } else if (previous_gc_kind == GC_COLLECTION_COMPACTING) {
    // We were evacuating, but we're good now.  Go back to minor
    // collections.
    DEBUG("returning to in-place collection, fragmentation %.2f%% < %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_low_threshold * 100.);
    gc_kind = GC_GENERATIONAL ? GC_COLLECTION_MINOR : GC_COLLECTION_MAJOR;
  } else if (!GC_GENERATIONAL) {
    DEBUG("keeping on with major in-place GC\n");
    GC_ASSERT(previous_gc_kind == GC_COLLECTION_MAJOR);
    gc_kind = GC_COLLECTION_MAJOR;
  } else if (previous_gc_kind != GC_COLLECTION_MINOR) {
    DEBUG("returning to minor collection\n");
    // Go back to minor collections.
    gc_kind = GC_COLLECTION_MINOR;
  } else if (yield < heap->major_gc_yield_threshold) {
    DEBUG("collection yield too low, triggering major collection\n");
    // Nursery is getting tight; trigger a major GC.
    gc_kind = GC_COLLECTION_MAJOR;
  } else {
    DEBUG("keeping on with minor GC\n");
    // Nursery has adequate space; keep trucking with minor GCs.
    GC_ASSERT(previous_gc_kind == GC_COLLECTION_MINOR);
    gc_kind = GC_COLLECTION_MINOR;
  }

  if (gc_has_conservative_intraheap_edges() &&
      gc_kind == GC_COLLECTION_COMPACTING) {
    DEBUG("welp.  conservative heap scanning, no evacuation for you\n");
    gc_kind = GC_COLLECTION_MAJOR;
    mark_while_stopping = 1;
  }

  // If this is the first in a series of minor collections, reset the
  // threshold at which we should do a major GC.
  if (gc_kind == GC_COLLECTION_MINOR &&
      previous_gc_kind != GC_COLLECTION_MINOR) {
    double yield = heap_last_gc_yield(heap);
    double threshold = yield * heap->minor_gc_yield_threshold;
    double clamped = clamp_major_gc_yield_threshold(heap, threshold);
    heap->major_gc_yield_threshold = clamped;
    DEBUG("first minor collection at yield %.2f%%, threshold %.2f%%\n",
          yield * 100., clamped * 100.);
  }

  atomic_store_explicit(&heap->mark_while_stopping, mark_while_stopping,
                        memory_order_release);

  atomic_store(&heap->gc_kind, gc_kind);
  return gc_kind;
}

static void
trace_conservative_roots_after_stop(struct gc_heap *heap) {
  GC_ASSERT(!heap_nofl_space(heap)->evacuating);
  if (gc_has_mutator_conservative_roots())
    trace_mutator_conservative_roots_after_stop(heap);
  if (gc_has_global_conservative_roots())
    trace_global_conservative_roots(heap);
}

static void
trace_pinned_roots_after_stop(struct gc_heap *heap) {
  GC_ASSERT(!heap_nofl_space(heap)->evacuating);
  trace_conservative_roots_after_stop(heap);
}

static void
trace_roots_after_stop(struct gc_heap *heap) {
  trace_mutator_roots_after_stop(heap);
  gc_trace_heap_roots(heap->roots, trace_and_enqueue_globally, heap, NULL);
  gc_visit_finalizer_roots(heap->finalizer_state, visit_root_edge, heap, NULL);
  trace_generational_roots(heap);
}

static void
resolve_ephemerons_lazily(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 0,
                        memory_order_release);
}

static void
resolve_ephemerons_eagerly(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 1,
                        memory_order_release);
  gc_scan_pending_ephemerons(heap->pending_ephemerons, heap, 0, 1);
}

static int
enqueue_resolved_ephemerons(struct gc_heap *heap) {
  struct gc_ephemeron *resolved = gc_pop_resolved_ephemerons(heap);
  if (!resolved)
    return 0;
  gc_trace_resolved_ephemerons(resolved, trace_and_enqueue_globally, heap, NULL);
  return 1;
}

static void
trace_resolved_ephemerons(struct gc_heap *heap) {
  for (struct gc_ephemeron *resolved = gc_pop_resolved_ephemerons(heap);
       resolved;
       resolved = gc_pop_resolved_ephemerons(heap)) {
    gc_tracer_add_root(&heap->tracer, gc_root_resolved_ephemerons(resolved));
    gc_tracer_trace(&heap->tracer);
  }
}

static void
resolve_finalizers(struct gc_heap *heap) {
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

static void
sweep_ephemerons(struct gc_heap *heap) {
  return gc_sweep_pending_ephemerons(heap->pending_ephemerons, 0, 1);
}

static void
collect(struct gc_mutator *mut, enum gc_collection_kind requested_kind) {
  struct gc_heap *heap = mutator_heap(mut);
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);
  struct gc_extern_space *exspace = heap_extern_space(heap);
  if (maybe_grow_heap(heap)) {
    DEBUG("grew heap instead of collecting #%ld:\n", heap->count);
    return;
  }
  MUTATOR_EVENT(mut, mutator_cause_gc);
  DEBUG("start collect #%ld:\n", heap->count);
  enum gc_collection_kind gc_kind =
    determine_collection_kind(heap, requested_kind);
  int is_minor = gc_kind == GC_COLLECTION_MINOR;
  HEAP_EVENT(heap, prepare_gc, gc_kind);
  nofl_space_prepare_gc(nofl_space, gc_kind);
  large_object_space_start_gc(lospace, is_minor);
  gc_extern_space_start_gc(exspace, is_minor);
  resolve_ephemerons_lazily(heap);
  gc_tracer_prepare(&heap->tracer);
  HEAP_EVENT(heap, requesting_stop);
  request_mutators_to_stop(heap);
  trace_mutator_roots_with_lock_before_stop(mut);
  nofl_finish_sweeping(&mut->allocator, nofl_space);
  HEAP_EVENT(heap, waiting_for_stop);
  wait_for_mutators_to_stop(heap);
  HEAP_EVENT(heap, mutators_stopped);
  double yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);
  HEAP_EVENT(heap, live_data_size, heap->size * (1 - yield));
  DEBUG("last gc yield: %f; fragmentation: %f\n", yield, fragmentation);
  detect_out_of_memory(heap);
  trace_pinned_roots_after_stop(heap);
  nofl_space_start_gc(nofl_space, gc_kind);
  trace_roots_after_stop(heap);
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
  nofl_space_finish_gc(nofl_space, gc_kind);
  large_object_space_finish_gc(lospace, is_minor);
  gc_extern_space_finish_gc(exspace, is_minor);
  heap->count++;
  heap->last_collection_was_minor = is_minor;
  heap_reset_large_object_pages(heap, lospace->live_pages_at_last_collection);
  HEAP_EVENT(heap, restarting_mutators);
  allow_mutators_to_continue(heap);
}

static void
trigger_collection(struct gc_mutator *mut,
                   enum gc_collection_kind requested_kind) {
  struct gc_heap *heap = mutator_heap(mut);
  int prev_kind = -1;
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    prev_kind = pause_mutator_for_collection_with_lock(mut);
  if (prev_kind < (int)requested_kind)
    collect(mut, requested_kind);
  heap_unlock(heap);
}

void
gc_collect(struct gc_mutator *mut, enum gc_collection_kind kind) {
  trigger_collection(mut, kind);
}

static void*
allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct nofl_space *nofl_space = heap_nofl_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);

  size_t npages = large_object_space_npages(lospace, size);

  nofl_space_request_release_memory(nofl_space,
                                    npages << lospace->page_size_log2);

  while (!nofl_space_sweep_until_memory_released(nofl_space,
                                                 &mut->allocator))
    trigger_collection(mut, GC_COLLECTION_COMPACTING);
  atomic_fetch_add(&heap->large_object_pages, npages);

  void *ret = large_object_space_alloc(lospace, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(lospace, npages);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    GC_CRASH();
  }

  return ret;
}

static void
collect_for_small_allocation(void *mut) {
  trigger_collection(mut, GC_COLLECTION_ANY);
}

void*
gc_allocate_slow(struct gc_mutator *mut, size_t size) {
  GC_ASSERT(size > 0); // allocating 0 bytes would be silly

  if (size > gc_allocator_large_threshold())
    return allocate_large(mut, size);

  return gc_ref_heap_object(nofl_allocate(&mut->allocator,
                                          heap_nofl_space(mutator_heap(mut)),
                                          size, collect_for_small_allocation,
                                          mut));
}

void*
gc_allocate_pointerless(struct gc_mutator *mut, size_t size) {
  return gc_allocate(mut, size);
}

struct gc_ephemeron*
gc_allocate_ephemeron(struct gc_mutator *mut) {
  struct gc_ref ret =
    gc_ref_from_heap_object(gc_allocate(mut, gc_ephemeron_size()));
  nofl_space_set_ephemeron_flag(ret);
  return gc_ref_heap_object(ret);
}

void
gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                  struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
  // No write barrier: we require that the ephemeron be newer than the
  // key or the value.
}

struct gc_pending_ephemerons *
gc_heap_pending_ephemerons(struct gc_heap *heap) {
  return heap->pending_ephemerons;
}

unsigned
gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return heap->count;
}

struct gc_finalizer*
gc_allocate_finalizer(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_finalizer_size());
}

void
gc_finalizer_attach(struct gc_mutator *mut, struct gc_finalizer *finalizer,
                    unsigned priority, struct gc_ref object,
                    struct gc_ref closure) {
  gc_finalizer_init_internal(finalizer, object, closure);
  gc_finalizer_attach_internal(mutator_heap(mut)->finalizer_state,
                               finalizer, priority);
  // No write barrier.
}

struct gc_finalizer*
gc_pop_finalizable(struct gc_mutator *mut) {
  return gc_finalizer_state_pop(mutator_heap(mut)->finalizer_state);
}

void
gc_set_finalizer_callback(struct gc_heap *heap,
                               gc_finalizer_callback callback) {
  gc_finalizer_state_set_callback(heap->finalizer_state, callback);
}

static int heap_prepare_pending_ephemerons(struct gc_heap *heap) {
  struct gc_pending_ephemerons *cur = heap->pending_ephemerons;
  size_t target = heap->size * heap->pending_ephemerons_size_factor;
  double slop = heap->pending_ephemerons_size_slop;

  heap->pending_ephemerons = gc_prepare_pending_ephemerons(cur, target, slop);

  return !!heap->pending_ephemerons;
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

static int heap_init(struct gc_heap *heap, const struct gc_options *options) {
  // *heap is already initialized to 0.

  pthread_mutex_init(&heap->lock, NULL);
  pthread_cond_init(&heap->mutator_cond, NULL);
  pthread_cond_init(&heap->collector_cond, NULL);
  heap->size = options->common.heap_size;

  if (!gc_tracer_init(&heap->tracer, heap, options->common.parallelism))
    GC_CRASH();

  heap->pending_ephemerons_size_factor = 0.005;
  heap->pending_ephemerons_size_slop = 0.5;
  heap->fragmentation_low_threshold = 0.05;
  heap->fragmentation_high_threshold = 0.10;
  heap->minor_gc_yield_threshold = 0.30;
  heap->minimum_major_gc_yield_threshold = 0.05;
  heap->major_gc_yield_threshold =
    clamp_major_gc_yield_threshold(heap, heap->minor_gc_yield_threshold);

  if (!heap_prepare_pending_ephemerons(heap))
    GC_CRASH();

  heap->finalizer_state = gc_make_finalizer_state();
  if (!heap->finalizer_state)
    GC_CRASH();

  return 1;
}

int gc_init(const struct gc_options *options, struct gc_stack_addr *stack_base,
            struct gc_heap **heap, struct gc_mutator **mut,
            struct gc_event_listener event_listener,
            void *event_listener_data) {
  GC_ASSERT_EQ(gc_allocator_small_granule_size(), NOFL_GRANULE_SIZE);
  GC_ASSERT_EQ(gc_allocator_large_threshold(), LARGE_OBJECT_THRESHOLD);
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct nofl_allocator, alloc));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct nofl_allocator, sweep));
  GC_ASSERT_EQ(gc_allocator_alloc_table_alignment(), NOFL_SLAB_SIZE);
  GC_ASSERT_EQ(gc_allocator_alloc_table_begin_pattern(), NOFL_METADATA_BYTE_YOUNG);
  GC_ASSERT_EQ(gc_allocator_alloc_table_end_pattern(), NOFL_METADATA_BYTE_END);
  if (GC_GENERATIONAL) {
    GC_ASSERT_EQ(gc_write_barrier_card_table_alignment(), NOFL_SLAB_SIZE);
    GC_ASSERT_EQ(gc_write_barrier_card_size(),
                 NOFL_BLOCK_SIZE / NOFL_REMSET_BYTES_PER_BLOCK);
  }

  if (options->common.heap_size_policy != GC_HEAP_SIZE_FIXED) {
    fprintf(stderr, "fixed heap size is currently required\n");
    return 0;
  }

  *heap = calloc(1, sizeof(struct gc_heap));
  if (!*heap) GC_CRASH();

  if (!heap_init(*heap, options))
    GC_CRASH();

  (*heap)->event_listener = event_listener;
  (*heap)->event_listener_data = event_listener_data;
  HEAP_EVENT(*heap, init, (*heap)->size);

  struct nofl_space *space = heap_nofl_space(*heap);
  if (!nofl_space_init(space, (*heap)->size,
                       options->common.parallelism != 1,
                       (*heap)->fragmentation_low_threshold)) {
    free(*heap);
    *heap = NULL;
    return 0;
  }
  
  if (!large_object_space_init(heap_large_object_space(*heap), *heap))
    GC_CRASH();

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  gc_stack_init(&(*mut)->stack, stack_base);
  add_mutator(*heap, *mut);
  return 1;
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr *stack_base,
                                      struct gc_heap *heap) {
  struct gc_mutator *ret = calloc(1, sizeof(struct gc_mutator));
  if (!ret)
    GC_CRASH();
  gc_stack_init(&ret->stack, stack_base);
  add_mutator(heap, ret);
  return ret;
}

void gc_finish_for_thread(struct gc_mutator *mut) {
  remove_mutator(mutator_heap(mut), mut);
  mutator_mark_buf_destroy(&mut->mark_buf);
  free(mut);
}

static void deactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(mut->next == NULL);
  nofl_allocator_finish(&mut->allocator, heap_nofl_space(heap));
  heap_lock(heap);
  mut->next = heap->inactive_mutators;
  heap->inactive_mutators = mut;
  heap->inactive_mutator_count++;
  gc_stack_capture_hot(&mut->stack);
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void reactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  struct gc_mutator **prev = &heap->inactive_mutators;
  while (*prev != mut)
    prev = &(*prev)->next;
  *prev = mut->next;
  mut->next = NULL;
  heap->inactive_mutator_count--;
  heap_unlock(heap);
}

void* gc_call_without_gc(struct gc_mutator *mut,
                         void* (*f)(void*),
                         void *data) {
  struct gc_heap *heap = mutator_heap(mut);
  deactivate_mutator(heap, mut);
  void *ret = f(data);
  reactivate_mutator(heap, mut);
  return ret;
}
