#ifndef COPY_SPACE_H
#define COPY_SPACE_H

#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "assert.h"
#include "background-thread.h"
#include "debug.h"
#include "extents.h"
#include "gc-align.h"
#include "gc-attrs.h"
#include "gc-inline.h"
#include "gc-lock.h"
#include "spin.h"

// A copy space: a block-structured space that traces via evacuation.

#define COPY_SPACE_SLAB_SIZE (64 * 1024 * 1024)
#define COPY_SPACE_REGION_SIZE (64 * 1024)
#define COPY_SPACE_BLOCK_SIZE (2 * COPY_SPACE_REGION_SIZE)
#define COPY_SPACE_BLOCKS_PER_SLAB \
  (COPY_SPACE_SLAB_SIZE / COPY_SPACE_BLOCK_SIZE)
#define COPY_SPACE_HEADER_BYTES_PER_BLOCK \
  (COPY_SPACE_BLOCK_SIZE / COPY_SPACE_BLOCKS_PER_SLAB)
#define COPY_SPACE_HEADER_BLOCKS_PER_SLAB 1
#define COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB \
  (COPY_SPACE_BLOCKS_PER_SLAB - COPY_SPACE_HEADER_BLOCKS_PER_SLAB)
#define COPY_SPACE_HEADER_BYTES_PER_SLAB \
  (COPY_SPACE_HEADER_BYTES_PER_BLOCK * COPY_SPACE_HEADER_BLOCKS_PER_SLAB)

struct copy_space_slab;

struct copy_space_slab_header {
  union {
    struct {
      struct copy_space_slab *next;
      struct copy_space_slab *prev;
      unsigned incore_block_count;
    };
    uint8_t padding[COPY_SPACE_HEADER_BYTES_PER_SLAB];
  };
};
STATIC_ASSERT_EQ(sizeof(struct copy_space_slab_header),
                 COPY_SPACE_HEADER_BYTES_PER_SLAB);

// Really just the block header.
struct copy_space_block {
  union {
    struct {
      struct copy_space_block *next;
      uint8_t in_core;
      uint8_t all_zeroes[2];
      size_t allocated; // For partly-empty blocks.
    };
    uint8_t padding[COPY_SPACE_HEADER_BYTES_PER_BLOCK];
  };
};
STATIC_ASSERT_EQ(sizeof(struct copy_space_block),
                 COPY_SPACE_HEADER_BYTES_PER_BLOCK);

struct copy_space_region {
  char data[COPY_SPACE_REGION_SIZE];
};

struct copy_space_block_payload {
  struct copy_space_region regions[2];
};

struct copy_space_slab {
  struct copy_space_slab_header header;
  struct copy_space_block headers[COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB];
  struct copy_space_block_payload blocks[COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct copy_space_slab), COPY_SPACE_SLAB_SIZE);

static inline struct copy_space_block*
copy_space_block_header(struct copy_space_block_payload *payload) {
  uintptr_t addr = (uintptr_t) payload;
  uintptr_t base = align_down(addr, COPY_SPACE_SLAB_SIZE);
  struct copy_space_slab *slab = (struct copy_space_slab*) base;
  uintptr_t block_idx =
    (addr / COPY_SPACE_BLOCK_SIZE) % COPY_SPACE_BLOCKS_PER_SLAB;
  return &slab->headers[block_idx - COPY_SPACE_HEADER_BLOCKS_PER_SLAB];
}

static inline struct copy_space_block_payload*
copy_space_block_payload(struct copy_space_block *block) {
  uintptr_t addr = (uintptr_t) block;
  uintptr_t base = align_down(addr, COPY_SPACE_SLAB_SIZE);
  struct copy_space_slab *slab = (struct copy_space_slab*) base;
  uintptr_t block_idx =
    (addr / COPY_SPACE_HEADER_BYTES_PER_BLOCK) % COPY_SPACE_BLOCKS_PER_SLAB;
  return &slab->blocks[block_idx - COPY_SPACE_HEADER_BLOCKS_PER_SLAB];
}

static uint8_t
copy_space_object_region(struct gc_ref obj) {
  return (gc_ref_value(obj) / COPY_SPACE_REGION_SIZE) & 1;
}

#define COPY_SPACE_PAGE_OUT_QUEUE_SIZE 4

struct copy_space_block_list {
  struct copy_space_block *head;
};

