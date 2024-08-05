#ifndef COPY_SPACE_H
#define COPY_SPACE_H

#include <stdlib.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "assert.h"
#include "debug.h"
#include "gc-align.h"
#include "gc-attrs.h"
#include "gc-inline.h"
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

struct copy_space_extent {
  uintptr_t low_addr;
  uintptr_t high_addr;
};

struct copy_space {
  struct copy_space_block *empty;
  struct copy_space_block *partly_full;
  struct copy_space_block *full ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t allocated_bytes;
  size_t fragmentation;
  struct copy_space_block *paged_out ALIGNED_TO_AVOID_FALSE_SHARING;
  ssize_t bytes_to_page_out ALIGNED_TO_AVOID_FALSE_SHARING;
  // The rest of these members are only changed rarely and with the heap
  // lock.
  uint8_t active_region ALIGNED_TO_AVOID_FALSE_SHARING;
  size_t allocated_bytes_at_last_gc;
  size_t fragmentation_at_last_gc;
  struct copy_space_extent *extents;
  size_t nextents;
  struct copy_space_slab *slabs;
  size_t nslabs;
};

struct copy_space_allocator {
  uintptr_t hp;
  uintptr_t limit;
  struct copy_space_block *block;
};

static void
copy_space_push_block(struct copy_space_block **list,
                      struct copy_space_block *block) {
  struct copy_space_block *next =
    atomic_load_explicit(list, memory_order_acquire);
  do {
    block->next = next;
  } while (!atomic_compare_exchange_weak(list, &next, block));
}

static struct copy_space_block*
copy_space_pop_block(struct copy_space_block **list) {
  struct copy_space_block *head =
    atomic_load_explicit(list, memory_order_acquire);
  struct copy_space_block *next;
  do {
    if (!head)
      return NULL;
  } while (!atomic_compare_exchange_weak(list, &head, head->next));
  head->next = NULL;
  return head;
}

static struct copy_space_block*
copy_space_pop_empty_block(struct copy_space *space) {
  struct copy_space_block *ret = copy_space_pop_block(&space->empty);
  if (ret)
    ret->allocated = 0;
  return ret;
}

static void
copy_space_push_empty_block(struct copy_space *space,
                            struct copy_space_block *block) {
  copy_space_push_block(&space->empty, block);
}

static struct copy_space_block*
copy_space_pop_full_block(struct copy_space *space) {
  return copy_space_pop_block(&space->full);
}

static void
copy_space_push_full_block(struct copy_space *space,
                           struct copy_space_block *block) {
  copy_space_push_block(&space->full, block);
}

static struct copy_space_block*
copy_space_pop_partly_full_block(struct copy_space *space) {
  return copy_space_pop_block(&space->partly_full);
}

static void
copy_space_push_partly_full_block(struct copy_space *space,
                                  struct copy_space_block *block) {
  copy_space_push_block(&space->partly_full, block);
}

static struct copy_space_block*
copy_space_pop_paged_out_block(struct copy_space *space) {
  return copy_space_pop_block(&space->paged_out);
}

static void
copy_space_push_paged_out_block(struct copy_space *space,
                                struct copy_space_block *block) {
  copy_space_push_block(&space->paged_out, block);
}

static void
copy_space_page_out_block(struct copy_space *space,
                          struct copy_space_block *block) {
  block->in_core = 0;
  block->all_zeroes[0] = block->all_zeroes[1] = 1;
  madvise(copy_space_block_payload(block), COPY_SPACE_BLOCK_SIZE, MADV_DONTNEED);
  copy_space_push_paged_out_block(space, block);
}

static struct copy_space_block*
copy_space_page_in_block(struct copy_space *space) {
  struct copy_space_block* block = copy_space_pop_paged_out_block(space);
  if (block) block->in_core = 1;
  return block;
}

static ssize_t
copy_space_request_release_memory(struct copy_space *space, size_t bytes) {
  return atomic_fetch_add(&space->bytes_to_page_out, bytes) + bytes;
}

