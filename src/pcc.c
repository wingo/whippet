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
#include "gc-trace.h"
#include "large-object-space.h"
#include "parallel-tracer.h"
#include "spin.h"
#include "pcc-attrs.h"

#define SLAB_SIZE (64 * 1024 * 1024)
#define REGION_SIZE (64 * 1024)
#define BLOCK_SIZE (2 * REGION_SIZE)
#define BLOCKS_PER_SLAB (SLAB_SIZE / BLOCK_SIZE)
#define HEADER_BYTES_PER_BLOCK (BLOCK_SIZE / BLOCKS_PER_SLAB)
#define HEADER_BLOCKS_PER_SLAB 1
#define NONHEADER_BLOCKS_PER_SLAB (BLOCKS_PER_SLAB - HEADER_BLOCKS_PER_SLAB)
#define HEADER_BYTES_PER_SLAB (HEADER_BYTES_PER_BLOCK * HEADER_BLOCKS_PER_SLAB)

struct pcc_slab;
struct pcc_block;

struct pcc_slab_header {
  union {
    struct {
      struct pcc_slab *next;
      struct pcc_slab *prev;
      unsigned incore_block_count;
    };
    uint8_t padding[HEADER_BYTES_PER_SLAB];
  };
};
STATIC_ASSERT_EQ(sizeof(struct pcc_slab_header),
                 HEADER_BYTES_PER_SLAB);

// Really just the block header.
struct pcc_block {
  union {
    struct {
      struct pcc_block *next;
      uint8_t in_core;
      size_t allocated; // For partially-allocated blocks.
    };
    uint8_t padding[HEADER_BYTES_PER_BLOCK];
  };
};
STATIC_ASSERT_EQ(sizeof(struct pcc_block),
                 HEADER_BYTES_PER_BLOCK);

struct pcc_region {
  char data[REGION_SIZE];
};

struct pcc_block_payload {
  struct pcc_region regions[2];
};

struct pcc_slab {
  struct pcc_slab_header header;
  struct pcc_block headers[NONHEADER_BLOCKS_PER_SLAB];
  struct pcc_block_payload blocks[NONHEADER_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct pcc_slab), SLAB_SIZE);

static struct pcc_block *block_header(struct pcc_block_payload *payload) {
  uintptr_t addr = (uintptr_t) payload;
  uintptr_t base = align_down(addr, SLAB_SIZE);
  struct pcc_slab *slab = (struct pcc_slab*) base;
  uintptr_t block_idx = (addr / BLOCK_SIZE) % BLOCKS_PER_SLAB;
  return &slab->headers[block_idx - HEADER_BLOCKS_PER_SLAB];
}

static struct pcc_block_payload *block_payload(struct pcc_block *block) {
  uintptr_t addr = (uintptr_t) block;
  uintptr_t base = align_down(addr, SLAB_SIZE);
  struct pcc_slab *slab = (struct pcc_slab*) base;
  uintptr_t block_idx = (addr / HEADER_BYTES_PER_BLOCK) % BLOCKS_PER_SLAB;
  return &slab->blocks[block_idx - HEADER_BLOCKS_PER_SLAB];
}

static uint8_t pcc_object_region(struct gc_ref obj) {
  return (gc_ref_value(obj) / REGION_SIZE) & 1;
}

struct pcc_extent {
  uintptr_t low_addr;
  uintptr_t high_addr;
};

struct pcc_space {
  struct pcc_block *available;
  struct pcc_block *partially_allocated;
  struct pcc_block *allocated ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t allocated_block_count;
  struct pcc_block *paged_out ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t fragmentation ALIGNED_TO_AVOID_FALSE_SHARING;
  ssize_t bytes_to_page_out ALIGNED_TO_AVOID_FALSE_SHARING;
  // The rest of these members are only changed rarely and with the heap
  // lock.
  uint8_t active_region ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t live_bytes_at_last_gc;
  size_t fragmentation_at_last_gc;
  struct pcc_extent *extents;
  size_t nextents;
  struct pcc_slab *slabs;
  size_t nslabs;
};

