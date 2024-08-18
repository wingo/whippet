#ifndef NOFL_SPACE_H
#define NOFL_SPACE_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "assert.h"
#include "debug.h"
#include "gc-align.h"
#include "gc-attrs.h"
#include "gc-inline.h"
#include "spin.h"
#include "swar.h"

// This is the nofl space!  It is a mark space which doesn't use
// free-lists to allocate, and which can evacuate objects if
// fragmentation is too high, inspired by Immix.  Nofl stands for "no
// free-list", but also "novel", in the sense that it hasn't been tried
// before.

#define NOFL_GRANULE_SIZE 16
#define NOFL_GRANULE_SIZE_LOG_2 4
#define NOFL_MEDIUM_OBJECT_THRESHOLD 256
#define NOFL_MEDIUM_OBJECT_GRANULE_THRESHOLD 16

STATIC_ASSERT_EQ(NOFL_GRANULE_SIZE, 1 << NOFL_GRANULE_SIZE_LOG_2);
STATIC_ASSERT_EQ(NOFL_MEDIUM_OBJECT_THRESHOLD,
                 NOFL_MEDIUM_OBJECT_GRANULE_THRESHOLD * NOFL_GRANULE_SIZE);

// Each granule has one mark byte stored in a side table.  A granule's
// mark state is a whole byte instead of a bit to facilitate parallel
// marking.  (Parallel markers are allowed to race.)  We also use this
// byte to compute object extent, via a bit flag indicating
// end-of-object.
//
// Because we want to allow for conservative roots, we need to know
// whether an address indicates an object or not.  That means that when
// an object is allocated, it has to set a bit, somewhere.  We use the
// metadata byte for this purpose, setting the "young" bit.
//
// The "young" bit's name might make you think about generational
// collection, and indeed all objects collected in a minor collection
// will have this bit set.  However, the nofl space never needs to check
// for the young bit; if it weren't for the need to identify
// conservative roots, we wouldn't need a young bit at all.  Perhaps in
// an all-precise system, we would be able to avoid the overhead of
// initializing mark byte upon each fresh allocation.
//
// When an object becomes dead after a GC, it will still have a bit set
// -- maybe the young bit, or maybe a survivor bit.  The sweeper has to
// clear these bits before the next collection.  But, for concurrent
// marking, we will also be marking "live" objects, updating their mark
// bits.  So there are four object states concurrently observable:
// young, dead, survivor, and marked.  (If we didn't have concurrent
// marking we would still need the "marked" state, because marking
// mutator roots before stopping is also a form of concurrent marking.)
// Even though these states are mutually exclusive, we use separate bits
// for them because we have the space.  After each collection, the dead,
// survivor, and marked states rotate by one bit.
enum nofl_metadata_byte {
  NOFL_METADATA_BYTE_NONE = 0,
  NOFL_METADATA_BYTE_YOUNG = 1,
  NOFL_METADATA_BYTE_MARK_0 = 2,
  NOFL_METADATA_BYTE_MARK_1 = 4,
  NOFL_METADATA_BYTE_MARK_2 = 8,
  NOFL_METADATA_BYTE_END = 16,
  NOFL_METADATA_BYTE_EPHEMERON = 32,
  NOFL_METADATA_BYTE_PINNED = 64,
  NOFL_METADATA_BYTE_UNUSED_1 = 128
};

static uint8_t
nofl_rotate_dead_survivor_marked(uint8_t mask) {
  uint8_t all =
    NOFL_METADATA_BYTE_MARK_0 | NOFL_METADATA_BYTE_MARK_1 | NOFL_METADATA_BYTE_MARK_2;
  return ((mask << 1) | (mask >> 2)) & all;
}

#define NOFL_SLAB_SIZE (4 * 1024 * 1024)
#define NOFL_BLOCK_SIZE (64 * 1024)
#define NOFL_METADATA_BYTES_PER_BLOCK (NOFL_BLOCK_SIZE / NOFL_GRANULE_SIZE)
#define NOFL_BLOCKS_PER_SLAB (NOFL_SLAB_SIZE / NOFL_BLOCK_SIZE)
#define NOFL_META_BLOCKS_PER_SLAB (NOFL_METADATA_BYTES_PER_BLOCK * NOFL_BLOCKS_PER_SLAB / NOFL_BLOCK_SIZE)
#define NOFL_NONMETA_BLOCKS_PER_SLAB (NOFL_BLOCKS_PER_SLAB - NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_METADATA_BYTES_PER_SLAB (NOFL_NONMETA_BLOCKS_PER_SLAB * NOFL_METADATA_BYTES_PER_BLOCK)
#define NOFL_SLACK_METADATA_BYTES_PER_SLAB (NOFL_META_BLOCKS_PER_SLAB * NOFL_METADATA_BYTES_PER_BLOCK)
#define NOFL_REMSET_BYTES_PER_BLOCK (NOFL_SLACK_METADATA_BYTES_PER_SLAB / NOFL_BLOCKS_PER_SLAB)
#define NOFL_REMSET_BYTES_PER_SLAB (NOFL_REMSET_BYTES_PER_BLOCK * NOFL_NONMETA_BLOCKS_PER_SLAB)
#define NOFL_SLACK_REMSET_BYTES_PER_SLAB (NOFL_REMSET_BYTES_PER_BLOCK * NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_SUMMARY_BYTES_PER_BLOCK (NOFL_SLACK_REMSET_BYTES_PER_SLAB / NOFL_BLOCKS_PER_SLAB)
#define NOFL_SUMMARY_BYTES_PER_SLAB (NOFL_SUMMARY_BYTES_PER_BLOCK * NONMETA_BLOCKS_PER_SLAB)
#define NOFL_SLACK_SUMMARY_BYTES_PER_SLAB (NOFL_SUMMARY_BYTES_PER_BLOCK * NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_HEADER_BYTES_PER_SLAB NOFL_SLACK_SUMMARY_BYTES_PER_SLAB

struct nofl_slab;

struct nofl_slab_header {
  union {
    struct {
      struct nofl_slab *next;
      struct nofl_slab *prev;
    };
    uint8_t padding[NOFL_HEADER_BYTES_PER_SLAB];
  };
};
STATIC_ASSERT_EQ(sizeof(struct nofl_slab_header), NOFL_HEADER_BYTES_PER_SLAB);

// Sometimes we want to put a block on a singly-linked list.  For that
// there's a pointer reserved in the block summary.  But because the
// pointer is aligned (32kB on 32-bit, 64kB on 64-bit), we can portably
// hide up to 15 flags in the low bits.  These flags can be accessed
// non-atomically by the mutator when it owns a block; otherwise they
// need to be accessed atomically.
enum nofl_block_summary_flag {
  NOFL_BLOCK_OUT_FOR_THREAD = 0x1,
  NOFL_BLOCK_HAS_PIN = 0x2,
  NOFL_BLOCK_PAGED_OUT = 0x4,
  NOFL_BLOCK_NEEDS_SWEEP = 0x8,
  NOFL_BLOCK_UNAVAILABLE = 0x10,
  NOFL_BLOCK_EVACUATE = 0x20,
  NOFL_BLOCK_VENERABLE = 0x40,
  NOFL_BLOCK_VENERABLE_AFTER_SWEEP = 0x80,
  NOFL_BLOCK_FLAG_UNUSED_8 = 0x100,
  NOFL_BLOCK_FLAG_UNUSED_9 = 0x200,
  NOFL_BLOCK_FLAG_UNUSED_10 = 0x400,
  NOFL_BLOCK_FLAG_UNUSED_11 = 0x800,
  NOFL_BLOCK_FLAG_UNUSED_12 = 0x1000,
  NOFL_BLOCK_FLAG_UNUSED_13 = 0x2000,
  NOFL_BLOCK_FLAG_UNUSED_14 = 0x4000,
};