static int
copy_space_page_out_blocks_until_memory_released(struct copy_space *space) {
  ssize_t pending = atomic_load(&space->bytes_to_page_out);
  while (pending > 0) {
    struct copy_space_block *block = copy_space_pop_empty_block(space);
    if (!block) return 0;
    copy_space_page_out_block(space, block);
    pending = (atomic_fetch_sub(&space->bytes_to_page_out, COPY_SPACE_BLOCK_SIZE)
               - COPY_SPACE_BLOCK_SIZE);
  }
  return 1;
}

static void
copy_space_reacquire_memory(struct copy_space *space, size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->bytes_to_page_out, bytes) - bytes;
  while (pending + COPY_SPACE_BLOCK_SIZE <= 0) {
    struct copy_space_block *block = copy_space_page_in_block(space);
    GC_ASSERT(block);
    copy_space_push_empty_block(space, block);
    pending = (atomic_fetch_add(&space->bytes_to_page_out, COPY_SPACE_BLOCK_SIZE)
               + COPY_SPACE_BLOCK_SIZE);
  }
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
  if (copy_space_allocator_acquire_block(alloc,
                                         copy_space_pop_empty_block(space),
                                         space->active_region)) {
    if (alloc->block->all_zeroes[space->active_region])
      alloc->block->all_zeroes[space->active_region] = 0;
    else
      memset((char*)alloc->hp, 0, COPY_SPACE_REGION_SIZE);
    return 1;
  }
  return 0;
}

static int
copy_space_allocator_acquire_partly_full_block(struct copy_space_allocator *alloc,
                                               struct copy_space *space) {
  if (copy_space_allocator_acquire_block(alloc,
                                         copy_space_pop_partly_full_block(space),
                                         space->active_region)) {
    alloc->hp += alloc->block->allocated;
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
    copy_space_push_partly_full_block(space, alloc->block);
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
  struct copy_space_block *flip = space->full;
  flip = copy_space_append_block_lists(space->partly_full, flip);
  flip = copy_space_append_block_lists(space->empty, flip);
  space->empty = flip;
  space->partly_full = NULL;
  space->full = NULL;
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
copy_space_forward(struct copy_space *space, struct gc_edge edge,
                   struct gc_ref old_ref, struct copy_space_allocator *alloc) {
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
copy_space_forward_if_traced(struct copy_space *space, struct gc_edge edge,
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
                             struct gc_ref old_ref, struct copy_space_allocator *alloc) {
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
copy_space_contains(struct copy_space *space, struct gc_ref ref) {
  for (size_t i = 0; i < space->nextents; i++)
    if (space->extents[i].low_addr <= gc_ref_value(ref) &&
        gc_ref_value(ref) < space->extents[i].high_addr)
      return 1;
  return 0;
}

static inline void
copy_space_allocator_init(struct copy_space_allocator *alloc,
                          struct copy_space *space) {
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

static int
copy_space_init(struct copy_space *space, size_t size) {
  size = align_up(size, COPY_SPACE_BLOCK_SIZE);
  size_t reserved = align_up(size, COPY_SPACE_SLAB_SIZE);
  size_t nslabs = reserved / COPY_SPACE_SLAB_SIZE;
  struct copy_space_slab *slabs = copy_space_allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  space->empty = NULL;
  space->partly_full = NULL;
  space->full = NULL;
  space->paged_out = NULL;
  space->allocated_bytes = 0;
  space->fragmentation = 0;
  space->bytes_to_page_out = 0;
  space->active_region = 0;
  space->allocated_bytes_at_last_gc = 0;
  space->fragmentation_at_last_gc = 0;
  space->extents = calloc(1, sizeof(struct copy_space_extent));
  space->extents[0].low_addr = (uintptr_t) slabs;
  space->extents[0].high_addr = space->extents[0].low_addr + reserved;
  space->nextents = 1;
  space->slabs = slabs;
  space->nslabs = nslabs;
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < COPY_SPACE_NONHEADER_BLOCKS_PER_SLAB; idx++) {
      struct copy_space_block *block = &slabs[slab].headers[idx];
      block->all_zeroes[0] = block->all_zeroes[1] = 1;
      if (reserved > size) {
        block->in_core = 0;
        copy_space_push_paged_out_block(space, block);
        reserved -= COPY_SPACE_BLOCK_SIZE;
      } else {
        block->in_core = 1;
        copy_space_push_empty_block(space, block);
      }
    }
  }
  return 1;
}

#endif // COPY_SPACE_H