struct gc_heap {
  struct pcc_space pcc_space;
  struct large_object_space large_object_space;
  struct gc_extern_space *extern_space;
  size_t large_object_pages;
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  size_t size;
  int collecting;
  int check_pending_ephemerons;
  struct gc_pending_ephemerons *pending_ephemerons;
  size_t mutator_count;
  size_t paused_mutator_count;
  size_t inactive_mutator_count;
  struct gc_heap_roots *roots;
  struct gc_mutator *mutators;
  long count;
  struct gc_tracer tracer;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
  struct gc_event_listener event_listener;
  void *event_listener_data;
};

#define HEAP_EVENT(heap, event, ...)                                    \
  (heap)->event_listener.event((heap)->event_listener_data, ##__VA_ARGS__)
#define MUTATOR_EVENT(mut, event, ...)                                  \
  (mut)->heap->event_listener.event((mut)->event_listener_data, ##__VA_ARGS__)

struct gc_mutator {
  uintptr_t hp;
  uintptr_t limit;
  struct pcc_block *block;
  struct gc_heap *heap;
  struct gc_mutator_roots *roots;
  void *event_listener_data;
  struct gc_mutator *next;
  struct gc_mutator *prev;
};

struct gc_trace_worker_data {
  uintptr_t hp;
  uintptr_t limit;
  struct pcc_block *block;
};

static inline struct pcc_space* heap_pcc_space(struct gc_heap *heap) {
  return &heap->pcc_space;
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

static inline void pcc_space_compute_region(struct pcc_space *space,
                                            struct pcc_block *block,
                                            uintptr_t *hp, uintptr_t *limit) {
  struct pcc_block_payload *payload = block_payload(block);
  struct pcc_region *region = &payload->regions[space->active_region];
  *hp = (uintptr_t)&region[0];
  *limit = (uintptr_t)&region[1];
}

static void push_block(struct pcc_block **list,
                       struct pcc_block *block) {
  struct pcc_block *next = atomic_load_explicit(list, memory_order_acquire);
  do {
    block->next = next;
  } while (!atomic_compare_exchange_weak(list, &next, block));
}

static struct pcc_block* pop_block(struct pcc_block **list) {
  struct pcc_block *head = atomic_load_explicit(list, memory_order_acquire);
  struct pcc_block *next;
  do {
    if (!head)
      return NULL;
  } while (!atomic_compare_exchange_weak(list, &head, head->next));
  head->next = NULL;
  return head;
}

static struct pcc_block* pop_available_block(struct pcc_space *space) {
  return pop_block(&space->available);
}
static void push_available_block(struct pcc_space *space,
                                 struct pcc_block *block) {
  push_block(&space->available, block);
}

static struct pcc_block* pop_allocated_block(struct pcc_space *space) {
  return pop_block(&space->allocated);
}
static void push_allocated_block(struct pcc_space *space,
                                 struct pcc_block *block) {
  push_block(&space->allocated, block);
  atomic_fetch_add_explicit(&space->allocated_block_count, 1,
                            memory_order_relaxed);
}

static struct pcc_block* pop_partially_allocated_block(struct pcc_space *space) {
  return pop_block(&space->partially_allocated);
}
static void push_partially_allocated_block(struct pcc_space *space,
                                           struct pcc_block *block,
                                           uintptr_t hp) {
  block->allocated = hp & (REGION_SIZE - 1);
  GC_ASSERT(block->allocated);
  push_block(&space->partially_allocated, block);
}

static struct pcc_block* pop_paged_out_block(struct pcc_space *space) {
  return pop_block(&space->paged_out);
}
static void push_paged_out_block(struct pcc_space *space,
                                 struct pcc_block *block) {
  push_block(&space->paged_out, block);
}

static void page_out_block(struct pcc_space *space,
                           struct pcc_block *block) {
  block->in_core = 0;
  madvise(block_payload(block), BLOCK_SIZE, MADV_DONTNEED);
  push_paged_out_block(space, block);
}

static struct pcc_block* page_in_block(struct pcc_space *space) {
  struct pcc_block* block = pop_paged_out_block(space);
  if (block) block->in_core = 1;
  return block;
}

static void record_fragmentation(struct pcc_space *space,
                                 size_t bytes) {
  atomic_fetch_add_explicit(&space->fragmentation, bytes,
                            memory_order_relaxed);
}

static ssize_t pcc_space_request_release_memory(struct pcc_space *space,
                                                  size_t bytes) {
  return atomic_fetch_add(&space->bytes_to_page_out, bytes) + bytes;
}

static int
pcc_space_page_out_blocks_until_memory_released(struct pcc_space *space) {
  ssize_t pending = atomic_load(&space->bytes_to_page_out);
  while (pending > 0) {
    struct pcc_block *block = pop_available_block(space);
    if (!block) return 0;
    page_out_block(space, block);
    pending =
      atomic_fetch_sub(&space->bytes_to_page_out, BLOCK_SIZE) - BLOCK_SIZE;
  }
  return 1;
}

static void pcc_space_reacquire_memory(struct pcc_space *space,
                                       size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->bytes_to_page_out, bytes) - bytes;
  while (pending + BLOCK_SIZE <= 0) {
    struct pcc_block *block = page_in_block(space);
    GC_ASSERT(block);
    push_available_block(space, block);
    pending =
      atomic_fetch_add(&space->bytes_to_page_out, BLOCK_SIZE) + BLOCK_SIZE;
  }
}

static void
gc_trace_worker_call_with_data(void (*f)(struct gc_tracer *tracer,
                                         struct gc_heap *heap,
                                         struct gc_trace_worker *worker,
                                         struct gc_trace_worker_data *data),
                               struct gc_tracer *tracer,
                               struct gc_heap *heap,
                               struct gc_trace_worker *worker) {
  struct gc_trace_worker_data data = {0,0,NULL};
  f(tracer, heap, worker, &data);
  if (data.block)
    push_partially_allocated_block(heap_pcc_space(heap), data.block,
                                   data.hp);
  // FIXME: Add (data.limit - data.hp) to fragmentation.
}

static void clear_mutator_allocation_buffers(struct gc_heap *heap) {
  for (struct gc_mutator *mut = heap->mutators; mut; mut = mut->next) {
    if (mut->block) {
      record_fragmentation(heap_pcc_space(heap), mut->limit - mut->hp);
      push_allocated_block(heap_pcc_space(heap), mut->block);
      mut->block = NULL;
    }
    mut->hp = mut->limit = 0;
  }
}

static struct pcc_block*
append_block_lists(struct pcc_block *head, struct pcc_block *tail) {
  if (!head) return tail;
  if (tail) {
    struct pcc_block *walk = head;
    while (walk->next)
      walk = walk->next;
    walk->next = tail;
  }
  return head;
}

static void pcc_space_flip(struct pcc_space *space) {
  // Mutators stopped, can access nonatomically.
  struct pcc_block *available = space->available;
  struct pcc_block *partially_allocated = space->partially_allocated;
  struct pcc_block *allocated = space->allocated;
  allocated = append_block_lists(partially_allocated, allocated);
  space->available = append_block_lists(available, allocated);
  space->partially_allocated = NULL;
  space->allocated = NULL;
  space->allocated_block_count = 0;
  space->fragmentation = 0;
  space->active_region ^= 1;
}

static void pcc_space_finish_gc(struct pcc_space *space) {
  // Mutators stopped, can access nonatomically.
  space->live_bytes_at_last_gc = space->allocated_block_count * REGION_SIZE;
  space->fragmentation_at_last_gc = space->fragmentation;
}

static void collect(struct gc_mutator *mut) GC_NEVER_INLINE;

static struct gc_ref evacuation_allocate(struct pcc_space *space,
                                         struct gc_trace_worker_data *data,
                                         size_t size) {
  GC_ASSERT(size > 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());
  size = align_up(size, GC_ALIGNMENT);

  uintptr_t hp = data->hp;
  uintptr_t limit = data->limit;
  uintptr_t new_hp = hp + size;
  if (new_hp <= limit)
    goto done;

  if (data->block) {
    record_fragmentation(space, limit - hp);
    push_allocated_block(space, data->block);
  }
  while ((data->block = pop_partially_allocated_block(space))) {
    pcc_space_compute_region(space, data->block, &hp, &limit);
    hp += data->block->allocated;
    new_hp = hp + size;
    if (new_hp <= limit) {
      data->limit = limit;
      goto done;
    }
    record_fragmentation(space, limit - hp);
    push_allocated_block(space, data->block);
  }
  data->block = pop_available_block(space);
  if (!data->block) {
    // Can happen if space is really tight and reordering of objects
    // during evacuation resulted in more end-of-block fragmentation and
    // thus block use than before collection started.  A dire situation.
    fprintf(stderr, "Out of memory\n");
    GC_CRASH();
  }
  pcc_space_compute_region(space, data->block, &hp, &data->limit);
  new_hp = hp + size;
  // The region is empty and is therefore large enough for a small
  // allocation.

done:
  data->hp = new_hp;
  return gc_ref(hp);
}

static inline int pcc_space_forward(struct pcc_space *space,
                                    struct gc_edge edge,
                                    struct gc_ref old_ref,
                                    struct gc_trace_worker_data *data) {
  GC_ASSERT(pcc_object_region(old_ref) != space->active_region);
  struct gc_atomic_forward fwd = gc_atomic_forward_begin(old_ref);

  if (fwd.state == GC_FORWARDING_STATE_NOT_FORWARDED)
    gc_atomic_forward_acquire(&fwd);

  switch (fwd.state) {
  case GC_FORWARDING_STATE_NOT_FORWARDED:
  case GC_FORWARDING_STATE_ABORTED:
  default:
    // Impossible.
    GC_CRASH();
  case GC_FORWARDING_STATE_ACQUIRED: {
    // We claimed the object successfully; evacuating is up to us.
    size_t bytes = gc_atomic_forward_object_size(&fwd);
    struct gc_ref new_ref = evacuation_allocate(space, data, bytes);
    // Copy object contents before committing, as we don't know what
    // part of the object (if any) will be overwritten by the
    // commit.
    memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(old_ref), bytes);
    gc_atomic_forward_commit(&fwd, new_ref);
    gc_edge_update(edge, new_ref);
    return 1;
  }
  case GC_FORWARDING_STATE_BUSY:
    // Someone else claimed this object first.  Spin until new address
    // known, or evacuation aborts.
    for (size_t spin_count = 0;; spin_count++) {
      if (gc_atomic_forward_retry_busy(&fwd))
        break;
      yield_for_spin(spin_count);
    }
    GC_ASSERT(fwd.state == GC_FORWARDING_STATE_FORWARDED);
    // Fall through.
  case GC_FORWARDING_STATE_FORWARDED:
    // The object has been evacuated already.  Update the edge;
    // whoever forwarded the object will make sure it's eventually
    // traced.
    gc_edge_update(edge, gc_ref(gc_atomic_forward_address(&fwd)));
    return 0;
  }
}

static inline int pcc_space_contains(struct pcc_space *space,
                                     struct gc_ref ref) {

  for (size_t i = 0; i < space->nextents; i++)
    if (space->extents[i].low_addr <= gc_ref_value(ref) &&
        gc_ref_value(ref) < space->extents[i].high_addr)
      return 1;
  return 0;
}

static inline int do_trace(struct gc_heap *heap, struct gc_edge edge,
                           struct gc_ref ref,
                           struct gc_trace_worker_data *data) {
  if (!gc_ref_is_heap_object(ref))
    return 0;
  if (GC_LIKELY(pcc_space_contains(heap_pcc_space(heap), ref)))
    return pcc_space_forward(heap_pcc_space(heap), edge, ref, data);
  else if (large_object_space_contains(heap_large_object_space(heap), ref))
    return large_object_space_mark_object(heap_large_object_space(heap), ref);
  else
    return gc_extern_space_visit(heap_extern_space(heap), edge, ref);
}

static inline int trace_edge(struct gc_heap *heap, struct gc_edge edge,
                             struct gc_trace_worker *worker) {
  struct gc_ref ref = gc_edge_ref(edge);
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
  if (!gc_ref_is_heap_object(ref))
    return 0;
  if (GC_LIKELY(pcc_space_contains(heap_pcc_space(heap), ref))) {
    struct gc_atomic_forward fwd = gc_atomic_forward_begin(ref);
    switch (fwd.state) {
    case GC_FORWARDING_STATE_NOT_FORWARDED:
      return 0;
    case GC_FORWARDING_STATE_BUSY:
      // Someone else claimed this object first.  Spin until new address
      // known.
      for (size_t spin_count = 0;; spin_count++) {
        if (gc_atomic_forward_retry_busy(&fwd))
          break;
        yield_for_spin(spin_count);
      }
      GC_ASSERT(fwd.state == GC_FORWARDING_STATE_FORWARDED);
      // Fall through.
    case GC_FORWARDING_STATE_FORWARDED:
      gc_edge_update(edge, gc_ref(gc_atomic_forward_address(&fwd)));
      return 1;
    default:
      GC_CRASH();
    }
  } else if (large_object_space_contains(heap_large_object_space(heap), ref)) {
    return large_object_space_is_copied(heap_large_object_space(heap), ref);
  }
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

static void add_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  mut->heap = heap;
  mut->event_listener_data =
    heap->event_listener.mutator_added(heap->event_listener_data);
  heap_lock(heap);
  mut->block = NULL;
  // We have no roots.  If there is a GC currently in progress, we have
  // nothing to add.  Just wait until it's done.
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  mut->next = mut->prev = NULL;
  struct gc_mutator *tail = heap->mutators;
  if (tail) {
    mut->next = tail;
    tail->prev = mut;
  }
  heap->mutators = mut;
  heap->mutator_count++;
  heap_unlock(heap);
}

static void remove_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  MUTATOR_EVENT(mut, mutator_removed);
  mut->heap = NULL;
  if (mut->block) {
    push_partially_allocated_block(heap_pcc_space(heap), mut->block,
                                   mut->hp);
    mut->block = NULL;
  }
  heap_lock(heap);
  heap->mutator_count--;
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

static void request_mutators_to_stop(struct gc_heap *heap) {
  GC_ASSERT(!mutators_are_stopping(heap));
  atomic_store_explicit(&heap->collecting, 1, memory_order_relaxed);
}

static void allow_mutators_to_continue(struct gc_heap *heap) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(all_mutators_stopped(heap));
  heap->paused_mutator_count = 0;
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
  pcc_space_reacquire_memory(heap_pcc_space(heap), bytes);
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

static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             struct gc_trace_worker *worker) {
#ifdef DEBUG
  if (pcc_space_contains(heap_pcc_space(heap), ref))
    GC_ASSERT(pcc_object_region(ref) == heap_pcc_space(heap)->active_region);
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
  default:
    GC_CRASH();
  }
}