struct nofl_block_summary {
  union {
    struct {
      // Counters related to previous collection: how many holes there
      // were, and how much space they had.
      uint16_t hole_count;
      uint16_t free_granules;
      // Counters related to allocation since previous collection:
      // wasted space due to fragmentation.
      uint16_t holes_with_fragmentation;
      uint16_t fragmentation_granules;
      // After a block is swept, if it's empty it goes on the empties
      // list.  Otherwise if it's not immediately used by a mutator (as
      // is usually the case), it goes on the swept list.  Both of these
      // lists use this field.  But as the next element in the field is
      // block-aligned, we stash flags in the low bits.
      uintptr_t next_and_flags;
    };
    uint8_t padding[NOFL_SUMMARY_BYTES_PER_BLOCK];
  };
};
STATIC_ASSERT_EQ(sizeof(struct nofl_block_summary),
                 NOFL_SUMMARY_BYTES_PER_BLOCK);

struct nofl_block {
  char data[NOFL_BLOCK_SIZE];
};

struct nofl_slab {
  struct nofl_slab_header header;
  struct nofl_block_summary summaries[NOFL_NONMETA_BLOCKS_PER_SLAB];
  uint8_t remembered_set[NOFL_REMSET_BYTES_PER_SLAB];
  uint8_t metadata[NOFL_METADATA_BYTES_PER_SLAB];
  struct nofl_block blocks[NOFL_NONMETA_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct nofl_slab), NOFL_SLAB_SIZE);

static struct nofl_slab*
nofl_object_slab(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = align_down(addr, NOFL_SLAB_SIZE);
  return (struct nofl_slab*) base;
}

static uint8_t*
nofl_metadata_byte_for_addr(uintptr_t addr) {
  uintptr_t base = align_down(addr, NOFL_SLAB_SIZE);
  uintptr_t granule = (addr & (NOFL_SLAB_SIZE - 1)) >> NOFL_GRANULE_SIZE_LOG_2;
  return (uint8_t*) (base + granule);
}

static uint8_t*
nofl_metadata_byte_for_object(struct gc_ref ref) {
  return nofl_metadata_byte_for_addr(gc_ref_value(ref));
}

#define NOFL_GRANULES_PER_BLOCK (NOFL_BLOCK_SIZE / NOFL_GRANULE_SIZE)
#define NOFL_GRANULES_PER_REMSET_BYTE \
  (NOFL_GRANULES_PER_BLOCK / NOFL_REMSET_BYTES_PER_BLOCK)

static struct nofl_block_summary*
nofl_block_summary_for_addr(uintptr_t addr) {
  uintptr_t base = align_down(addr, NOFL_SLAB_SIZE);
  uintptr_t block = (addr & (NOFL_SLAB_SIZE - 1)) / NOFL_BLOCK_SIZE;
  return (struct nofl_block_summary*)
    (base + block * sizeof(struct nofl_block_summary));
}

static uintptr_t
nofl_block_summary_has_flag(struct nofl_block_summary *summary,
                            enum nofl_block_summary_flag flag) {
  return summary->next_and_flags & flag;
}

static void
nofl_block_summary_set_flag(struct nofl_block_summary *summary,
                                        enum nofl_block_summary_flag flag) {
  summary->next_and_flags |= flag;
}

static void
nofl_block_summary_clear_flag(struct nofl_block_summary *summary,
                              enum nofl_block_summary_flag flag) {
  summary->next_and_flags &= ~(uintptr_t)flag;
}

static uintptr_t
nofl_block_summary_next(struct nofl_block_summary *summary) {
  return align_down(summary->next_and_flags, NOFL_BLOCK_SIZE);
}

static void
nofl_block_summary_set_next(struct nofl_block_summary *summary,
                            uintptr_t next) {
  GC_ASSERT((next & (NOFL_BLOCK_SIZE - 1)) == 0);
  summary->next_and_flags =
    (summary->next_and_flags & (NOFL_BLOCK_SIZE - 1)) | next;
}

// Lock-free block list.
struct nofl_block_list {
  size_t count;
  uintptr_t blocks;
};

static void
nofl_push_block(struct nofl_block_list *list, uintptr_t block) {
  atomic_fetch_add_explicit(&list->count, 1, memory_order_acq_rel);
  struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
  uintptr_t next = atomic_load_explicit(&list->blocks, memory_order_acquire);
  do {
    nofl_block_summary_set_next(summary, next);
  } while (!atomic_compare_exchange_weak(&list->blocks, &next, block));
}

static uintptr_t
nofl_pop_block(struct nofl_block_list *list) {
  uintptr_t head = atomic_load_explicit(&list->blocks, memory_order_acquire);
  struct nofl_block_summary *summary;
  uintptr_t next;
  do {
    if (!head)
      return 0;
    summary = nofl_block_summary_for_addr(head);
    next = nofl_block_summary_next(summary);
  } while (!atomic_compare_exchange_weak(&list->blocks, &head, next));
  nofl_block_summary_set_next(summary, 0);
  atomic_fetch_sub_explicit(&list->count, 1, memory_order_acq_rel);
  return head;
}

static inline size_t
nofl_size_to_granules(size_t size) {
  return (size + NOFL_GRANULE_SIZE - 1) >> NOFL_GRANULE_SIZE_LOG_2;
}

struct nofl_evacuation_allocator {
  size_t allocated; // atomically
  size_t limit;
  uintptr_t block_cursor; // atomically
};

struct nofl_space {
  uint64_t sweep_mask;
  uint8_t live_mask;
  uint8_t marked_mask;
  uint8_t evacuating;
  uintptr_t low_addr;
  size_t extent;
  size_t heap_size;
  uint8_t last_collection_was_minor;
  uintptr_t next_block;   // atomically
  struct nofl_block_list empty;
  struct nofl_block_list unavailable;
  struct nofl_block_list evacuation_targets;
  double evacuation_minimum_reserve;
  double evacuation_reserve;
  double venerable_threshold;
  ssize_t pending_unavailable_bytes; // atomically
  struct nofl_evacuation_allocator evacuation_allocator;
  struct nofl_slab *slabs;
  size_t nslabs;
  uintptr_t granules_freed_by_last_collection; // atomically
  uintptr_t fragmentation_granules_since_last_collection; // atomically
};

struct nofl_allocator {
  uintptr_t alloc;
  uintptr_t sweep;
  uintptr_t block;
};

static inline void
nofl_clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static size_t
nofl_space_live_object_granules(uint8_t *metadata) {
  return scan_for_byte(metadata, -1, broadcast_byte(NOFL_METADATA_BYTE_END)) + 1;
}

static inline int
nofl_space_mark_object(struct nofl_space *space, struct gc_ref ref) {
  uint8_t *loc = nofl_metadata_byte_for_object(ref);
  uint8_t byte = *loc;
  if (byte & space->marked_mask)
    return 0;
  uint8_t mask = NOFL_METADATA_BYTE_YOUNG | NOFL_METADATA_BYTE_MARK_0
    | NOFL_METADATA_BYTE_MARK_1 | NOFL_METADATA_BYTE_MARK_2;
  *loc = (byte & ~mask) | space->marked_mask;
  return 1;
}