struct copy_space_block_stack {
  struct copy_space_block_list list;
};

struct copy_space {
  pthread_mutex_t lock;
  struct copy_space_block_stack empty;
  struct copy_space_block_stack partly_full;
  struct copy_space_block_list full ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t allocated_bytes;
  size_t fragmentation;
  struct copy_space_block_stack paged_out[COPY_SPACE_PAGE_OUT_QUEUE_SIZE]
    ALIGNED_TO_AVOID_FALSE_SHARING;
  ssize_t bytes_to_page_out ALIGNED_TO_AVOID_FALSE_SHARING;
  // The rest of these members are only changed rarely and with the heap
  // lock.
  uint8_t active_region ALIGNED_TO_AVOID_FALSE_SHARING;
  uint8_t atomic_forward;
  size_t allocated_bytes_at_last_gc;
  size_t fragmentation_at_last_gc;
  struct extents *extents;
  struct copy_space_slab **slabs;
  size_t nslabs;
};

struct copy_space_allocator {
  uintptr_t hp;
  uintptr_t limit;
  struct copy_space_block *block;
};

static struct gc_lock
copy_space_lock(struct copy_space *space) {
  return gc_lock_acquire(&space->lock);
}

static void
copy_space_block_list_push(struct copy_space_block_list *list,
                           struct copy_space_block *block) {
  struct copy_space_block *next =
    atomic_load_explicit(&list->head, memory_order_acquire);
  do {
    block->next = next;
  } while (!atomic_compare_exchange_weak(&list->head, &next, block));
}

static struct copy_space_block*
copy_space_block_list_pop(struct copy_space_block_list *list) {
  struct copy_space_block *head =
    atomic_load_explicit(&list->head, memory_order_acquire);
  struct copy_space_block *next;
  do {
    if (!head)
      return NULL;
  } while (!atomic_compare_exchange_weak(&list->head, &head, head->next));
  head->next = NULL;
  return head;
}

static void
copy_space_block_stack_push(struct copy_space_block_stack *stack,
                            struct copy_space_block *block,
                            const struct gc_lock *lock) {
  struct copy_space_block *next = stack->list.head;
  block->next = next;
  stack->list.head = block;
}

static struct copy_space_block*
copy_space_block_stack_pop(struct copy_space_block_stack *stack,
                           const struct gc_lock *lock) {
  struct copy_space_block *head = stack->list.head;
  if (head) {
    stack->list.head = head->next;
    head->next = NULL;
  }
  return head;
}

static struct copy_space_block*
copy_space_pop_empty_block(struct copy_space *space,
                           const struct gc_lock *lock) {
  struct copy_space_block *ret = copy_space_block_stack_pop(&space->empty,
                                                            lock);
  if (ret)
    ret->allocated = 0;
  return ret;
}

static void
copy_space_push_empty_block(struct copy_space *space,
                            struct copy_space_block *block,
                            const struct gc_lock *lock) {
  copy_space_block_stack_push(&space->empty, block, lock);
}

static struct copy_space_block*
copy_space_pop_full_block(struct copy_space *space) {
  return copy_space_block_list_pop(&space->full);
}

static void
copy_space_push_full_block(struct copy_space *space,
                           struct copy_space_block *block) {
  copy_space_block_list_push(&space->full, block);
}

static struct copy_space_block*
copy_space_pop_partly_full_block(struct copy_space *space,
                                 const struct gc_lock *lock) {
  return copy_space_block_stack_pop(&space->partly_full, lock);
}

static void
copy_space_push_partly_full_block(struct copy_space *space,
                                  struct copy_space_block *block,
                                  const struct gc_lock *lock) {
  copy_space_block_stack_push(&space->partly_full, block, lock);
}

static void
copy_space_page_out_block(struct copy_space *space,
                          struct copy_space_block *block,
                          const struct gc_lock *lock) {
  copy_space_block_stack_push
    (block->in_core
     ? &space->paged_out[0]
     : &space->paged_out[COPY_SPACE_PAGE_OUT_QUEUE_SIZE-1],
     block,
     lock);
}

static struct copy_space_block*
copy_space_page_in_block(struct copy_space *space,
                         const struct gc_lock *lock) {
  for (int age = 0; age < COPY_SPACE_PAGE_OUT_QUEUE_SIZE; age++) {
    struct copy_space_block *block =
      copy_space_block_stack_pop(&space->paged_out[age], lock);
    if (block) return block;
  }
  return NULL;
}