static void wait_for_mutators_to_stop(struct gc_heap *heap) {
  heap->paused_mutator_count++;
  while (!all_mutators_stopped(heap))
    pthread_cond_wait(&heap->collector_cond, &heap->lock);
}

void gc_write_barrier_extern(struct gc_ref obj, size_t obj_size,
                             struct gc_edge edge, struct gc_ref new_val) {
}

static void
pause_mutator_for_collection(struct gc_heap *heap,
                             struct gc_mutator *mut) GC_NEVER_INLINE;
static void
pause_mutator_for_collection(struct gc_heap *heap, struct gc_mutator *mut) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(!all_mutators_stopped(heap));
  MUTATOR_EVENT(mut, mutator_stopped);
  heap->paused_mutator_count++;
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);

  do {
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  } while (mutators_are_stopping(heap));

  MUTATOR_EVENT(mut, mutator_restarted);
}

static void
pause_mutator_for_collection_with_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static void
pause_mutator_for_collection_with_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  MUTATOR_EVENT(mut, mutator_stopping);
  pause_mutator_for_collection(heap, mut);
}

static void pause_mutator_for_collection_without_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static void pause_mutator_for_collection_without_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  MUTATOR_EVENT(mut, mutator_stopping);
  heap_lock(heap);
  pause_mutator_for_collection(heap, mut);
  heap_unlock(heap);
}