static uintptr_t
nofl_make_evacuation_allocator_cursor(uintptr_t block, size_t allocated) {
  GC_ASSERT(allocated < (NOFL_BLOCK_SIZE - 1) * (uint64_t) NOFL_BLOCK_SIZE);
  return align_down(block, NOFL_BLOCK_SIZE) | (allocated / NOFL_BLOCK_SIZE);
}

static void
nofl_prepare_evacuation_allocator(struct nofl_evacuation_allocator *alloc,
                                  struct nofl_block_list *targets) {
  uintptr_t first_block = targets->blocks;
  atomic_store_explicit(&alloc->allocated, 0, memory_order_release);
  alloc->limit =
    atomic_load_explicit(&targets->count, memory_order_acquire) * NOFL_BLOCK_SIZE;
  atomic_store_explicit(&alloc->block_cursor,
                        nofl_make_evacuation_allocator_cursor(first_block, 0),
                        memory_order_release);
}

static void
nofl_clear_remaining_metadata_bytes_in_block(uintptr_t block,
                                             uintptr_t allocated) {
  GC_ASSERT((allocated & (NOFL_GRANULE_SIZE - 1)) == 0);
  uintptr_t base = block + allocated;
  uintptr_t limit = block + NOFL_BLOCK_SIZE;
  uintptr_t granules = (limit - base) >> NOFL_GRANULE_SIZE_LOG_2;
  GC_ASSERT(granules <= NOFL_GRANULES_PER_BLOCK);
  memset(nofl_metadata_byte_for_addr(base), 0, granules);
}

static void
nofl_finish_evacuation_allocator_block(uintptr_t block,
                                       uintptr_t allocated) {
  GC_ASSERT(allocated <= NOFL_BLOCK_SIZE);
  struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
  nofl_block_summary_set_flag(summary, NOFL_BLOCK_NEEDS_SWEEP);
  size_t fragmentation = (NOFL_BLOCK_SIZE - allocated) >> NOFL_GRANULE_SIZE_LOG_2;
  summary->hole_count = 1;
  summary->free_granules = NOFL_GRANULES_PER_BLOCK;
  summary->holes_with_fragmentation = fragmentation ? 1 : 0;
  summary->fragmentation_granules = fragmentation;
  if (fragmentation)
    nofl_clear_remaining_metadata_bytes_in_block(block, allocated);
}

static void
nofl_finish_evacuation_allocator(struct nofl_evacuation_allocator *alloc,
                                 struct nofl_block_list *targets,
                                 struct nofl_block_list *empties,
                                 size_t reserve) {
  // Blocks that we used for evacuation get returned to the mutator as
  // sweepable blocks.  Blocks that we didn't get to use go to the
  // empties.
  size_t allocated = atomic_load_explicit(&alloc->allocated,
                                          memory_order_acquire);
  atomic_store_explicit(&alloc->allocated, 0, memory_order_release);
  if (allocated > alloc->limit)
    allocated = alloc->limit;
  while (allocated >= NOFL_BLOCK_SIZE) {
    uintptr_t block = nofl_pop_block(targets);
    GC_ASSERT(block);
    allocated -= NOFL_BLOCK_SIZE;
  }
  if (allocated) {
    // Finish off the last partially-filled block.
    uintptr_t block = nofl_pop_block(targets);
    GC_ASSERT(block);
    nofl_finish_evacuation_allocator_block(block, allocated);
  }
  size_t remaining = atomic_load_explicit(&targets->count, memory_order_acquire);
  while (remaining-- > reserve)
    nofl_push_block(empties, nofl_pop_block(targets));
}

static struct gc_ref
nofl_evacuation_allocate(struct nofl_space *space, size_t granules) {
  // All collector threads compete to allocate from what is logically a
  // single bump-pointer arena, which is actually composed of a linked
  // list of blocks.
  struct nofl_evacuation_allocator *alloc = &space->evacuation_allocator;
  uintptr_t cursor = atomic_load_explicit(&alloc->block_cursor,
                                          memory_order_acquire);
  size_t bytes = granules * NOFL_GRANULE_SIZE;
  size_t prev = atomic_load_explicit(&alloc->allocated, memory_order_acquire);
  size_t block_mask = (NOFL_BLOCK_SIZE - 1);
  size_t next;
  do {
    if (prev >= alloc->limit)
      // No more space.
      return gc_ref_null();
    next = prev + bytes;
    if ((prev ^ next) & ~block_mask)
      // Allocation straddles a block boundary; advance so it starts a
      // fresh block.
      next = (next & ~block_mask) + bytes;
  } while (!atomic_compare_exchange_weak(&alloc->allocated, &prev, next));
  // OK, we've claimed our memory, starting at next - bytes.  Now find
  // the node in the linked list of evacuation targets that corresponds
  // to this allocation pointer.
  uintptr_t block = cursor & ~block_mask;
  // This is the SEQ'th block to be allocated into.
  uintptr_t seq = cursor & block_mask;
  // Therefore this block handles allocations starting at SEQ*BLOCK_SIZE
  // and continuing for NOFL_BLOCK_SIZE bytes.
  uintptr_t base = seq * NOFL_BLOCK_SIZE;

  while ((base ^ next) & ~block_mask) {
    GC_ASSERT(base < next);
    if (base + NOFL_BLOCK_SIZE > prev) {
      // The allocation straddles a block boundary, and the cursor has
      // caught up so that we identify the block for the previous
      // allocation pointer.  Finish the previous block, probably
      // leaving a small hole at the end.
      nofl_finish_evacuation_allocator_block(block, prev - base);
    }
    // Cursor lags; advance it.
    block = nofl_block_summary_next(nofl_block_summary_for_addr(block));
    base += NOFL_BLOCK_SIZE;
    if (base >= alloc->limit) {
      // Ran out of blocks!
      GC_ASSERT(!block);
      return gc_ref_null();
    }
    GC_ASSERT(block);
    // This store can race with other allocators, but that's OK as long
    // as it never advances the cursor beyond the allocation pointer,
    // which it won't because we updated the allocation pointer already.
    atomic_store_explicit(&alloc->block_cursor,
                          nofl_make_evacuation_allocator_cursor(block, base),
                          memory_order_release);
  }

  uintptr_t addr = block + (next & block_mask) - bytes;
  return gc_ref(addr);
}