static ssize_t
copy_space_request_release_memory(struct copy_space *space, size_t bytes) {
  return atomic_fetch_add(&space->bytes_to_page_out, bytes) + bytes;
}

static int
copy_space_page_out_blocks_until_memory_released(struct copy_space *space) {
  ssize_t pending = atomic_load(&space->bytes_to_page_out);
  struct gc_lock lock = copy_space_lock(space);
  while (pending > 0) {
    struct copy_space_block *block = copy_space_pop_empty_block(space, &lock);
    if (!block) break;
    copy_space_page_out_block(space, block, &lock);
    pending = (atomic_fetch_sub(&space->bytes_to_page_out, COPY_SPACE_BLOCK_SIZE)
               - COPY_SPACE_BLOCK_SIZE);
  }
  gc_lock_release(&lock);
  return pending <= 0;
}

static ssize_t
copy_space_maybe_reacquire_memory(struct copy_space *space, size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->bytes_to_page_out, bytes) - bytes;
  struct gc_lock lock = copy_space_lock(space);
  while (pending + COPY_SPACE_BLOCK_SIZE <= 0) {
    struct copy_space_block *block = copy_space_page_in_block(space, &lock);
    if (!block) break;
    copy_space_push_empty_block(space, block, &lock);
    pending = (atomic_fetch_add(&space->bytes_to_page_out,
                                COPY_SPACE_BLOCK_SIZE)
               + COPY_SPACE_BLOCK_SIZE);
  }
  gc_lock_release(&lock);
  return pending;
}

static void
copy_space_reacquire_memory(struct copy_space *space, size_t bytes) {
  ssize_t pending = copy_space_maybe_reacquire_memory(space, bytes);
  GC_ASSERT(pending + COPY_SPACE_BLOCK_SIZE > 0);
}

static inline void
copy_space_allocator_set_block(struct copy_space_allocator *alloc,
                               struct copy_space_block *block,
                               int active_region) {
  struct copy_space_block_payload *payload = copy_space_block_payload(block);
  struct copy_space_region *region = &payload->regions[active_region];
  alloc->block = block;
  alloc->hp = (uintptr_t)&region[0];
  alloc->limit = (uintptr_t)&region[1];
}

static inline int
copy_space_allocator_acquire_block(struct copy_space_allocator *alloc,
                                   struct copy_space_block *block,
                                   int active_region) {
  if (block) {
    copy_space_allocator_set_block(alloc, block, active_region);
    return 1;
  }
  return 0;
}

static int
copy_space_allocator_acquire_empty_block(struct copy_space_allocator *alloc,
                                         struct copy_space *space) {
  struct gc_lock lock = copy_space_lock(space);
  struct copy_space_block *block = copy_space_pop_empty_block(space, &lock);
  gc_lock_release(&lock);
  if (copy_space_allocator_acquire_block(alloc, block, space->active_region)) {
    block->in_core = 1;
    if (block->all_zeroes[space->active_region])
      block->all_zeroes[space->active_region] = 0;
    else
      memset((char*)alloc->hp, 0, COPY_SPACE_REGION_SIZE);
    return 1;
  }
  return 0;
}

static int
copy_space_allocator_acquire_partly_full_block(struct copy_space_allocator *alloc,
                                               struct copy_space *space) {
  struct gc_lock lock = copy_space_lock(space);
  struct copy_space_block *block = copy_space_pop_partly_full_block(space,
                                                                    &lock);
  gc_lock_release(&lock);
  if (copy_space_allocator_acquire_block(alloc, block, space->active_region)) {
    alloc->hp += block->allocated;
    return 1;
  }
  return 0;
}

static void
copy_space_allocator_release_full_block(struct copy_space_allocator *alloc,
                                        struct copy_space *space) {
  size_t fragmentation = alloc->limit - alloc->hp;
  size_t allocated = COPY_SPACE_REGION_SIZE - alloc->block->allocated;
  atomic_fetch_add_explicit(&space->allocated_bytes, allocated,
                            memory_order_relaxed);
  if (fragmentation)
    atomic_fetch_add_explicit(&space->fragmentation, fragmentation,
                              memory_order_relaxed);
  copy_space_push_full_block(space, alloc->block);
  alloc->hp = alloc->limit = 0;
  alloc->block = NULL;
}