static inline void maybe_pause_mutator_for_collection(struct gc_mutator *mut) {
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static int maybe_grow_heap(struct gc_heap *heap) {
  return 0;
}

static void add_roots(struct gc_heap *heap) {
  for (struct gc_mutator *mut = heap->mutators; mut; mut = mut->next)
    gc_tracer_add_root(&heap->tracer, gc_root_mutator(mut));
  gc_tracer_add_root(&heap->tracer, gc_root_heap(heap));
}

static void resolve_ephemerons_lazily(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 0,
                        memory_order_release);
}

static void resolve_ephemerons_eagerly(struct gc_heap *heap) {
  atomic_store_explicit(&heap->check_pending_ephemerons, 1,
                        memory_order_release);
  gc_scan_pending_ephemerons(heap->pending_ephemerons, heap, 0, 1);
}

static int enqueue_resolved_ephemerons(struct gc_heap *heap) {
  struct gc_ephemeron *resolved = gc_pop_resolved_ephemerons(heap);
  if (!resolved)
    return 0;
  gc_tracer_add_root(&heap->tracer, gc_root_resolved_ephemerons(resolved));
  return 1;
}

static void sweep_ephemerons(struct gc_heap *heap) {
  return gc_sweep_pending_ephemerons(heap->pending_ephemerons, 0, 1);
}