static inline int
nofl_space_evacuate_or_mark_object(struct nofl_space *space,
                                   struct gc_edge edge,
                                   struct gc_ref old_ref) {
  uint8_t *metadata = nofl_metadata_byte_for_object(old_ref);
  uint8_t byte = *metadata;
  if (byte & space->marked_mask)
    return 0;
  if (space->evacuating &&
      nofl_block_summary_has_flag(nofl_block_summary_for_addr(gc_ref_value(old_ref)),
                                  NOFL_BLOCK_EVACUATE)) {
    // This is an evacuating collection, and we are attempting to
    // evacuate this block, and we are tracing this particular object
    // for what appears to be the first time.
    struct gc_atomic_forward fwd = gc_atomic_forward_begin(old_ref);

    if (fwd.state == GC_FORWARDING_STATE_NOT_FORWARDED)
      gc_atomic_forward_acquire(&fwd);

    switch (fwd.state) {
    case GC_FORWARDING_STATE_NOT_FORWARDED:
    case GC_FORWARDING_STATE_ABORTED:
      // Impossible.
      GC_CRASH();
    case GC_FORWARDING_STATE_ACQUIRED: {
      // We claimed the object successfully; evacuating is up to us.
      size_t object_granules = nofl_space_live_object_granules(metadata);
      struct gc_ref new_ref = nofl_evacuation_allocate(space, object_granules);
      if (gc_ref_is_heap_object(new_ref)) {
        // Copy object contents before committing, as we don't know what
        // part of the object (if any) will be overwritten by the
        // commit.
        memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(old_ref),
               object_granules * NOFL_GRANULE_SIZE);
        gc_atomic_forward_commit(&fwd, new_ref);
        // Now update extent metadata, and indicate to the caller that
        // the object's fields need to be traced.
        uint8_t *new_metadata = nofl_metadata_byte_for_object(new_ref);
        memcpy(new_metadata + 1, metadata + 1, object_granules - 1);
        gc_edge_update(edge, new_ref);
        metadata = new_metadata;
        // Fall through to set mark bits.
      } else {
        // Well shucks; allocation failed, marking the end of
        // opportunistic evacuation.  No future evacuation of this
        // object will succeed.  Mark in place instead.
        gc_atomic_forward_abort(&fwd);
      }
      break;
    }
    case GC_FORWARDING_STATE_BUSY:
      // Someone else claimed this object first.  Spin until new address
      // known, or evacuation aborts.
      for (size_t spin_count = 0;; spin_count++) {
        if (gc_atomic_forward_retry_busy(&fwd))
          break;
        yield_for_spin(spin_count);
      }
      if (fwd.state == GC_FORWARDING_STATE_ABORTED)
        // Remove evacuation aborted; remote will mark and enqueue.
        return 0;
      ASSERT(fwd.state == GC_FORWARDING_STATE_FORWARDED);
      // Fall through.
    case GC_FORWARDING_STATE_FORWARDED:
      // The object has been evacuated already.  Update the edge;
      // whoever forwarded the object will make sure it's eventually
      // traced.
      gc_edge_update(edge, gc_ref(gc_atomic_forward_address(&fwd)));
      return 0;
    }
  }

  uint8_t mask = NOFL_METADATA_BYTE_YOUNG | NOFL_METADATA_BYTE_MARK_0
    | NOFL_METADATA_BYTE_MARK_1 | NOFL_METADATA_BYTE_MARK_2;
  *metadata = (byte & ~mask) | space->marked_mask;
  return 1;
}

static inline int
nofl_space_contains_address(struct nofl_space *space, uintptr_t addr) {
  return addr - space->low_addr < space->extent;
}

static inline int
nofl_space_contains_conservative_ref(struct nofl_space *space,
                                     struct gc_conservative_ref ref) {
  return nofl_space_contains_address(space, gc_conservative_ref_value(ref));
}

static inline int
nofl_space_contains(struct nofl_space *space, struct gc_ref ref) {
  return nofl_space_contains_address(space, gc_ref_value(ref));
}

static int
nofl_space_forward_or_mark_if_traced(struct nofl_space *space,
                                     struct gc_edge edge,
                                     struct gc_ref ref) {
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = *metadata;
  if (byte & space->marked_mask)
    return 1;

  if (!space->evacuating)
    return 0;
  if (!nofl_block_summary_has_flag(nofl_block_summary_for_addr(gc_ref_value(ref)),
                                   NOFL_BLOCK_EVACUATE))
    return 0;

  struct gc_atomic_forward fwd = gc_atomic_forward_begin(ref);
  switch (fwd.state) {
  case GC_FORWARDING_STATE_NOT_FORWARDED:
    return 0;
  case GC_FORWARDING_STATE_BUSY:
    // Someone else claimed this object first.  Spin until new address
    // known, or evacuation aborts.
    for (size_t spin_count = 0;; spin_count++) {
      if (gc_atomic_forward_retry_busy(&fwd))
        break;
      yield_for_spin(spin_count);
    }
    if (fwd.state == GC_FORWARDING_STATE_ABORTED)
      // Remote evacuation aborted; remote will mark and enqueue.
      return 1;
    ASSERT(fwd.state == GC_FORWARDING_STATE_FORWARDED);
    // Fall through.
  case GC_FORWARDING_STATE_FORWARDED:
    gc_edge_update(edge, gc_ref(gc_atomic_forward_address(&fwd)));
    return 1;
  default:
    GC_CRASH();
  }
}

static inline struct gc_ref
nofl_space_mark_conservative_ref(struct nofl_space *space,
                                 struct gc_conservative_ref ref,
                                 int possibly_interior) {
  uintptr_t addr = gc_conservative_ref_value(ref);

  if (possibly_interior) {
    addr = align_down(addr, NOFL_GRANULE_SIZE);
  } else {
    // Addr not an aligned granule?  Not an object.
    uintptr_t displacement = addr & (NOFL_GRANULE_SIZE - 1);
    if (!gc_is_valid_conservative_ref_displacement(displacement))
      return gc_ref_null();
    addr -= displacement;
  }

  // Addr in meta block?  Not an object.
  if ((addr & (NOFL_SLAB_SIZE - 1)) < NOFL_META_BLOCKS_PER_SLAB * NOFL_BLOCK_SIZE)
    return gc_ref_null();

  // Addr in block that has been paged out?  Not an object.
  struct nofl_block_summary *summary = nofl_block_summary_for_addr(addr);
  if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE))
    return gc_ref_null();

  uint8_t *loc = nofl_metadata_byte_for_addr(addr);
  uint8_t byte = atomic_load_explicit(loc, memory_order_relaxed);

  // Already marked object?  Nothing to do.
  if (byte & space->marked_mask)
    return gc_ref_null();

  // Addr is the not start of an unmarked object?  Search backwards if
  // we have interior pointers, otherwise not an object.
  uint8_t object_start_mask = space->live_mask | NOFL_METADATA_BYTE_YOUNG;
  if (!(byte & object_start_mask)) {
    if (!possibly_interior)
      return gc_ref_null();

    uintptr_t block_base = align_down(addr, NOFL_BLOCK_SIZE);
    uint8_t *loc_base = nofl_metadata_byte_for_addr(block_base);
    do {
      // Searched past block?  Not an object.
      if (loc-- == loc_base)
        return gc_ref_null();

      byte = atomic_load_explicit(loc, memory_order_relaxed);

      // Ran into the end of some other allocation?  Not an object, then.
      if (byte & NOFL_METADATA_BYTE_END)
        return gc_ref_null();

      // Continue until we find object start.
    } while (!(byte & object_start_mask));

    // Found object start, and object is unmarked; adjust addr.
    addr = block_base + (loc - loc_base) * NOFL_GRANULE_SIZE;
  }

  uint8_t mask = NOFL_METADATA_BYTE_YOUNG | NOFL_METADATA_BYTE_MARK_0
    | NOFL_METADATA_BYTE_MARK_1 | NOFL_METADATA_BYTE_MARK_2;
  atomic_store_explicit(loc, (byte & ~mask) | space->marked_mask,
                        memory_order_relaxed);

  return gc_ref(addr);
}

static inline size_t
nofl_space_object_size(struct nofl_space *space, struct gc_ref ref) {
  uint8_t *loc = nofl_metadata_byte_for_object(ref);
  size_t granules = nofl_space_live_object_granules(loc);
  return granules * NOFL_GRANULE_SIZE;
}

static void
nofl_push_unavailable_block(struct nofl_space *space, uintptr_t block) {
  struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
  GC_ASSERT(!nofl_block_summary_has_flag(summary, NOFL_BLOCK_NEEDS_SWEEP));
  GC_ASSERT(!nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE));
  nofl_block_summary_set_flag(summary, NOFL_BLOCK_UNAVAILABLE);
  madvise((void*)block, NOFL_BLOCK_SIZE, MADV_DONTNEED);
  nofl_push_block(&space->unavailable, block);
}