static void
copy_space_allocator_release_partly_full_block(struct copy_space_allocator *alloc,
                                               struct copy_space *space) {
  size_t allocated = alloc->hp & (COPY_SPACE_REGION_SIZE - 1);
  if (allocated) {
    atomic_fetch_add_explicit(&space->allocated_bytes,
                              allocated - alloc->block->allocated,
                              memory_order_relaxed);
    alloc->block->allocated = allocated;
    struct gc_lock lock = copy_space_lock(space);
    copy_space_push_partly_full_block(space, alloc->block, &lock);
    gc_lock_release(&lock);
  } else {
    // In this case, hp was bumped all the way to the limit, in which
    // case allocated wraps to 0; the block is full.
    atomic_fetch_add_explicit(&space->allocated_bytes,
                              COPY_SPACE_REGION_SIZE - alloc->block->allocated,
                              memory_order_relaxed);
    copy_space_push_full_block(space, alloc->block);
  }
  alloc->hp = alloc->limit = 0;
  alloc->block = NULL;
}

static inline struct gc_ref
copy_space_allocate(struct copy_space_allocator *alloc,
                    struct copy_space *space,
                    size_t size,
                    void (*get_more_empty_blocks)(void *data),
                    void *data) {
  GC_ASSERT(size > 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());
  size = align_up(size, gc_allocator_small_granule_size());

  if (alloc->hp + size <= alloc->limit)
    goto done;

  if (alloc->block)
    copy_space_allocator_release_full_block(alloc, space);
  while (copy_space_allocator_acquire_partly_full_block(alloc, space)) {
    if (alloc->hp + size <= alloc->limit)
      goto done;
    copy_space_allocator_release_full_block(alloc, space);
  }
  while (!copy_space_allocator_acquire_empty_block(alloc, space))
    get_more_empty_blocks(data);
  // The newly acquired block is empty and is therefore large enough for
  // a small allocation.

done:
  struct gc_ref ret = gc_ref(alloc->hp);
  alloc->hp += size;
  return ret;
}

static struct copy_space_block*
copy_space_append_block_lists(struct copy_space_block *head,
                              struct copy_space_block *tail) {
  if (!head) return tail;
  if (tail) {
    struct copy_space_block *walk = head;
    while (walk->next)
      walk = walk->next;
    walk->next = tail;
  }
  return head;
}

static void
copy_space_flip(struct copy_space *space) {
  // Mutators stopped, can access nonatomically.
  struct copy_space_block* flip = space->full.head;
  flip = copy_space_append_block_lists(space->partly_full.list.head, flip);
  flip = copy_space_append_block_lists(space->empty.list.head, flip);
  space->empty.list.head = flip;
  space->partly_full.list.head = NULL;
  space->full.head = NULL;
  space->allocated_bytes = 0;
  space->fragmentation = 0;
  space->active_region ^= 1;
}

static void
copy_space_finish_gc(struct copy_space *space) {
  // Mutators stopped, can access nonatomically.
  space->allocated_bytes_at_last_gc = space->allocated_bytes;
  space->fragmentation_at_last_gc = space->fragmentation;
}

static void
copy_space_add_to_allocation_counter(struct copy_space *space,
                                     uintptr_t *counter) {
  *counter += space->allocated_bytes - space->allocated_bytes_at_last_gc;
}

static void
copy_space_gc_during_evacuation(void *data) {
  // If space is really tight and reordering of objects during
  // evacuation resulted in more end-of-block fragmentation and thus
  // block use than before collection started, we can actually run out
  // of memory while collecting.  We should probably attempt to expand
  // the heap here, at least by a single block; it's better than the
  // alternatives.
  fprintf(stderr, "Out of memory\n");
  GC_CRASH();
}