static void collect(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  struct pcc_space *cspace = heap_pcc_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);
  struct gc_extern_space *exspace = heap_extern_space(heap);
  MUTATOR_EVENT(mut, mutator_cause_gc);
  DEBUG("start collect #%ld:\n", heap->count);
  HEAP_EVENT(heap, prepare_gc, GC_COLLECTION_COMPACTING);
  large_object_space_start_gc(lospace, 0);
  gc_extern_space_start_gc(exspace, 0);
  resolve_ephemerons_lazily(heap);
  HEAP_EVENT(heap, requesting_stop);
  request_mutators_to_stop(heap);
  HEAP_EVENT(heap, waiting_for_stop);
  wait_for_mutators_to_stop(heap);
  HEAP_EVENT(heap, mutators_stopped);
  clear_mutator_allocation_buffers(heap);
  pcc_space_flip(cspace);
  gc_tracer_prepare(&heap->tracer);
  add_roots(heap);
  HEAP_EVENT(heap, roots_traced);
  gc_tracer_trace(&heap->tracer);
  HEAP_EVENT(heap, heap_traced);
  resolve_ephemerons_eagerly(heap);
  while (enqueue_resolved_ephemerons(heap))
    gc_tracer_trace(&heap->tracer);
  HEAP_EVENT(heap, ephemerons_traced);
  sweep_ephemerons(heap);
  gc_tracer_release(&heap->tracer);
  pcc_space_finish_gc(cspace);
  large_object_space_finish_gc(lospace, 0);
  gc_extern_space_finish_gc(exspace, 0);
  heap->count++;
  heap_reset_large_object_pages(heap, lospace->live_pages_at_last_collection);
  size_t live_size = (cspace->live_bytes_at_last_gc +
                      large_object_space_size_at_last_collection(lospace));
  HEAP_EVENT(heap, live_data_size, live_size);
  maybe_grow_heap(heap);
  if (!pcc_space_page_out_blocks_until_memory_released(cspace)) {
    fprintf(stderr, "ran out of space, heap size %zu (%zu slabs)\n",
            heap->size, cspace->nslabs);
    GC_CRASH();
  }
  HEAP_EVENT(heap, restarting_mutators);
  allow_mutators_to_continue(heap);
}