static uintptr_t
nofl_pop_unavailable_block(struct nofl_space *space) {
  uintptr_t block = nofl_pop_block(&space->unavailable);
  if (!block)
    return 0;
  struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
  GC_ASSERT(nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE));
  nofl_block_summary_clear_flag(summary, NOFL_BLOCK_UNAVAILABLE);
  return block;
}

static uintptr_t
nofl_pop_empty_block(struct nofl_space *space) {
  return nofl_pop_block(&space->empty);
}

static int
nofl_maybe_push_evacuation_target(struct nofl_space *space,
                                  uintptr_t block, double reserve) {
  GC_ASSERT(!nofl_block_summary_has_flag(nofl_block_summary_for_addr(block),
                                         NOFL_BLOCK_NEEDS_SWEEP));
  size_t targets = atomic_load_explicit(&space->evacuation_targets.count,
                                        memory_order_acquire);
  size_t total = space->nslabs * NOFL_NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = atomic_load_explicit(&space->unavailable.count,
                                            memory_order_acquire);
  if (targets >= (total - unavailable) * reserve)
    return 0;

  nofl_push_block(&space->evacuation_targets, block);
  return 1;
}

static int
nofl_push_evacuation_target_if_needed(struct nofl_space *space,
                                      uintptr_t block) {
  return nofl_maybe_push_evacuation_target(space, block,
                                           space->evacuation_minimum_reserve);
}

static int
nofl_push_evacuation_target_if_possible(struct nofl_space *space,
                                        uintptr_t block) {
  return nofl_maybe_push_evacuation_target(space, block,
                                           space->evacuation_reserve);
}

static void
nofl_push_empty_block(struct nofl_space *space, uintptr_t block) {
  GC_ASSERT(!nofl_block_summary_has_flag(nofl_block_summary_for_addr(block),
                                         NOFL_BLOCK_NEEDS_SWEEP));
  nofl_push_block(&space->empty, block);
}

static ssize_t
nofl_space_request_release_memory(struct nofl_space *space, size_t bytes) {
  return atomic_fetch_add(&space->pending_unavailable_bytes, bytes) + bytes;
}

static void
nofl_space_reacquire_memory(struct nofl_space *space, size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->pending_unavailable_bytes, bytes) - bytes;
  while (pending + NOFL_BLOCK_SIZE <= 0) {
    uintptr_t block = nofl_pop_unavailable_block(space);
    GC_ASSERT(block);
    if (nofl_push_evacuation_target_if_needed(space, block))
      continue;
    nofl_push_empty_block(space, block);
    pending = atomic_fetch_add(&space->pending_unavailable_bytes, NOFL_BLOCK_SIZE)
      + NOFL_BLOCK_SIZE;
  }
}

static size_t
nofl_allocator_next_hole(struct nofl_allocator *alloc,
                         struct nofl_space *space);

static int
nofl_space_sweep_until_memory_released(struct nofl_space *space,
                                       struct nofl_allocator *alloc) {
  ssize_t pending = atomic_load_explicit(&space->pending_unavailable_bytes,
                                         memory_order_acquire);
  // First try to unmap previously-identified empty blocks.  If pending
  // > 0 and other mutators happen to identify empty blocks, they will
  // be unmapped directly and moved to the unavailable list.
  while (pending > 0) {
    uintptr_t block = nofl_pop_empty_block(space);
    if (!block)
      break;
    // Note that we may have competing uses; if we're evacuating,
    // perhaps we should push this block to the evacuation target list.
    // That would enable us to reach a fragmentation low water-mark in
    // fewer cycles.  But maybe evacuation started in order to obtain
    // free blocks for large objects; in that case we should just reap
    // the fruits of our labor.  Probably this second use-case is more
    // important.
    nofl_push_unavailable_block(space, block);
    pending = atomic_fetch_sub(&space->pending_unavailable_bytes, NOFL_BLOCK_SIZE);
    pending -= NOFL_BLOCK_SIZE;
  }
  // Otherwise, sweep, transitioning any empty blocks to unavailable and
  // throwing away any non-empty block.  A bit wasteful but hastening
  // the next collection is a reasonable thing to do here.
  while (pending > 0) {
    if (!nofl_allocator_next_hole(alloc, space))
      return 0;
    pending = atomic_load_explicit(&space->pending_unavailable_bytes,
                                   memory_order_acquire);
  }
  return pending <= 0;
}

static inline int
nofl_is_ephemeron(struct gc_ref ref) {
  uint8_t meta = *nofl_metadata_byte_for_addr(gc_ref_value(ref));
  return meta & NOFL_METADATA_BYTE_EPHEMERON;
}

static void
nofl_space_set_ephemeron_flag(struct gc_ref ref) {
  if (gc_has_conservative_intraheap_edges()) {
    uint8_t *metadata = nofl_metadata_byte_for_addr(gc_ref_value(ref));
    *metadata |= NOFL_METADATA_BYTE_EPHEMERON;
  }
}

static void nofl_finish_sweeping(struct nofl_allocator *alloc,
                                 struct nofl_space *space);
static void nofl_finish_sweeping_in_block(struct nofl_allocator *alloc,
                                          struct nofl_space *space);

// Note that it's quite possible (and even likely) that any given remset
// byte doesn't hold any roots, if all stores were to nursery objects.
STATIC_ASSERT_EQ(NOFL_GRANULES_PER_REMSET_BYTE % 8, 0);
static void
nofl_space_trace_card(struct nofl_space *space, struct nofl_slab *slab,
                      size_t card,
                      void (*enqueue)(struct gc_ref, struct gc_heap*),
                      struct gc_heap *heap) {
  uintptr_t first_addr_in_slab = (uintptr_t) &slab->blocks[0];
  size_t granule_base = card * NOFL_GRANULES_PER_REMSET_BYTE;
  for (size_t granule_in_remset = 0;
       granule_in_remset < NOFL_GRANULES_PER_REMSET_BYTE;
       granule_in_remset += 8, granule_base += 8) {
    uint64_t mark_bytes = load_eight_aligned_bytes(slab->metadata + granule_base);
    mark_bytes &= space->sweep_mask;
    while (mark_bytes) {
      size_t granule_offset = count_zero_bytes(mark_bytes);
      mark_bytes &= ~(((uint64_t)0xff) << (granule_offset * 8));
      size_t granule = granule_base + granule_offset;
      uintptr_t addr = first_addr_in_slab + granule * NOFL_GRANULE_SIZE;
      GC_ASSERT(nofl_metadata_byte_for_addr(addr) == &slab->metadata[granule]);
      enqueue(gc_ref(addr), heap);
    }
  }
}

static void
nofl_space_trace_remembered_set(struct nofl_space *space,
                                void (*enqueue)(struct gc_ref,
                                                struct gc_heap*),
                                struct gc_heap *heap) {
  GC_ASSERT(!space->evacuating);
  for (size_t s = 0; s < space->nslabs; s++) {
    struct nofl_slab *slab = &space->slabs[s];
    uint8_t *remset = slab->remembered_set;
    for (size_t card_base = 0;
         card_base < NOFL_REMSET_BYTES_PER_SLAB;
         card_base += 8) {
      uint64_t remset_bytes = load_eight_aligned_bytes(remset + card_base);
      if (!remset_bytes) continue;
      memset(remset + card_base, 0, 8);
      while (remset_bytes) {
        size_t card_offset = count_zero_bytes(remset_bytes);
        remset_bytes &= ~(((uint64_t)0xff) << (card_offset * 8));
        nofl_space_trace_card(space, slab, card_base + card_offset,
                              enqueue, heap);
      }
    }
  }
}