static inline int
copy_space_forward_atomic(struct copy_space *space, struct gc_edge edge,
                          struct gc_ref old_ref,
                          struct copy_space_allocator *alloc) {
  GC_ASSERT(copy_space_object_region(old_ref) != space->active_region);
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
    struct gc_ref new_ref =
      copy_space_allocate(alloc, space, bytes,
                          copy_space_gc_during_evacuation, NULL);
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

static int
copy_space_forward_if_traced_atomic(struct copy_space *space,
                                    struct gc_edge edge,
                                    struct gc_ref old_ref) {
  GC_ASSERT(copy_space_object_region(old_ref) != space->active_region);
  struct gc_atomic_forward fwd = gc_atomic_forward_begin(old_ref);
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
}

static inline int
copy_space_forward_nonatomic(struct copy_space *space, struct gc_edge edge,
                             struct gc_ref old_ref,
                             struct copy_space_allocator *alloc) {
  GC_ASSERT(copy_space_object_region(old_ref) != space->active_region);

  uintptr_t forwarded = gc_object_forwarded_nonatomic(old_ref);
  if (forwarded) {
    gc_edge_update(edge, gc_ref(forwarded));
    return 0;
  } else {
    size_t size;
    gc_trace_object(old_ref, NULL, NULL, NULL, &size);
    struct gc_ref new_ref =
      copy_space_allocate(alloc, space, size,
                          copy_space_gc_during_evacuation, NULL);
    memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(old_ref), size);
    gc_object_forward_nonatomic(old_ref, new_ref);
    gc_edge_update(edge, new_ref);
    return 1;
  }
}

static int
copy_space_forward_if_traced_nonatomic(struct copy_space *space,
                                       struct gc_edge edge,
                                       struct gc_ref old_ref) {
  GC_ASSERT(copy_space_object_region(old_ref) != space->active_region);
  uintptr_t forwarded = gc_object_forwarded_nonatomic(old_ref);
  if (forwarded) {
    gc_edge_update(edge, gc_ref(forwarded));
    return 1;
  }
  return 0;
}

static inline int
copy_space_forward(struct copy_space *space, struct gc_edge edge,
                   struct gc_ref old_ref,
                   struct copy_space_allocator *alloc) {
  if (GC_PARALLEL && space->atomic_forward)
    return copy_space_forward_atomic(space, edge, old_ref, alloc);
  return copy_space_forward_nonatomic(space, edge, old_ref, alloc);
}

static inline int
copy_space_forward_if_traced(struct copy_space *space, struct gc_edge edge,
                             struct gc_ref old_ref) {
  if (GC_PARALLEL && space->atomic_forward)
    return copy_space_forward_if_traced_atomic(space, edge, old_ref);
  return copy_space_forward_if_traced_nonatomic(space, edge, old_ref);
}

static inline int
copy_space_contains(struct copy_space *space, struct gc_ref ref) {
  return extents_contain_addr(space->extents, gc_ref_value(ref));
}

static inline void
copy_space_allocator_init(struct copy_space_allocator *alloc) {
  memset(alloc, 0, sizeof(*alloc));
}

static inline void
copy_space_allocator_finish(struct copy_space_allocator *alloc,
                            struct copy_space *space) {
  if (alloc->block)
    copy_space_allocator_release_partly_full_block(alloc, space);
}

static struct copy_space_slab*
copy_space_allocate_slabs(size_t nslabs) {
  size_t size = nslabs * COPY_SPACE_SLAB_SIZE;
  size_t extent = size + COPY_SPACE_SLAB_SIZE;

  char *mem = mmap(NULL, extent, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return NULL;
  }

  uintptr_t base = (uintptr_t) mem;
  uintptr_t end = base + extent;
  uintptr_t aligned_base = align_up(base, COPY_SPACE_SLAB_SIZE);
  uintptr_t aligned_end = aligned_base + size;

  if (aligned_base - base)
    munmap((void*)base, aligned_base - base);
  if (end - aligned_end)
    munmap((void*)aligned_end, end - aligned_end);

  return (struct copy_space_slab*) aligned_base;
}

static void
copy_space_add_slabs(struct copy_space *space, struct copy_space_slab *slabs,
                     size_t nslabs) {
  size_t old_size = space->nslabs * sizeof(struct copy_space_slab*);
  size_t additional_size = nslabs * sizeof(struct copy_space_slab*);
  space->extents = extents_adjoin(space->extents, slabs,
                                  nslabs * sizeof(struct copy_space_slab));
  space->slabs = realloc(space->slabs, old_size + additional_size);
  if (!space->slabs)
    GC_CRASH();
  while (nslabs--)
    space->slabs[space->nslabs++] = slabs++;
}

static void
copy_space_shrink(struct copy_space *space, size_t bytes) {
  ssize_t pending = copy_space_request_release_memory(space, bytes);
  copy_space_page_out_blocks_until_memory_released(space);
  
  // It still may be the case we need to page out more blocks.  Only collection
  // can help us then!
}
      