static void trigger_collection(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  heap_lock(heap);
  long epoch = heap->count;
  while (mutators_are_stopping(heap))
    pause_mutator_for_collection_with_lock(mut);
  if (epoch == heap->count)
    collect(mut);
  heap_unlock(heap);
}

void gc_collect(struct gc_mutator *mut, enum gc_collection_kind kind) {
  trigger_collection(mut);
}

static void* allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);

  size_t npages = large_object_space_npages(space, size);

  pcc_space_request_release_memory(heap_pcc_space(heap),
                                     npages << space->page_size_log2);
  while (!pcc_space_page_out_blocks_until_memory_released(heap_pcc_space(heap)))
    trigger_collection(mut);
  atomic_fetch_add(&heap->large_object_pages, npages);

  void *ret = large_object_space_alloc(space, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(space, npages);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    GC_CRASH();
  }

  return ret;
}

void* gc_allocate_slow(struct gc_mutator *mut, size_t size) {
  GC_ASSERT(size > 0); // allocating 0 bytes would be silly

  if (size > gc_allocator_large_threshold())
    return allocate_large(mut, size);

  size = align_up(size, GC_ALIGNMENT);
  uintptr_t hp = mut->hp;
  uintptr_t limit = mut->limit;
  uintptr_t new_hp = hp + size;
  if (new_hp <= limit)
    goto done;

  struct pcc_space *space = heap_pcc_space(mutator_heap(mut));
  if (mut->block) {
    record_fragmentation(space, limit - hp);
    push_allocated_block(space, mut->block);
  }
  while ((mut->block = pop_partially_allocated_block(space))) {
    pcc_space_compute_region(space, mut->block, &hp, &limit);
    hp += mut->block->allocated;
    new_hp = hp + size;
    if (new_hp <= limit) {
      mut->limit = limit;
      goto done;
    }
    record_fragmentation(space, limit - hp);
    push_allocated_block(space, mut->block);
  }
  while (!(mut->block = pop_available_block(space))) {
    trigger_collection(mut);
  }
  pcc_space_compute_region(space, mut->block, &hp, &mut->limit);
  new_hp = hp + size;
  // The region is empty and is therefore large enough for a small
  // allocation.

done:
  mut->hp = new_hp;
  gc_clear_fresh_allocation(gc_ref(hp), size);
  return gc_ref_heap_object(gc_ref(hp));
}