static void
nofl_space_clear_remembered_set(struct nofl_space *space) {
  if (!GC_GENERATIONAL) return;
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    memset(space->slabs[slab].remembered_set, 0, NOFL_REMSET_BYTES_PER_SLAB);
  }
}

static void
nofl_space_reset_sweeper(struct nofl_space *space) {
  space->next_block = (uintptr_t) &space->slabs[0].blocks;
}

static void
nofl_space_update_mark_patterns(struct nofl_space *space,
                                int advance_mark_mask) {
  uint8_t survivor_mask = space->marked_mask;
  uint8_t next_marked_mask = nofl_rotate_dead_survivor_marked(survivor_mask);
  if (advance_mark_mask)
    space->marked_mask = next_marked_mask;
  space->live_mask = survivor_mask | next_marked_mask;
  space->sweep_mask = broadcast_byte(space->live_mask);
}

static void
nofl_space_reset_statistics(struct nofl_space *space) {
  space->granules_freed_by_last_collection = 0;
  space->fragmentation_granules_since_last_collection = 0;
}

static size_t
nofl_space_yield(struct nofl_space *space) {
  return space->granules_freed_by_last_collection * NOFL_GRANULE_SIZE;
}

static size_t
nofl_space_evacuation_reserve(struct nofl_space *space) {
  return atomic_load_explicit(&space->evacuation_targets.count,
                              memory_order_acquire) * NOFL_BLOCK_SIZE;
}

static size_t
nofl_space_fragmentation(struct nofl_space *space) {
  size_t granules = space->fragmentation_granules_since_last_collection;
  return granules * NOFL_GRANULE_SIZE;
}

static void
nofl_space_release_evacuation_target_blocks(struct nofl_space *space) {
  // Move excess evacuation target blocks back to empties.
  size_t total = space->nslabs * NOFL_NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = atomic_load_explicit(&space->unavailable.count,
                                            memory_order_acquire);
  size_t reserve = space->evacuation_minimum_reserve * (total - unavailable);
  nofl_finish_evacuation_allocator(&space->evacuation_allocator,
                                   &space->evacuation_targets,
                                   &space->empty,
                                   reserve);
}

static void
nofl_space_prepare_for_evacuation(struct nofl_space *space,
                                  enum gc_collection_kind gc_kind) {
  if (gc_kind != GC_COLLECTION_COMPACTING) {
    space->evacuating = 0;
    space->evacuation_reserve = space->evacuation_minimum_reserve;
    return;
  }

  // Put the mutator into evacuation mode, collecting up to 50% of free space as
  // evacuation blocks.
  space->evacuation_reserve = 0.5;

  size_t target_blocks = space->evacuation_targets.count;
  DEBUG("evacuation target block count: %zu\n", target_blocks);

  if (target_blocks == 0) {
    DEBUG("no evacuation target blocks, disabling evacuation for this round\n");
    space->evacuating = 0;
    return;
  }

  size_t target_granules = target_blocks * NOFL_GRANULES_PER_BLOCK;
  // Compute histogram where domain is the number of granules in a block
  // that survived the last collection, aggregated into 33 buckets, and
  // range is number of blocks in that bucket.  (Bucket 0 is for blocks
  // that were found to be completely empty; such blocks may be on the
  // evacuation target list.)
  const size_t bucket_count = 33;
  size_t histogram[33] = {0,};
  size_t bucket_size = NOFL_GRANULES_PER_BLOCK / 32;
  size_t empties = 0;
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    for (size_t block = 0; block < NOFL_NONMETA_BLOCKS_PER_SLAB; block++) {
      struct nofl_block_summary *summary = &space->slabs[slab].summaries[block];
      if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE))
        continue;
      if (!nofl_block_summary_has_flag(summary, NOFL_BLOCK_NEEDS_SWEEP)) {
        empties++;
        continue;
      }
      size_t survivor_granules = NOFL_GRANULES_PER_BLOCK - summary->free_granules;
      size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
      histogram[bucket]++;
    }
  }

  // Blocks which lack the NEEDS_SWEEP flag are empty, either because
  // they have been removed from the pool and have the UNAVAILABLE flag
  // set, or because they are on the empties or evacuation target
  // lists.  When evacuation starts, the empties list should be empty.
  GC_ASSERT(empties == target_blocks);

  // Now select a number of blocks that is likely to fill the space in
  // the target blocks.  Prefer candidate blocks with fewer survivors
  // from the last GC, to increase expected free block yield.
  for (size_t bucket = 0; bucket < bucket_count; bucket++) {
    size_t bucket_granules = bucket * bucket_size * histogram[bucket];
    if (bucket_granules <= target_granules) {
      target_granules -= bucket_granules;
    } else {
      histogram[bucket] = target_granules / (bucket_size * bucket);
      target_granules = 0;
    }
  }

  // Having selected the number of blocks, now we set the evacuation
  // candidate flag on all blocks.
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    for (size_t block = 0; block < NOFL_NONMETA_BLOCKS_PER_SLAB; block++) {
      struct nofl_block_summary *summary = &space->slabs[slab].summaries[block];
      if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE))
        continue;
      if (!nofl_block_summary_has_flag(summary, NOFL_BLOCK_NEEDS_SWEEP))
        continue;
      size_t survivor_granules = NOFL_GRANULES_PER_BLOCK - summary->free_granules;
      size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
      if (histogram[bucket]) {
        nofl_block_summary_set_flag(summary, NOFL_BLOCK_EVACUATE);
        histogram[bucket]--;
      } else {
        nofl_block_summary_clear_flag(summary, NOFL_BLOCK_EVACUATE);
      }
    }
  }

  // We are ready to evacuate!
  nofl_prepare_evacuation_allocator(&space->evacuation_allocator,
                                    &space->evacuation_targets);
  space->evacuating = 1;
}

static void
nofl_space_verify_before_restart(struct nofl_space *space) {
  // Iterate objects in each block, verifying that the END bytes correspond to
  // the measured object size.
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    for (size_t block = 0; block < NOFL_NONMETA_BLOCKS_PER_SLAB; block++) {
      struct nofl_block_summary *summary = &space->slabs[slab].summaries[block];
      if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE))
        continue;

      uintptr_t addr = (uintptr_t)space->slabs[slab].blocks[block].data;
      uintptr_t limit = addr + NOFL_BLOCK_SIZE;
      uint8_t *meta = nofl_metadata_byte_for_addr(addr);
      while (addr < limit) {
        if (meta[0] & space->live_mask) {
          struct gc_ref obj = gc_ref(addr);
          size_t obj_bytes;
          gc_trace_object(obj, NULL, NULL, NULL, &obj_bytes);
          size_t granules = nofl_size_to_granules(obj_bytes);
          GC_ASSERT(granules);
          for (size_t granule = 0; granule < granules - 1; granule++)
            GC_ASSERT(!(meta[granule] & NOFL_METADATA_BYTE_END));
          GC_ASSERT(meta[granules - 1] & NOFL_METADATA_BYTE_END);
          meta += granules;
          addr += granules * NOFL_GRANULE_SIZE;
        } else {
          meta++;
          addr += NOFL_GRANULE_SIZE;
        }
      }
      GC_ASSERT(addr == limit);
    }
  }
}

static void
nofl_space_finish_gc(struct nofl_space *space,
                     enum gc_collection_kind gc_kind) {
  space->evacuating = 0;
  space->last_collection_was_minor = (gc_kind == GC_COLLECTION_MINOR);
  nofl_space_reset_sweeper(space);
  nofl_space_update_mark_patterns(space, 0);
  nofl_space_reset_statistics(space);
  nofl_space_release_evacuation_target_blocks(space);
  if (GC_DEBUG)
    nofl_space_verify_before_restart(space);
}