static void
copy_space_expand(struct copy_space *space, size_t bytes) {
  ssize_t to_acquire = -copy_space_maybe_reacquire_memory(space, bytes);
  if (to_acquire <= 0) return;
  size_t reserved = align_up(to_acquire, COPY_SPACE_SLAB_SIZE);
  size_t nslabs = reserved / COPY_SPACE_SLAB_SIZE;
  struct copy_space_slab *slabs = copy_space_allocate_slabs(nslabs);
  copy_space_add_slabs(space, slabs, nslabs);

  struct gc_lock lock = copy_space_lock(space);
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB; idx++) {
      struct copy_space_block *block = &slabs[slab].headers[idx];
      block->all_zeroes[0] = block->all_zeroes[1] = 1;
      block->in_core = 0;
      copy_space_page_out_block(space, block, &lock);
      reserved -= COPY_SPACE_BLOCK_SIZE;
    }
  }
  gc_lock_release(&lock);
  copy_space_reacquire_memory(space, 0);
}

static void
copy_space_advance_page_out_queue(void *data) {
  struct copy_space *space = data;
  struct gc_lock lock = copy_space_lock(space);
  for (int age = COPY_SPACE_PAGE_OUT_QUEUE_SIZE - 3; age >= 0; age--) {
    while (1) {
      struct copy_space_block *block =
        copy_space_block_stack_pop(&space->paged_out[age], &lock);
      if (!block) break;
      copy_space_block_stack_push(&space->paged_out[age + 1], block, &lock);
    }
  }
  gc_lock_release(&lock);
}

static void
copy_space_page_out_blocks(void *data) {
  struct copy_space *space = data;
  int age = COPY_SPACE_PAGE_OUT_QUEUE_SIZE - 2;
  struct gc_lock lock = copy_space_lock(space);
  while (1) {
    struct copy_space_block *block =
      copy_space_block_stack_pop(&space->paged_out[age], &lock);
    if (!block) break;
    block->in_core = 0;
    block->all_zeroes[0] = block->all_zeroes[1] = 1;
    madvise(copy_space_block_payload(block), COPY_SPACE_BLOCK_SIZE,
            MADV_DONTNEED);
    copy_space_block_stack_push(&space->paged_out[age + 1], block, &lock);
  }
  gc_lock_release(&lock);
}

static int
copy_space_init(struct copy_space *space, size_t size, int atomic,
                struct gc_background_thread *thread) {
  size = align_up(size, COPY_SPACE_BLOCK_SIZE);
  size_t reserved = align_up(size, COPY_SPACE_SLAB_SIZE);
  size_t nslabs = reserved / COPY_SPACE_SLAB_SIZE;
  struct copy_space_slab *slabs = copy_space_allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  pthread_mutex_init(&space->lock, NULL);
  space->empty.list.head = NULL;
  space->partly_full.list.head = NULL;
  space->full.head = NULL;
  for (int age = 0; age < COPY_SPACE_PAGE_OUT_QUEUE_SIZE; age++)
    space->paged_out[age].list.head = NULL;
  space->allocated_bytes = 0;
  space->fragmentation = 0;
  space->bytes_to_page_out = 0;
  space->active_region = 0;
  space->atomic_forward = atomic;
  space->allocated_bytes_at_last_gc = 0;
  space->fragmentation_at_last_gc = 0;
  space->extents = extents_allocate(10);
  copy_space_add_slabs(space, slabs, nslabs);
  struct gc_lock lock = copy_space_lock(space);
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB; idx++) {
      struct copy_space_block *block = &slabs[slab].headers[idx];
      block->all_zeroes[0] = block->all_zeroes[1] = 1;
      block->in_core = 0;
      if (reserved > size) {
        copy_space_page_out_block(space, block, &lock);
        reserved -= COPY_SPACE_BLOCK_SIZE;
      } else {
        copy_space_push_empty_block(space, block, &lock);
      }
    }
  }
  gc_lock_release(&lock);
  gc_background_thread_add_task(thread, GC_BACKGROUND_TASK_START,
                                copy_space_advance_page_out_queue,
                                space);
  gc_background_thread_add_task(thread, GC_BACKGROUND_TASK_END,
                                copy_space_page_out_blocks,
                                space);
  return 1;
}

#endif // COPY_SPACE_H