void* gc_allocate_pointerless(struct gc_mutator *mut, size_t size) {
  return gc_allocate(mut, size);
}

struct gc_ephemeron* gc_allocate_ephemeron(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_ephemeron_size());
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
}

struct gc_pending_ephemerons *gc_heap_pending_ephemerons(struct gc_heap *heap) {
  return heap->pending_ephemerons;
}

unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return heap->count;
}

static struct pcc_slab* allocate_slabs(size_t nslabs) {
  size_t size = nslabs * SLAB_SIZE;
  size_t extent = size + SLAB_SIZE;

  char *mem = mmap(NULL, extent, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return NULL;
  }

  uintptr_t base = (uintptr_t) mem;
  uintptr_t end = base + extent;
  uintptr_t aligned_base = align_up(base, SLAB_SIZE);
  uintptr_t aligned_end = aligned_base + size;

  if (aligned_base - base)
    munmap((void*)base, aligned_base - base);
  if (end - aligned_end)
    munmap((void*)aligned_end, end - aligned_end);

  return (struct pcc_slab*) aligned_base;
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

  if (!heap_prepare_pending_ephemerons(heap))
    GC_CRASH();

  return 1;
}

static int pcc_space_init(struct pcc_space *space, struct gc_heap *heap) {
  size_t size = align_up(heap->size, SLAB_SIZE);
  size_t nslabs = size / SLAB_SIZE;
  struct pcc_slab *slabs = allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  space->available = NULL;
  space->partially_allocated = NULL;
  space->allocated = NULL;
  space->allocated_block_count = 0;
  space->paged_out = NULL;
  space->fragmentation = 0;
  space->bytes_to_page_out = 0;
  space->active_region = 0;
  space->live_bytes_at_last_gc = 0;
  space->fragmentation_at_last_gc = 0;
  space->extents = calloc(1, sizeof(struct pcc_extent));
  space->extents[0].low_addr = (uintptr_t) slabs;
  space->extents[0].high_addr = space->extents[0].low_addr + size;
  space->nextents = 1;
  space->slabs = slabs;
  space->nslabs = nslabs;
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < NONHEADER_BLOCKS_PER_SLAB; idx++) {
      struct pcc_block *block = &slabs[slab].headers[idx];
      if (size > heap->size) {
        block->in_core = 0;
        push_paged_out_block(space, block);
        size -= BLOCK_SIZE;
      } else {
        block->in_core = 1;
        push_available_block(space, block);
      }
    }
  }
  return 1;
}

int gc_init(const struct gc_options *options, struct gc_stack_addr *stack_base,
            struct gc_heap **heap, struct gc_mutator **mut,
            struct gc_event_listener event_listener,
            void *event_listener_data) {
  GC_ASSERT_EQ(gc_allocator_small_granule_size(), GC_ALIGNMENT);
  GC_ASSERT_EQ(gc_allocator_large_threshold(), GC_LARGE_OBJECT_THRESHOLD);
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct gc_mutator, hp));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct gc_mutator, limit));

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

  struct pcc_space *space = heap_pcc_space(*heap);
  if (!pcc_space_init(space, *heap)) {
    free(*heap);
    *heap = NULL;
    return 0;
  }
  
  if (!large_object_space_init(heap_large_object_space(*heap), *heap))
    GC_CRASH();

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  add_mutator(*heap, *mut);
  return 1;
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr *stack_base,
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
  heap_lock(heap);
  heap->inactive_mutator_count++;
  if (all_mutators_stopped(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void reactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
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