static int
nofl_sweep_byte(uint8_t *loc, uintptr_t sweep_mask) {
  uint8_t metadata = atomic_load_explicit(loc, memory_order_relaxed);
  // If the metadata byte is nonzero, that means either a young, dead,
  // survived, or marked object.  If it's live (survived or marked), we
  // found the next mark.  Otherwise it's dead and we clear the byte.
  // If we see an END, that means an end of a dead object; clear it.
  if (metadata) {
    if (metadata & sweep_mask)
      return 1;
    atomic_store_explicit(loc, 0, memory_order_relaxed);
  }
  return 0;
}

static int
nofl_sweep_word(uintptr_t *loc, uintptr_t sweep_mask) {
  uintptr_t metadata = atomic_load_explicit(loc, memory_order_relaxed);
  if (metadata) {
    if (metadata & sweep_mask)
      return 1;
    atomic_store_explicit(loc, 0, memory_order_relaxed);
  }
  return 0;
}

static uintptr_t
nofl_space_next_block_to_sweep(struct nofl_space *space) {
  uintptr_t block = atomic_load_explicit(&space->next_block,
                                         memory_order_acquire);
  uintptr_t next_block;
  do {
    if (block == 0)
      return 0;

    next_block = block + NOFL_BLOCK_SIZE;
    if (next_block % NOFL_SLAB_SIZE == 0) {
      uintptr_t hi_addr = space->low_addr + space->extent;
      if (next_block == hi_addr)
        next_block = 0;
      else
        next_block += NOFL_META_BLOCKS_PER_SLAB * NOFL_BLOCK_SIZE;
    }
  } while (!atomic_compare_exchange_weak(&space->next_block, &block,
                                         next_block));
  return block;
}

static void
nofl_allocator_release_block(struct nofl_allocator *alloc) {
  alloc->alloc = alloc->sweep = alloc->block = 0;
}

static void
nofl_allocator_finish_block(struct nofl_allocator *alloc,
                            struct nofl_space *space) {
  GC_ASSERT(alloc->block);
  struct nofl_block_summary *block = nofl_block_summary_for_addr(alloc->block);
  atomic_fetch_add(&space->granules_freed_by_last_collection,
                   block->free_granules);
  atomic_fetch_add(&space->fragmentation_granules_since_last_collection,
                   block->fragmentation_granules);

  // If this block has mostly survivors, we should avoid sweeping it and
  // trying to allocate into it for a minor GC.  Sweep it next time to
  // clear any garbage allocated in this cycle and mark it as
  // "venerable" (i.e., old).
  GC_ASSERT(!nofl_block_summary_has_flag(block, NOFL_BLOCK_VENERABLE));
  if (!nofl_block_summary_has_flag(block, NOFL_BLOCK_VENERABLE_AFTER_SWEEP) &&
      block->free_granules < NOFL_GRANULES_PER_BLOCK * space->venerable_threshold)
    nofl_block_summary_set_flag(block, NOFL_BLOCK_VENERABLE_AFTER_SWEEP);

  nofl_allocator_release_block(alloc);
}

// Sweep some heap to reclaim free space, resetting alloc->alloc and
// alloc->sweep.  Return the size of the hole in granules.
static size_t
nofl_allocator_next_hole_in_block(struct nofl_allocator *alloc,
                                  struct nofl_space *space) {
  uintptr_t sweep = alloc->sweep;
  if (sweep == 0)
    return 0;
  uintptr_t limit = alloc->block + NOFL_BLOCK_SIZE;
  uintptr_t sweep_mask = space->sweep_mask;

  while (sweep != limit) {
    GC_ASSERT((sweep & (NOFL_GRANULE_SIZE - 1)) == 0);
    uint8_t* metadata = nofl_metadata_byte_for_addr(sweep);
    size_t limit_granules = (limit - sweep) >> NOFL_GRANULE_SIZE_LOG_2;

    // Except for when we first get a block, alloc->sweep is positioned
    // right after a hole, which can point to either the end of the
    // block or to a live object.  Assume that a live object is more
    // common.
    {
      size_t live_granules = 0;
      while (limit_granules && (metadata[0] & sweep_mask)) {
        // Object survived collection; skip over it and continue sweeping.
        size_t object_granules = nofl_space_live_object_granules(metadata);
        live_granules += object_granules;
        limit_granules -= object_granules;
        metadata += object_granules;
      }
      if (!limit_granules)
        break;
      sweep += live_granules * NOFL_GRANULE_SIZE;
    }

    size_t free_granules = scan_for_byte(metadata, limit_granules, sweep_mask);
    GC_ASSERT(free_granules);
    GC_ASSERT(free_granules <= limit_granules);

    struct nofl_block_summary *summary = nofl_block_summary_for_addr(sweep);
    summary->hole_count++;
    GC_ASSERT(free_granules <= NOFL_GRANULES_PER_BLOCK - summary->free_granules);
    summary->free_granules += free_granules;

    size_t free_bytes = free_granules * NOFL_GRANULE_SIZE;
    alloc->alloc = sweep;
    alloc->sweep = sweep + free_bytes;
    return free_granules;
  }

  nofl_allocator_finish_block(alloc, space);
  return 0;
}

static void
nofl_allocator_finish_hole(struct nofl_allocator *alloc) {
  size_t granules = (alloc->sweep - alloc->alloc) / NOFL_GRANULE_SIZE;
  if (granules) {
    struct nofl_block_summary *summary = nofl_block_summary_for_addr(alloc->block);
    summary->holes_with_fragmentation++;
    summary->fragmentation_granules += granules;
    uint8_t *metadata = nofl_metadata_byte_for_addr(alloc->alloc);
    memset(metadata, 0, granules);
    alloc->alloc = alloc->sweep;
  }
  // FIXME: add to fragmentation
}

static int
nofl_maybe_release_swept_empty_block(struct nofl_allocator *alloc,
                                     struct nofl_space *space) {
  GC_ASSERT(alloc->block);
  uintptr_t block = alloc->block;
  if (atomic_load_explicit(&space->pending_unavailable_bytes,
                           memory_order_acquire) <= 0)
    return 0;

  nofl_push_unavailable_block(space, block);
  atomic_fetch_sub(&space->pending_unavailable_bytes, NOFL_BLOCK_SIZE);
  nofl_allocator_release_block(alloc);
  return 1;
}

static size_t
nofl_allocator_next_hole(struct nofl_allocator *alloc,
                         struct nofl_space *space) {
  nofl_allocator_finish_hole(alloc);
  // As we sweep if we find that a block is empty, we return it to the
  // empties list.  Empties are precious.  But if we return 10 blocks in
  // a row, and still find an 11th empty, go ahead and use it.
  size_t empties_countdown = 10;
  while (1) {
    // Sweep current block for a hole.
    size_t granules = nofl_allocator_next_hole_in_block(alloc, space);
    if (granules) {
      // If the hole spans only part of a block, let the allocator try
      // to use it.
      if (granules < NOFL_GRANULES_PER_BLOCK)
        return granules;
      struct nofl_block_summary *summary = nofl_block_summary_for_addr(alloc->block);
      memset(nofl_metadata_byte_for_addr(alloc->block), 0, NOFL_GRANULES_PER_BLOCK);
      nofl_block_summary_clear_flag(summary, NOFL_BLOCK_NEEDS_SWEEP);
      // Sweeping found a completely empty block.  If we are below the
      // minimum evacuation reserve, take the block.
      if (nofl_push_evacuation_target_if_needed(space, alloc->block)) {
        nofl_allocator_release_block(alloc);
        continue;
      }
      // If we have pending pages to release to the OS, we should unmap
      // this block.
      if (nofl_maybe_release_swept_empty_block(alloc, space))
        continue;
      // Otherwise if we've already returned lots of empty blocks to the
      // freelist, let the allocator keep this block.
      if (!empties_countdown) {
        // After this block is allocated into, it will need to be swept.
        nofl_block_summary_set_flag(summary, NOFL_BLOCK_NEEDS_SWEEP);
        return granules;
      }
      // Otherwise we push to the empty blocks list.
      nofl_push_empty_block(space, alloc->block);
      nofl_allocator_release_block(alloc);
      empties_countdown--;
    }
    GC_ASSERT(alloc->block == 0);
    while (1) {
      uintptr_t block = nofl_space_next_block_to_sweep(space);
      if (block) {
        // Sweeping found a block.  We might take it for allocation, or
        // we might send it back.
        struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
        // If it's marked unavailable, it's already on a list of
        // unavailable blocks, so skip and get the next block.
        if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_UNAVAILABLE))
          continue;
        if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_VENERABLE)) {
          // Skip venerable blocks after a minor GC -- we don't need to
          // sweep as they weren't allocated into last cycle, and the
          // mark bytes didn't rotate, so we have no cleanup to do; and
          // we shouldn't try to allocate into them as it's not worth
          // it.  Any wasted space is measured as fragmentation.
          if (space->last_collection_was_minor)
            continue;
          else
            nofl_block_summary_clear_flag(summary, NOFL_BLOCK_VENERABLE);
        }
        if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_NEEDS_SWEEP)) {
          // Prepare to sweep the block for holes.
          alloc->alloc = alloc->sweep = alloc->block = block;
          if (nofl_block_summary_has_flag(summary, NOFL_BLOCK_VENERABLE_AFTER_SWEEP)) {
            // In the last cycle we noted that this block consists of
            // mostly old data.  Sweep any garbage, commit the mark as
            // venerable, and avoid allocating into it.
            nofl_block_summary_clear_flag(summary, NOFL_BLOCK_VENERABLE_AFTER_SWEEP);
            if (space->last_collection_was_minor) {
              nofl_finish_sweeping_in_block(alloc, space);
              nofl_block_summary_set_flag(summary, NOFL_BLOCK_VENERABLE);
              continue;
            }
          }
          // This block was marked in the last GC and needs sweeping.
          // As we sweep we'll want to record how many bytes were live
          // at the last collection.  As we allocate we'll record how
          // many granules were wasted because of fragmentation.
          summary->hole_count = 0;
          summary->free_granules = 0;
          summary->holes_with_fragmentation = 0;
          summary->fragmentation_granules = 0;
          break;
        } else {
          // Otherwise this block is completely empty and is on the
          // empties list.  We take from the empties list only after all
          // the NEEDS_SWEEP blocks are processed.
          continue;
        }
      } else {
        // We are done sweeping for blocks.  Now take from the empties
        // list.
        block = nofl_pop_empty_block(space);
        // No empty block?  Return 0 to cause collection.
        if (!block)
          return 0;

        // Maybe we should use this empty as a target for evacuation.
        if (nofl_push_evacuation_target_if_possible(space, block))
          continue;

        // Otherwise give the block to the allocator.
        struct nofl_block_summary *summary = nofl_block_summary_for_addr(block);
        nofl_block_summary_set_flag(summary, NOFL_BLOCK_NEEDS_SWEEP);
        summary->hole_count = 1;
        summary->free_granules = NOFL_GRANULES_PER_BLOCK;
        summary->holes_with_fragmentation = 0;
        summary->fragmentation_granules = 0;
        alloc->block = block;
        alloc->alloc = block;
        alloc->sweep = block + NOFL_BLOCK_SIZE;
        return NOFL_GRANULES_PER_BLOCK;
      }
    }
  }
}

static void
nofl_finish_sweeping_in_block(struct nofl_allocator *alloc,
                              struct nofl_space *space) {
  do {
    nofl_allocator_finish_hole(alloc);
  } while (nofl_allocator_next_hole_in_block(alloc, space));
}

// Another thread is triggering GC.  Before we stop, finish clearing the
// dead mark bytes for the mutator's block, and release the block.
static void
nofl_finish_sweeping(struct nofl_allocator *alloc,
                     struct nofl_space *space) {
  while (nofl_allocator_next_hole(alloc, space)) {}
}

static struct gc_ref
nofl_allocate(struct nofl_allocator *alloc, struct nofl_space *space,
              size_t size, void (*gc)(void*), void *gc_data) {
  GC_ASSERT(size > 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());
  size = align_up(size, NOFL_GRANULE_SIZE);

  if (alloc->alloc + size > alloc->sweep) {
    size_t granules = size >> NOFL_GRANULE_SIZE_LOG_2;
    while (1) {
      size_t hole = nofl_allocator_next_hole(alloc, space);
      if (hole >= granules) {
        nofl_clear_memory(alloc->alloc, hole * NOFL_GRANULE_SIZE);
        break;
      }
      if (!hole)
        gc(gc_data);
    }
  }

  struct gc_ref ret = gc_ref(alloc->alloc);
  alloc->alloc += size;
  gc_update_alloc_table(ret, size);
  return ret;
}

static struct nofl_slab*
nofl_allocate_slabs(size_t nslabs) {
  size_t size = nslabs * NOFL_SLAB_SIZE;
  size_t extent = size + NOFL_SLAB_SIZE;

  char *mem = mmap(NULL, extent, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return NULL;
  }

  uintptr_t base = (uintptr_t) mem;
  uintptr_t end = base + extent;
  uintptr_t aligned_base = align_up(base, NOFL_SLAB_SIZE);
  uintptr_t aligned_end = aligned_base + size;

  if (aligned_base - base)
    munmap((void*)base, aligned_base - base);
  if (end - aligned_end)
    munmap((void*)aligned_end, end - aligned_end);

  return (struct nofl_slab*) aligned_base;
}

static int
nofl_space_init(struct nofl_space *space, size_t size, int atomic,
                double venerable_threshold) {
  size = align_up(size, NOFL_BLOCK_SIZE);
  size_t reserved = align_up(size, NOFL_SLAB_SIZE);
  size_t nslabs = reserved / NOFL_SLAB_SIZE;
  struct nofl_slab *slabs = nofl_allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  space->marked_mask = NOFL_METADATA_BYTE_MARK_0;
  nofl_space_update_mark_patterns(space, 0);
  space->slabs = slabs;
  space->nslabs = nslabs;
  space->low_addr = (uintptr_t) slabs;
  space->extent = reserved;
  space->next_block = 0;
  space->evacuation_minimum_reserve = 0.02;
  space->evacuation_reserve = space->evacuation_minimum_reserve;
  space->venerable_threshold = venerable_threshold;
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t block = 0; block < NOFL_NONMETA_BLOCKS_PER_SLAB; block++) {
      uintptr_t addr = (uintptr_t)slabs[slab].blocks[block].data;
      if (reserved > size) {
        nofl_push_unavailable_block(space, addr);
        reserved -= NOFL_BLOCK_SIZE;
      } else {
        if (!nofl_push_evacuation_target_if_needed(space, addr))
          nofl_push_empty_block(space, addr);
      }
    }
  }
  return 1;
}

#endif // NOFL_SPACE_H
