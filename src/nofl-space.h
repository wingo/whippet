#ifndef NOFL_SPACE_H
#define NOFL_SPACE_H

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "assert.h"
#include "debug.h"
#include "extents.h"
#include "gc-align.h"
#include "gc-attrs.h"
#include "gc-inline.h"
#include "gc-lock.h"
#include "gc-platform.h"
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

#include "nofl-holeset.h"

#define NOFL_SLAB_SIZE (4 * 1024 * 1024)
#define NOFL_BLOCK_SIZE (64 * 1024)
#define NOFL_METADATA_BYTES_PER_BLOCK (NOFL_BLOCK_SIZE / NOFL_GRANULE_SIZE)
#define NOFL_BLOCKS_PER_SLAB (NOFL_SLAB_SIZE / NOFL_BLOCK_SIZE)
#define NOFL_META_BLOCKS_PER_SLAB (NOFL_METADATA_BYTES_PER_BLOCK * NOFL_BLOCKS_PER_SLAB / NOFL_BLOCK_SIZE)
#define NOFL_NONMETA_BLOCKS_PER_SLAB (NOFL_BLOCKS_PER_SLAB - NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_METADATA_BYTES_PER_SLAB (NOFL_NONMETA_BLOCKS_PER_SLAB * NOFL_METADATA_BYTES_PER_BLOCK)
#define NOFL_SLACK_METADATA_BYTES_PER_SLAB (NOFL_META_BLOCKS_PER_SLAB * NOFL_METADATA_BYTES_PER_BLOCK)
#define NOFL_VESTIGIAL_BYTES_PER_BLOCK (NOFL_SLACK_METADATA_BYTES_PER_SLAB / NOFL_BLOCKS_PER_SLAB)
#define NOFL_VESTIGIAL_BYTES_PER_SLAB (NOFL_VESTIGIAL_BYTES_PER_BLOCK * NOFL_NONMETA_BLOCKS_PER_SLAB)
#define NOFL_SLACK_VESTIGIAL_BYTES_PER_SLAB (NOFL_VESTIGIAL_BYTES_PER_BLOCK * NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_SUMMARY_BYTES_PER_BLOCK (NOFL_SLACK_VESTIGIAL_BYTES_PER_SLAB / NOFL_BLOCKS_PER_SLAB)
#define NOFL_SUMMARY_BYTES_PER_SLAB (NOFL_SUMMARY_BYTES_PER_BLOCK * NONMETA_BLOCKS_PER_SLAB)
#define NOFL_SLACK_SUMMARY_BYTES_PER_SLAB (NOFL_SUMMARY_BYTES_PER_BLOCK * NOFL_META_BLOCKS_PER_SLAB)
#define NOFL_HEADER_BYTES_PER_SLAB NOFL_SLACK_SUMMARY_BYTES_PER_SLAB

struct nofl_slab;

struct nofl_slab_header {
  union {
    struct {
      uint8_t block_marks[NOFL_BLOCKS_PER_SLAB];
    };
    uint8_t padding[NOFL_HEADER_BYTES_PER_SLAB];
  };
};
STATIC_ASSERT_EQ(sizeof(struct nofl_slab_header), NOFL_HEADER_BYTES_PER_SLAB);

// Sometimes we want to put a block on a singly-linked list.  For that
// there's a pointer reserved in the block summary.  But because the
// pointer is aligned (32kB on 32-bit, 64kB on 64-bit), we can portably
// hide up to 15 flags in the low bits.  These flags are accessed
// non-atomically, in two situations: one, when a block is not on a
// list, which guarantees that no other thread can access it; or when no
// pushing or popping is happening, for example during an evacuation
// cycle.
enum nofl_block_summary_flag {
  NOFL_BLOCK_EVACUATE = 0x1,
  NOFL_BLOCK_ZERO = 0x2,
  NOFL_BLOCK_UNAVAILABLE = 0x4,
  NOFL_BLOCK_PAGED_OUT = 0x8,
  NOFL_BLOCK_FLAG_UNUSED_3 = 0x8,
  NOFL_BLOCK_FLAG_UNUSED_4 = 0x10,
  NOFL_BLOCK_FLAG_UNUSED_5 = 0x20,
  NOFL_BLOCK_FLAG_UNUSED_6 = 0x40,
  NOFL_BLOCK_FLAG_UNUSED_7 = 0x80,
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
      uint16_t hole_granules;
      // Counters related to allocation since previous collection:
      // wasted space due to fragmentation.  Also used by blocks on the
      // "partly full" list, which have zero holes_with_fragmentation
      // but nonzero fragmentation_granules.
      uint16_t holes_with_fragmentation;
      uint16_t fragmentation_granules;
      // Next pointer, and flags in low bits.  See comment above
      // regarding enum nofl_block_summary_flag.
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

struct nofl_block_ref {
  struct nofl_block_summary *summary;
  uintptr_t addr;
};

struct nofl_slab {
  struct nofl_slab_header header;
  struct nofl_block_summary summaries[NOFL_NONMETA_BLOCKS_PER_SLAB];
  uint8_t unused[NOFL_VESTIGIAL_BYTES_PER_SLAB];
  uint8_t metadata[NOFL_METADATA_BYTES_PER_SLAB];
  struct nofl_block blocks[NOFL_NONMETA_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct nofl_slab), NOFL_SLAB_SIZE);

// Lock-free block list, which either only has threads removing items
// from it or only has threads adding items to it -- i.e., adding and
// removing items don't happen concurrently.
struct nofl_block_list {
  size_t count;
  uintptr_t blocks;
};

// A block list that has concurrent threads adding and removing items
// from it.
struct nofl_block_stack {
  struct nofl_block_list list;
};

#define NOFL_PAGE_OUT_QUEUE_SIZE 4

struct nofl_space {
  uint8_t current_mark;
  uint8_t survivor_mark;
  uint8_t evacuating;
  struct extents *extents;
  size_t heap_size;
  uint8_t last_collection_was_minor;
  uint8_t heap_has_ambiguous_edges;
  struct nofl_block_stack empty;
  struct nofl_block_stack paged_out[NOFL_PAGE_OUT_QUEUE_SIZE];
  struct nofl_block_list to_sweep;
  struct nofl_block_stack partly_full;
  struct nofl_block_list full;
  struct nofl_block_list promoted;
  struct nofl_block_list old;
  struct nofl_block_list evacuation_targets;
  struct nofl_holeset holes;
  pthread_mutex_t lock;
  double evacuation_minimum_reserve;
  double evacuation_reserve;
  double promotion_threshold;
  ssize_t pending_unavailable_bytes; // atomically
  struct nofl_slab **slabs;
  size_t nslabs;
  uintptr_t old_generation_granules; // atomically
  uintptr_t survivor_granules_at_last_collection; // atomically
  uintptr_t allocated_granules_since_last_collection; // atomically
  uintptr_t fragmentation_granules_since_last_collection; // atomically
};

struct nofl_allocator {
  uintptr_t alloc;
  uintptr_t sweep;
  struct nofl_block_ref block;
  struct nofl_holeset holes;
};

#if GC_CONSERVATIVE_TRACE && GC_CONCURRENT_TRACE
// There are just not enough bits in the mark table.
#error Unsupported configuration
#endif

// Each granule has one mark byte stored in a side table.  A granule's
// mark state is a whole byte instead of a bit to facilitate parallel
// marking.  (Parallel markers are allowed to race.)  We also use this
// byte to compute object extent, via a bit flag indicating
// end-of-object.
//
// Because we want to allow for conservative roots, we need to know
// whether an address indicates an object or not.  That means that when
// an object is allocated, it has to set a bit, somewhere.  We use the
// metadata byte for this purpose, setting the "young" mark.
//
// The "young" mark's name might make you think about generational
// collection, and indeed all objects collected in a minor collection
// will have this bit set.  However, the nofl space never needs to check
// for the young mark; if it weren't for the need to identify
// conservative roots, we wouldn't need a young mark at all.  Perhaps in
// an all-precise system, we would be able to avoid the overhead of
// initializing mark byte upon each fresh allocation.
//
// When an object becomes dead after a GC, it will still have a mark set
// -- maybe the young mark, or maybe a survivor mark.  The sweeper has
// to clear these marks before the next collection.  The same goes for
// "forwarded" marks left in the metadata of evacuated objects.  If we
// add concurrent marking, we will also be marking "live" objects,
// updating their mark bits.  So there are three and possibly four
// object states concurrently observable:  young, dead, survivor, and
// marked.  (We don't currently have concurrent marking, though.)  We
// store this state in the low 3 bits of the byte.  After each major
// collection, the dead, survivor, and marked states rotate.
//
// It can be useful to support "raw" allocations, most often
// pointerless, but for compatibility with BDW-GC, sometimes
// conservatively-traced tagless data.  We reserve one or two bits for
// the "kind" of the allocation: either a normal object traceable via
// `gc_trace_object`, a pointerless untagged allocation that doesn't
// need tracing, an allocation that should be traced conservatively.
// The latter state is only used when conservative tracing is enabled.
//
// An object can be pinned, preventing it from being evacuated during
// collection.  Pinning does not keep the object alive; if it is
// otherwise unreachable, it will be collected.  To pin an object, a
// running mutator can set the pinned bit, using atomic
// compare-and-swap.  This bit overlaps the "trace conservatively" trace
// kind, but that's OK because we don't use the pinned bit in those
// cases, as all objects are implicitly pinned.
//
// For generational collectors, the nofl space supports a field-logging
// write barrier.  The two logging bits correspond to the two words in a
// granule.  When a field is written to, the write barrier should check
// the logged bit; if it is unset, it should try to atomically set the
// bit, and if that works, then we record the field location as a
// generational root, adding it to a sequential-store buffer.
enum nofl_metadata_byte {
  NOFL_METADATA_BYTE_NONE = 0,
  NOFL_METADATA_BYTE_YOUNG = 1,
  NOFL_METADATA_BYTE_MARK_0 = 2,
  NOFL_METADATA_BYTE_MARK_1 = 3,
  NOFL_METADATA_BYTE_MARK_2 = 4,
  NOFL_METADATA_BYTE_BUSY = 5,
  NOFL_METADATA_BYTE_FORWARDED = 6,
  NOFL_METADATA_BYTE_UNUSED = 7,
  NOFL_METADATA_BYTE_MARK_MASK = 7,
  NOFL_METADATA_BYTE_TRACE_PRECISELY = 0,
  NOFL_METADATA_BYTE_TRACE_NONE = 8,
  NOFL_METADATA_BYTE_TRACE_CONSERVATIVELY = 16,
  NOFL_METADATA_BYTE_TRACE_UNUSED = 24,
  NOFL_METADATA_BYTE_TRACE_KIND_MASK = 0|8|16|24,
  NOFL_METADATA_BYTE_PINNED = 16,
  NOFL_METADATA_BYTE_END = 32,
  NOFL_METADATA_BYTE_LOGGED_0 = 64,
  NOFL_METADATA_BYTE_LOGGED_1 = 128,
};

STATIC_ASSERT_EQ(0,
                 NOFL_METADATA_BYTE_TRACE_PRECISELY&NOFL_METADATA_BYTE_PINNED);
STATIC_ASSERT_EQ(0,
                 NOFL_METADATA_BYTE_TRACE_NONE&NOFL_METADATA_BYTE_PINNED);

static uint8_t
nofl_advance_current_mark(uint8_t mark) {
  switch (mark) {
    case NOFL_METADATA_BYTE_MARK_0:
      return NOFL_METADATA_BYTE_MARK_1;
    case NOFL_METADATA_BYTE_MARK_1:
      return NOFL_METADATA_BYTE_MARK_2;
    case NOFL_METADATA_BYTE_MARK_2:
      return NOFL_METADATA_BYTE_MARK_0;
    default:
      GC_CRASH();
  }
}

static struct gc_lock
nofl_space_lock(struct nofl_space *space) {
  return gc_lock_acquire(&space->lock);
}

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
  uint8_t *ret = nofl_metadata_byte_for_addr(gc_ref_value(ref));
  GC_ASSERT(*ret & NOFL_METADATA_BYTE_MARK_MASK);
  return ret;
}

static uint8_t*
nofl_block_mark_loc(uintptr_t addr) {
  uintptr_t base = align_down(addr, NOFL_SLAB_SIZE);
  struct nofl_slab *slab = (struct nofl_slab *) base;
  unsigned block_idx = (addr / NOFL_BLOCK_SIZE) % NOFL_BLOCKS_PER_SLAB;
  return &slab->header.block_marks[block_idx];
}

static int
nofl_block_is_marked(uintptr_t addr) {
  return atomic_load_explicit(nofl_block_mark_loc(addr), memory_order_relaxed);
}

static void
nofl_block_set_mark(uintptr_t addr) {
  uint8_t *loc = nofl_block_mark_loc(addr);
  if (!atomic_load_explicit(loc, memory_order_relaxed))
    atomic_store_explicit(loc, 1, memory_order_relaxed);
}

#define NOFL_GRANULES_PER_BLOCK (NOFL_BLOCK_SIZE / NOFL_GRANULE_SIZE)

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
  return (summary->next_and_flags & flag) == flag;
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

static struct nofl_block_ref
nofl_block_for_addr(uintptr_t addr) {
  return (struct nofl_block_ref) {
    nofl_block_summary_for_addr(addr),
    align_down(addr, NOFL_BLOCK_SIZE)
  };
}

static struct nofl_block_ref
nofl_block_null(void) {
  return (struct nofl_block_ref) { NULL, 0 };
}

static int
nofl_block_is_null(struct nofl_block_ref block) {
  return block.summary == NULL;
}

static uintptr_t
nofl_block_has_flag(struct nofl_block_ref block, uintptr_t flags) {
  GC_ASSERT(!nofl_block_is_null(block));
  return nofl_block_summary_has_flag(block.summary, flags);
}

static void
nofl_block_set_flag(struct nofl_block_ref block, uintptr_t flags) {
  GC_ASSERT(!nofl_block_is_null(block));
  nofl_block_summary_set_flag(block.summary, flags);
}

static void
nofl_block_clear_flag(struct nofl_block_ref block, uintptr_t flags) {
  GC_ASSERT(!nofl_block_is_null(block));
  nofl_block_summary_clear_flag(block.summary, flags);
}

static struct nofl_block_ref
nofl_block_next(struct nofl_block_ref block) {
  GC_ASSERT(!nofl_block_is_null(block));
  return nofl_block_for_addr(nofl_block_summary_next(block.summary));
}

static void
nofl_block_set_next(struct nofl_block_ref head, struct nofl_block_ref tail) {
  GC_ASSERT(!nofl_block_is_null(head));
  nofl_block_summary_set_next(head.summary, tail.addr);
}

static int
nofl_allocator_has_block(struct nofl_allocator *alloc) {
  return !nofl_block_is_null(alloc->block);
}

static struct nofl_block_ref
nofl_block_head(struct nofl_block_list *list) {
  uintptr_t head = atomic_load_explicit(&list->blocks, memory_order_acquire);
  if (!head)
    return nofl_block_null();
  return (struct nofl_block_ref){ nofl_block_summary_for_addr(head), head };
}

static int
nofl_block_compare_and_exchange(struct nofl_block_list *list,
                                struct nofl_block_ref *expected,
                                struct nofl_block_ref desired) {
  if (atomic_compare_exchange_weak_explicit(&list->blocks,
                                            &expected->addr,
                                            desired.addr,
                                            memory_order_acq_rel,
                                            memory_order_acquire))
    return 1;
      
  expected->summary = nofl_block_summary_for_addr(expected->addr);
  return 0;
}

static void
nofl_block_list_push(struct nofl_block_list *list,
                     struct nofl_block_ref block) {
  atomic_fetch_add_explicit(&list->count, 1, memory_order_acq_rel);
  GC_ASSERT(nofl_block_is_null(nofl_block_next(block)));
  struct nofl_block_ref next = nofl_block_head(list);
  do {
    nofl_block_set_next(block, next);
  } while (!nofl_block_compare_and_exchange(list, &next, block));
}

static struct nofl_block_ref
nofl_block_list_pop(struct nofl_block_list *list) {
  struct nofl_block_ref head = nofl_block_head(list);
  struct nofl_block_ref next;
  do {
    if (nofl_block_is_null(head))
      return nofl_block_null();
    next = nofl_block_next(head);
  } while (!nofl_block_compare_and_exchange(list, &head, next));
  nofl_block_set_next(head, nofl_block_null());
  atomic_fetch_sub_explicit(&list->count, 1, memory_order_acq_rel);
  return head;
}

static void
nofl_block_stack_push(struct nofl_block_stack *stack,
                      struct nofl_block_ref block,
                      const struct gc_lock *lock) {
  struct nofl_block_list *list = &stack->list;
  list->count++;
  GC_ASSERT(nofl_block_is_null(nofl_block_next(block)));
  struct nofl_block_ref next = nofl_block_head(list);
  nofl_block_set_next(block, next);
  list->blocks = block.addr;
}

static struct nofl_block_ref
nofl_block_stack_pop(struct nofl_block_stack *stack,
                     const struct gc_lock *lock) {
  struct nofl_block_list *list = &stack->list;
  struct nofl_block_ref head = nofl_block_head(list);
  if (!nofl_block_is_null(head)) {
    list->count--;
    list->blocks = nofl_block_next(head).addr;
    nofl_block_set_next(head, nofl_block_null());
  }
  return head;
}

static size_t
nofl_block_count(struct nofl_block_list *list) {
  return atomic_load_explicit(&list->count, memory_order_acquire);
}

static void
nofl_push_unavailable_block(struct nofl_space *space,
                            struct nofl_block_ref block,
                            const struct gc_lock *lock) {
  nofl_block_set_flag(block, NOFL_BLOCK_UNAVAILABLE);
  nofl_block_stack_push(nofl_block_has_flag(block, NOFL_BLOCK_PAGED_OUT)
                        ? &space->paged_out[NOFL_PAGE_OUT_QUEUE_SIZE-1]
                        : &space->paged_out[0],
                        block, lock);
}

static struct nofl_block_ref
nofl_pop_unavailable_block(struct nofl_space *space,
                           const struct gc_lock *lock) {
  for (int age = 0; age < NOFL_PAGE_OUT_QUEUE_SIZE; age++) {
    struct nofl_block_ref block =
      nofl_block_stack_pop(&space->paged_out[age], lock);
    if (!nofl_block_is_null(block)) {
      nofl_block_clear_flag(block, NOFL_BLOCK_UNAVAILABLE);
      return block;
    }
  }
  return nofl_block_null();
}

static size_t
nofl_empty_block_count(struct nofl_space *space) {
  struct gc_lock lock = nofl_space_lock(space);
  size_t ret = nofl_block_count(&space->empty.list);
  gc_lock_release(&lock);
  return ret;
}

static void
nofl_push_empty_block(struct nofl_space *space,
                      struct nofl_block_ref block,
                      const struct gc_lock *lock) {
  nofl_block_stack_push(&space->empty, block, lock);
}

static struct nofl_block_ref
nofl_pop_empty_block_with_lock(struct nofl_space *space,
                               const struct gc_lock *lock) {
  return nofl_block_stack_pop(&space->empty, lock);
}

static struct nofl_block_ref
nofl_pop_empty_block(struct nofl_space *space) {
  struct gc_lock lock = nofl_space_lock(space);
  struct nofl_block_ref ret = nofl_pop_empty_block_with_lock(space, &lock);
  gc_lock_release(&lock);
  return ret;
}

static size_t
nofl_active_block_count(struct nofl_space *space) {
  size_t total = space->nslabs * NOFL_NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = 0;
  for (int age = 0; age < NOFL_PAGE_OUT_QUEUE_SIZE; age++)
    unavailable += nofl_block_count(&space->paged_out[age].list);
  GC_ASSERT(unavailable <= total);
  return total - unavailable;
}

static size_t
nofl_evacuation_block_count(struct nofl_space *space) {
  return nofl_block_count(&space->evacuation_targets);
}

static int
nofl_maybe_push_evacuation_target(struct nofl_space *space,
                                  struct nofl_block_ref block,
                                  double reserve) {
  size_t targets = nofl_evacuation_block_count(space);
  size_t active = nofl_active_block_count(space);
  if (targets >= ceil(active * reserve))
    return 0;

  nofl_block_list_push(&space->evacuation_targets, block);
  return 1;
}

static int
nofl_push_evacuation_target_if_needed(struct nofl_space *space,
                                      struct nofl_block_ref block) {
  return nofl_maybe_push_evacuation_target(space, block,
                                           space->evacuation_minimum_reserve);
}

static int
nofl_push_evacuation_target_if_possible(struct nofl_space *space,
                                        struct nofl_block_ref block) {
  return nofl_maybe_push_evacuation_target(space, block,
                                           space->evacuation_reserve);
}

static inline void
nofl_clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static size_t
nofl_space_live_object_granules(uint8_t *metadata) {
  return scan_for_byte_with_bits(metadata, -1, NOFL_METADATA_BYTE_END) + 1;
}

static void
nofl_allocator_reset(struct nofl_allocator *alloc) {
  alloc->alloc = alloc->sweep = 0;
  alloc->block = nofl_block_null();
  nofl_holeset_clear(&alloc->holes);
}

static int
nofl_should_promote_block(struct nofl_space *space,
                          struct nofl_block_ref block) {
  // If this block has mostly survivors, we can promote it to the old
  // generation.  Old-generation blocks won't be used for allocation
  // until after the next full GC.
  if (!GC_GENERATIONAL) return 0;
  size_t threshold = NOFL_GRANULES_PER_BLOCK * space->promotion_threshold;
  return block.summary->hole_granules < threshold;
}

static void
nofl_allocator_release_full_block(struct nofl_allocator *alloc,
                                  struct nofl_space *space) {
  GC_ASSERT(nofl_allocator_has_block(alloc));
  struct nofl_block_ref block = alloc->block;
  GC_ASSERT(alloc->alloc == alloc->sweep);
  atomic_fetch_add(&space->allocated_granules_since_last_collection,
                   block.summary->hole_granules);
  atomic_fetch_add(&space->survivor_granules_at_last_collection,
                   NOFL_GRANULES_PER_BLOCK - block.summary->hole_granules);
  atomic_fetch_add(&space->fragmentation_granules_since_last_collection,
                   block.summary->fragmentation_granules);

  if (nofl_should_promote_block(space, block))
    nofl_block_list_push(&space->promoted, block);
  else
    nofl_block_list_push(&space->full, block);

  nofl_allocator_reset(alloc);
}

static void
nofl_allocator_release_full_evacuation_target(struct nofl_allocator *alloc,
                                              struct nofl_space *space) {
  GC_ASSERT(nofl_allocator_has_block(alloc));
  struct nofl_block_ref block = alloc->block;
  GC_ASSERT(alloc->alloc > block.addr);
  GC_ASSERT(alloc->sweep == block.addr + NOFL_BLOCK_SIZE);
  size_t hole_size = alloc->sweep - alloc->alloc;
  // FIXME: Check how this affects statistics.
  GC_ASSERT_EQ(block.summary->hole_count, 1);
  GC_ASSERT_EQ(block.summary->hole_granules, NOFL_GRANULES_PER_BLOCK);
  atomic_fetch_add(&space->old_generation_granules,
                   NOFL_GRANULES_PER_BLOCK);
  if (hole_size) {
    hole_size >>= NOFL_GRANULE_SIZE_LOG_2;
    block.summary->holes_with_fragmentation = 1;
    block.summary->fragmentation_granules = hole_size / NOFL_GRANULE_SIZE;
  } else {
    GC_ASSERT_EQ(block.summary->fragmentation_granules, 0);
    GC_ASSERT_EQ(block.summary->holes_with_fragmentation, 0);
  }
  nofl_block_list_push(&space->old, block);
  nofl_allocator_reset(alloc);
}

static void
nofl_allocator_release_partly_full_block(struct nofl_allocator *alloc,
                                         struct nofl_space *space) {
  // A block can go on the partly full list if it has exactly one
  // hole, located at the end of the block.
  GC_ASSERT(nofl_allocator_has_block(alloc));
  struct nofl_block_ref block = alloc->block;
  GC_ASSERT(alloc->alloc > block.addr);
  GC_ASSERT(alloc->sweep == block.addr + NOFL_BLOCK_SIZE);
  size_t hole_size = alloc->sweep - alloc->alloc;
  GC_ASSERT(hole_size);
  block.summary->fragmentation_granules = hole_size / NOFL_GRANULE_SIZE;
  struct gc_lock lock = nofl_space_lock(space);
  nofl_block_stack_push(&space->partly_full, block, &lock);
  gc_lock_release(&lock);
  nofl_allocator_reset(alloc);
}

static size_t
nofl_allocator_acquire_partly_full_block(struct nofl_allocator *alloc,
                                         struct nofl_space *space) {
  struct gc_lock lock = nofl_space_lock(space);
  struct nofl_block_ref block = nofl_block_stack_pop(&space->partly_full,
                                                     &lock);
  gc_lock_release(&lock);
  if (nofl_block_is_null(block))
    return 0;
  GC_ASSERT_EQ(block.summary->holes_with_fragmentation, 0);
  alloc->block = block;
  alloc->sweep = block.addr + NOFL_BLOCK_SIZE;
  size_t hole_granules = block.summary->fragmentation_granules;
  block.summary->fragmentation_granules = 0;
  alloc->alloc = alloc->sweep - (hole_granules << NOFL_GRANULE_SIZE_LOG_2);
  return hole_granules;
}

static size_t
nofl_allocator_acquire_empty_block(struct nofl_allocator *alloc,
                                   struct nofl_space *space) {
  struct nofl_block_ref block = nofl_pop_empty_block(space);
  if (nofl_block_is_null(block))
    return 0;
  block.summary->hole_count = 1;
  block.summary->hole_granules = NOFL_GRANULES_PER_BLOCK;
  block.summary->holes_with_fragmentation = 0;
  block.summary->fragmentation_granules = 0;
  alloc->block = block;
  alloc->alloc = block.addr;
  alloc->sweep = block.addr + NOFL_BLOCK_SIZE;
  if (nofl_block_has_flag(block, NOFL_BLOCK_ZERO))
    nofl_block_clear_flag(block, NOFL_BLOCK_ZERO | NOFL_BLOCK_PAGED_OUT);
  else
    nofl_clear_memory(block.addr, NOFL_BLOCK_SIZE);
  return NOFL_GRANULES_PER_BLOCK;
}

static size_t
nofl_allocator_acquire_evacuation_target(struct nofl_allocator* alloc,
                                         struct nofl_space *space) {
  size_t granules = nofl_allocator_acquire_partly_full_block(alloc, space);
  if (granules)
    return granules;
  return nofl_allocator_acquire_empty_block(alloc, space);
}

static void
nofl_allocator_finish_hole(struct nofl_allocator *alloc) {
  size_t granules = (alloc->sweep - alloc->alloc) / NOFL_GRANULE_SIZE;
  if (granules) {
    nofl_holeset_push_local(&alloc->holes, alloc->alloc, granules);
    alloc->block.summary->holes_with_fragmentation++;
    alloc->block.summary->fragmentation_granules += granules;
    alloc->alloc = alloc->sweep;
  }
}

static inline int
nofl_metadata_byte_has_mark(uint8_t byte, uint8_t marked) {
  return (byte & NOFL_METADATA_BYTE_MARK_MASK) == marked;
}

static inline int
nofl_metadata_byte_is_young_or_has_mark(uint8_t byte, uint8_t marked) {
  return (nofl_metadata_byte_has_mark(byte, NOFL_METADATA_BYTE_YOUNG)
          || nofl_metadata_byte_has_mark(byte, marked));
}

// Sweep some heap to reclaim free space, advancing alloc->alloc and
// alloc->sweep.  Return the size of the hole in granules, or 0 if we
// reached the end of the block.
static size_t
nofl_allocator_next_hole_in_block(struct nofl_allocator *alloc,
                                  uint8_t survivor_mark) {
  GC_ASSERT(nofl_allocator_has_block(alloc));
  GC_ASSERT_EQ(alloc->alloc, alloc->sweep);
  uintptr_t sweep = alloc->sweep;
  uintptr_t limit = alloc->block.addr + NOFL_BLOCK_SIZE;

  if (sweep == limit)
    return 0;

  GC_ASSERT((sweep & (NOFL_GRANULE_SIZE - 1)) == 0);
  uint8_t* metadata = nofl_metadata_byte_for_addr(sweep);
  size_t limit_granules = (limit - sweep) >> NOFL_GRANULE_SIZE_LOG_2;

  // Except for when we first get a block, alloc->sweep is positioned
  // right after a hole, which can point to either the end of the
  // block or to a live object.  Assume that a live object is more
  // common.
  while (limit_granules &&
         nofl_metadata_byte_has_mark(metadata[0], survivor_mark)) {
    // Object survived collection; skip over it and continue sweeping.
    size_t object_granules = nofl_space_live_object_granules(metadata);
    sweep += object_granules * NOFL_GRANULE_SIZE;
    limit_granules -= object_granules;
    metadata += object_granules;
  }
  if (!limit_granules) {
    GC_ASSERT_EQ(sweep, limit);
    alloc->alloc = alloc->sweep = limit;
    return 0;
  }

  size_t hole_granules = scan_for_byte_with_tag(metadata, limit_granules,
                                                NOFL_METADATA_BYTE_MARK_MASK,
                                                survivor_mark);
  size_t free_bytes = hole_granules * NOFL_GRANULE_SIZE;
  GC_ASSERT(hole_granules);
  GC_ASSERT(hole_granules <= limit_granules);

  memset(metadata, 0, hole_granules);
  memset((char*)sweep, 0, free_bytes);

  alloc->block.summary->hole_count++;
  GC_ASSERT(hole_granules <=
            NOFL_GRANULES_PER_BLOCK - alloc->block.summary->hole_granules);
  alloc->block.summary->hole_granules += hole_granules;

  alloc->alloc = sweep;
  alloc->sweep = sweep + free_bytes;
  return hole_granules;
}

static void
nofl_allocator_finish_sweeping_in_block(struct nofl_allocator *alloc,
                                        uint8_t survivor_mark) {
  do {
    nofl_allocator_finish_hole(alloc);
  } while (nofl_allocator_next_hole_in_block(alloc, survivor_mark));
}

static void
nofl_allocator_release_block(struct nofl_allocator *alloc,
                             struct nofl_space *space) {
  GC_ASSERT(nofl_allocator_has_block(alloc));
  if (alloc->alloc < alloc->sweep &&
      alloc->sweep == alloc->block.addr + NOFL_BLOCK_SIZE &&
      alloc->block.summary->holes_with_fragmentation == 0) {
    nofl_allocator_release_partly_full_block(alloc, space);
  } else if (space->evacuating) {
    nofl_allocator_release_full_evacuation_target(alloc, space);
  } else {
    nofl_allocator_finish_sweeping_in_block(alloc, space->survivor_mark);
    nofl_allocator_release_full_block(alloc, space);
  }
}

static void
nofl_allocator_finish(struct nofl_allocator *alloc, struct nofl_space *space) {
  if (nofl_allocator_has_block(alloc))
    nofl_allocator_release_block(alloc, space);
  nofl_holeset_release(&alloc->holes, &space->holes, &space->lock);
}

static int
nofl_allocator_acquire_block_to_sweep(struct nofl_allocator *alloc,
                                      struct nofl_space *space) {
  struct nofl_block_ref block = nofl_block_list_pop(&space->to_sweep);
  if (nofl_block_is_null(block))
    return 0;
  alloc->block = block;
  alloc->alloc = alloc->sweep = block.addr;
  return 1;
}

static size_t
nofl_allocator_next_hole_in_block_of_size(struct nofl_allocator *alloc,
                                          struct nofl_space *space,
                                          size_t min_granules) {
  if (!nofl_allocator_has_block(alloc))
    return 0;

  while (1) {
    nofl_allocator_finish_hole(alloc);
    size_t granules =
      nofl_allocator_next_hole_in_block(alloc, space->survivor_mark);
    if (granules == 0) {
      nofl_allocator_release_full_block(alloc, space);
      return 0;
    }
    else if (min_granules <= granules)
      return granules;
  }
}

static size_t
nofl_blocks_to_sweep_for_size(size_t granules) {
  if (!granules)
    return -1;

  size_t max_granules = NOFL_GRANULES_PER_BLOCK;
  GC_ASSERT(granules <= max_granules);
  return __builtin_clzll(granules) - __builtin_clzll(max_granules);
}

static size_t
nofl_allocator_next_hole(struct nofl_allocator *alloc,
                         struct nofl_space *space,
                         size_t min_granules) {
  // Sweep current block for a hole.
  {
    size_t granules =
      nofl_allocator_next_hole_in_block_of_size(alloc, space, min_granules);
    if (granules)
      return granules;
  }

  // Current block
  size_t blocks_to_sweep = nofl_blocks_to_sweep_for_size(min_granules);
  while (1) {
    size_t blocks_swept = 0;
    for (;
         blocks_swept < blocks_to_sweep
           && nofl_allocator_acquire_block_to_sweep(alloc, space);
         blocks_swept++) {
      // This block was marked in the last GC and needs sweeping.
      // As we sweep we'll want to record how many bytes were live
      // at the last collection.  As we allocate we'll record how
      // many granules were wasted because of fragmentation.
      alloc->block.summary->hole_count = 0;
      alloc->block.summary->hole_granules = 0;
      alloc->block.summary->holes_with_fragmentation = 0;
      alloc->block.summary->fragmentation_granules = 0;
      size_t granules =
        nofl_allocator_next_hole_in_block_of_size(alloc, space, min_granules);
      if (granules)
        return granules;
    }

    while (1) {
      size_t granules = nofl_allocator_acquire_partly_full_block(alloc, space);
      if (!granules)
        break;
      if (min_granules <= granules)
        return granules;
      nofl_allocator_finish_hole(alloc);
      nofl_allocator_release_full_block(alloc, space);
    }

    // We are done sweeping for blocks.  Now take from the empties list.
    if (nofl_allocator_acquire_empty_block(alloc, space))
      return NOFL_GRANULES_PER_BLOCK;

    // Couldn't acquire another block; return 0 to cause collection.
    if (blocks_swept == 0)
      return 0;
  }
}

static inline struct gc_ref
nofl_allocate_bump_pointer(struct nofl_allocator *alloc, size_t size,
                           enum gc_allocation_kind kind) {
  GC_ASSERT(size <= alloc->sweep - alloc->alloc);
  struct gc_ref ret = gc_ref(alloc->alloc);
  alloc->alloc += size;
  gc_update_alloc_table(ret, size, kind);
  return ret;
}

static struct gc_ref
nofl_allocate_second_chance(struct nofl_allocator *alloc,
                            struct nofl_space *space, size_t granules) {
  struct gc_ref local = nofl_holeset_try_pop(&alloc->holes, granules);
  if (!gc_ref_is_null(local))
    return local;
  if (nofl_holeset_acquire(&alloc->holes, &space->holes, &space->lock,
                           granules))
    return nofl_holeset_try_pop(&alloc->holes, granules);
  return gc_ref_null();
}

static struct gc_ref
nofl_allocate_slow(struct nofl_allocator *alloc, struct nofl_space *space,
                   size_t size, enum gc_allocation_kind kind) {
  size_t granules = size >> NOFL_GRANULE_SIZE_LOG_2;

  if (nofl_allocator_next_hole_in_block_of_size(alloc, space, granules))
    return nofl_allocate_bump_pointer(alloc, size, kind);

  struct gc_ref second_chance =
    nofl_allocate_second_chance(alloc, space, granules);
  if (!gc_ref_is_null(second_chance)) {
    gc_update_alloc_table(second_chance, size, kind);
    return second_chance;
  }

  if (nofl_allocator_next_hole(alloc, space, granules))
    return nofl_allocate_bump_pointer(alloc, size, kind);

  return gc_ref_null();
}

static struct gc_ref
nofl_allocate(struct nofl_allocator *alloc, struct nofl_space *space,
              size_t size, enum gc_allocation_kind kind) {
  GC_ASSERT(size > 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());
  size = align_up(size, NOFL_GRANULE_SIZE);

  if (alloc->alloc + size <= alloc->sweep)
    return nofl_allocate_bump_pointer(alloc, size, kind);

  return nofl_allocate_slow(alloc, space, size, kind);
}

static struct gc_ref
nofl_evacuation_allocate(struct nofl_allocator* alloc, struct nofl_space *space,
                         size_t granules) {
  size_t avail = (alloc->sweep - alloc->alloc) >> NOFL_GRANULE_SIZE_LOG_2;
  while (avail < granules) {
    if (nofl_allocator_has_block(alloc))
      // No need to finish the hole, these mark bytes are zero.
      nofl_allocator_release_full_evacuation_target(alloc, space);
    avail = nofl_allocator_acquire_evacuation_target(alloc, space);
    if (!avail)
      return gc_ref_null();
  }

  struct gc_ref ret = gc_ref(alloc->alloc);
  alloc->alloc += granules * NOFL_GRANULE_SIZE;
  // Caller is responsible for updating alloc table.
  return ret;
}

// Another thread is triggering GC.  Before we stop, finish clearing the
// dead mark bytes for the mutator's block, and release the block.
static void
nofl_finish_sweeping(struct nofl_allocator *alloc,
                     struct nofl_space *space) {
  while (nofl_allocator_next_hole(alloc, space, 0)) {}
}

struct gc_trace_worker;

static inline int
nofl_space_heap_has_ambiguous_edges(struct nofl_space *space) {
  return atomic_load_explicit(&space->heap_has_ambiguous_edges,
                              memory_order_relaxed);
}

static void
nofl_clear_pinned_bits_in_block(struct nofl_block_ref block) {
  uint8_t *meta = nofl_metadata_byte_for_addr(block.addr);
  uint64_t mask = broadcast_byte (NOFL_METADATA_BYTE_PINNED);
  for (size_t i = 0; i < NOFL_GRANULES_PER_BLOCK; i += 8, meta += 8) {
    uint64_t vals = load_eight_aligned_bytes(meta);
    if (vals & mask)
      store_eight_aligned_bytes(meta, vals & ~mask);
  }
}

static void
nofl_space_clear_all_pinned_bits(struct nofl_space *space) GC_NEVER_INLINE;
static void
nofl_space_clear_all_pinned_bits(struct nofl_space *space) {
  // FIXME: This is racy in three ways:
  // (1) with regards to other threads acquiring sweepable blocks
  // (2) with regards to other threads allocating in their own blocks
  // (3) with regards to other threads pinning objects anywhere
  struct gc_lock lock = nofl_space_lock(space);

  for (struct nofl_block_ref walk = nofl_block_head(&space->to_sweep);
       !nofl_block_is_null(walk);
       walk = nofl_block_next(walk))
    nofl_clear_pinned_bits_in_block(walk);

  // not racy here though because of the lock
  for (struct nofl_block_ref walk = nofl_block_head(&space->partly_full.list);
       !nofl_block_is_null (walk);
       walk = nofl_block_next (walk))
    nofl_clear_pinned_bits_in_block(walk);

  for (struct nofl_block_ref walk = nofl_block_head(&space->full);
       !nofl_block_is_null(walk);
       walk = nofl_block_next(walk))
    nofl_clear_pinned_bits_in_block(walk);

  for (struct nofl_block_ref walk = nofl_block_head(&space->promoted);
       !nofl_block_is_null(walk);
       walk = nofl_block_next(walk))
    nofl_clear_pinned_bits_in_block(walk);

  for (struct nofl_block_ref walk = nofl_block_head(&space->old);
       !nofl_block_is_null(walk);
       walk = nofl_block_next(walk))
    nofl_clear_pinned_bits_in_block(walk);

  gc_lock_release(&lock);
}

static inline void
nofl_space_set_heap_has_ambiguous_edges (struct nofl_space *space)
{
  if (!nofl_space_heap_has_ambiguous_edges (space)) {
    fprintf (stderr,
             "warning: conservatively-traced allocation disables compaction\n");
    atomic_store_explicit (&space->heap_has_ambiguous_edges, 1,
                           memory_order_relaxed);

    // FIXME: We need to repurpose the pinned bit to indicate objects that
    // should be traced conservatively, but we don't want to reinterpret
    // previously pinned but precisely-traced objects as being
    // conservatively-traced.  Ideally we would have another bit here.  For now,
    // race to clear all pinned bits.
    nofl_space_clear_all_pinned_bits (space);
    space->evacuation_minimum_reserve = space->evacuation_reserve = 0.0;
  }
}

static inline int
nofl_space_contains_address(struct nofl_space *space, uintptr_t addr) {
  return extents_contain_addr(space->extents, addr);
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

static inline int
nofl_space_contains_edge(struct nofl_space *space, struct gc_edge edge) {
  return nofl_space_contains_address(space, gc_edge_address(edge));
}  

static inline int
nofl_space_is_survivor(struct nofl_space *space, struct gc_ref ref) {
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_relaxed);
  return nofl_metadata_byte_has_mark(byte, space->survivor_mark);
}

static uint8_t*
nofl_field_logged_byte(struct gc_edge edge) {
  return nofl_metadata_byte_for_addr(gc_edge_address(edge));
}

static uint8_t
nofl_field_logged_bit(struct gc_edge edge) {
  GC_ASSERT_EQ(sizeof(uintptr_t) * 2, NOFL_GRANULE_SIZE);
  size_t field = gc_edge_address(edge) / sizeof(uintptr_t);
  return NOFL_METADATA_BYTE_LOGGED_0 << (field % 2);
}

static int
nofl_space_remember_edge(struct nofl_space *space, struct gc_ref obj,
                         struct gc_edge edge) {
  GC_ASSERT(nofl_space_contains(space, obj));
  if (!GC_GENERATIONAL) return 0;
  if (!nofl_space_is_survivor(space, obj))
    return 0;
  uint8_t* loc = nofl_field_logged_byte(edge);
  uint8_t bit = nofl_field_logged_bit(edge);
  uint8_t byte = atomic_load_explicit(loc, memory_order_acquire);
  do {
    if (byte & bit) return 0;
  } while (!atomic_compare_exchange_weak_explicit(loc, &byte, byte|bit,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
  return 1;
}

static void
nofl_space_forget_edge(struct nofl_space *space, struct gc_edge edge) {
  GC_ASSERT(nofl_space_contains_edge(space, edge));
  GC_ASSERT(GC_GENERATIONAL);
  uint8_t* loc = nofl_field_logged_byte(edge);
  if (GC_DEBUG) {
    pthread_mutex_lock(&space->lock);
    uint8_t bit = nofl_field_logged_bit(edge);
    GC_ASSERT(*loc & bit);
    *loc &= ~bit;
    pthread_mutex_unlock(&space->lock);
  } else {
    // In release mode, race to clear both bits at once.
    uint8_t byte = atomic_load_explicit(loc, memory_order_relaxed);
    byte &= ~(NOFL_METADATA_BYTE_LOGGED_0 | NOFL_METADATA_BYTE_LOGGED_1);
    atomic_store_explicit(loc, byte, memory_order_relaxed);
  }
}

static void
nofl_space_reset_statistics(struct nofl_space *space) {
  space->survivor_granules_at_last_collection = 0;
  space->allocated_granules_since_last_collection = 0;
  space->fragmentation_granules_since_last_collection = 0;
}

static size_t
nofl_space_live_size_at_last_collection(struct nofl_space *space) {
  size_t granules = space->old_generation_granules
    + space->survivor_granules_at_last_collection;
  return granules * NOFL_GRANULE_SIZE;
}

static void
nofl_space_add_to_allocation_counter(struct nofl_space *space,
                                     uint64_t *counter) {
  *counter +=
    atomic_load_explicit(&space->allocated_granules_since_last_collection,
                         memory_order_relaxed) * NOFL_GRANULE_SIZE;
}
  
static size_t
nofl_space_estimate_live_bytes_after_gc(struct nofl_space *space,
                                        double last_yield)
{
  // The nofl space mostly traces via marking, and as such doesn't precisely
  // know the live data size until after sweeping.  But it is important to
  // promptly compute the live size so that we can grow the heap if
  // appropriate.  Therefore sometimes we will estimate the live data size
  // instead of measuring it precisely.
  size_t bytes = 0;
  bytes += nofl_block_count(&space->full) * NOFL_BLOCK_SIZE;
  bytes += nofl_block_count(&space->partly_full.list) * NOFL_BLOCK_SIZE / 2;
  GC_ASSERT_EQ(nofl_block_count(&space->promoted), 0);
  bytes += space->old_generation_granules * NOFL_GRANULE_SIZE;
  bytes +=
    nofl_block_count(&space->to_sweep) * NOFL_BLOCK_SIZE * (1 - last_yield);

  DEBUG("--- nofl estimate before adjustment: %zu\n", bytes);

  // Assume that if we have pending unavailable bytes after GC that there is a
  // large object waiting to be allocated, so it is practically in the live set.
  ssize_t pending = atomic_load_explicit(&space->pending_unavailable_bytes,
                                         memory_order_acquire);
  if (pending > 0)
    bytes += pending;
  DEBUG("--- nofl estimate after adjustment: %zu\n", bytes);

  return bytes;
}

static size_t
nofl_space_evacuation_reserve_bytes(struct nofl_space *space) {
  return nofl_evacuation_block_count(space) * NOFL_BLOCK_SIZE;
}

static size_t
nofl_space_evacuation_minimum_reserve_bytes(struct nofl_space *space) {
  return
    ceil(space->evacuation_minimum_reserve * nofl_active_block_count(space))
    * NOFL_BLOCK_SIZE;
}

static size_t
nofl_space_fragmentation(struct nofl_space *space) {
  size_t young = space->fragmentation_granules_since_last_collection;
  GC_ASSERT(nofl_block_count(&space->old) * NOFL_GRANULES_PER_BLOCK >=
            space->old_generation_granules);
  size_t old = nofl_block_count(&space->old) * NOFL_GRANULES_PER_BLOCK -
    space->old_generation_granules;
  return (young + old) * NOFL_GRANULE_SIZE;
}

static void
nofl_space_prepare_evacuation(struct nofl_space *space) {
  GC_ASSERT(!space->evacuating);
  struct nofl_block_ref block;
  struct gc_lock lock = nofl_space_lock(space);
  while (!nofl_block_is_null
         (block = nofl_block_list_pop(&space->evacuation_targets)))
    nofl_push_empty_block(space, block, &lock);
  gc_lock_release(&lock);
  // Blocks are either to_sweep, empty, or unavailable.
  GC_ASSERT_EQ(nofl_block_count(&space->partly_full.list), 0);
  GC_ASSERT_EQ(nofl_block_count(&space->full), 0);
  GC_ASSERT_EQ(nofl_block_count(&space->promoted), 0);
  GC_ASSERT_EQ(nofl_block_count(&space->old), 0);
  GC_ASSERT_EQ(nofl_block_count(&space->evacuation_targets), 0);
  size_t target_blocks = nofl_block_count(&space->empty.list);
  DEBUG("evacuation target block count: %zu\n", target_blocks);

  if (target_blocks == 0) {
    DEBUG("no evacuation target blocks, not evacuating this round\n");
    return;
  }

  // Put the mutator into evacuation mode, collecting up to 50% of free
  // space as evacuation blocks.
  space->evacuation_reserve = 0.5;
  space->evacuating = 1;

  size_t target_granules = target_blocks * NOFL_GRANULES_PER_BLOCK;
  // Compute histogram where domain is the number of granules in a block
  // that survived the last collection, aggregated into 33 buckets, and
  // range is number of blocks in that bucket.  (Bucket 0 is for blocks
  // that were found to be completely empty; such blocks may be on the
  // evacuation target list.)
  const size_t bucket_count = 33;
  size_t histogram[33] = {0,};
  size_t bucket_size = NOFL_GRANULES_PER_BLOCK / 32;
  for (struct nofl_block_ref b = nofl_block_for_addr(space->to_sweep.blocks);
       !nofl_block_is_null(b);
       b = nofl_block_next(b)) {
    size_t survivor_granules = NOFL_GRANULES_PER_BLOCK - b.summary->hole_granules;
    size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
    histogram[bucket]++;
  }

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
  // candidate flag on all blocks that have live objects.
  for (struct nofl_block_ref b = nofl_block_for_addr(space->to_sweep.blocks);
       !nofl_block_is_null(b);
       b = nofl_block_next(b)) {
    size_t survivor_granules = NOFL_GRANULES_PER_BLOCK - b.summary->hole_granules;
    size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
    if (histogram[bucket]) {
      nofl_block_set_flag(b, NOFL_BLOCK_EVACUATE);
      histogram[bucket]--;
    } else {
      nofl_block_clear_flag(b, NOFL_BLOCK_EVACUATE);
    }
  }
}

static void
nofl_space_clear_block_marks(struct nofl_space *space) {
  for (size_t s = 0; s < space->nslabs; s++) {
    struct nofl_slab *slab = space->slabs[s];
    memset(slab->header.block_marks, 0, sizeof(slab->header.block_marks));
  }
}

static void
nofl_space_prepare_gc(struct nofl_space *space, enum gc_collection_kind kind) {
  int is_minor = kind == GC_COLLECTION_MINOR;
  if (!is_minor) {
    space->current_mark = nofl_advance_current_mark(space->current_mark);
    nofl_space_clear_block_marks(space);
  }
}

static void
nofl_space_start_gc(struct nofl_space *space, enum gc_collection_kind gc_kind) {
  GC_ASSERT_EQ(nofl_block_count(&space->to_sweep), 0);

  // Any block that was the target of allocation in the last cycle will need to
  // be swept next cycle.
  struct nofl_block_ref block;
  while (!nofl_block_is_null
         (block = nofl_block_list_pop(&space->partly_full.list)))
    nofl_block_list_push(&space->to_sweep, block);
  while (!nofl_block_is_null(block = nofl_block_list_pop(&space->full)))
    nofl_block_list_push(&space->to_sweep, block);

  if (gc_kind != GC_COLLECTION_MINOR) {
    while (!nofl_block_is_null(block = nofl_block_list_pop(&space->promoted)))
      nofl_block_list_push(&space->to_sweep, block);
    while (!nofl_block_is_null(block = nofl_block_list_pop(&space->old)))
      nofl_block_list_push(&space->to_sweep, block);
    space->old_generation_granules = 0;
  }

  if (gc_kind == GC_COLLECTION_COMPACTING)
    nofl_space_prepare_evacuation(space);
}

static void
nofl_space_finish_evacuation(struct nofl_space *space,
                             const struct gc_lock *lock) {
  // When evacuation began, the evacuation reserve was moved to the
  // empties list.  Now that evacuation is finished, attempt to
  // repopulate the reserve.
  GC_ASSERT(space->evacuating);
  space->evacuating = 0;
  size_t active = nofl_active_block_count(space);
  size_t reserve = ceil(space->evacuation_minimum_reserve * active);
  GC_ASSERT(nofl_evacuation_block_count(space) == 0);
  while (reserve--) {
    struct nofl_block_ref block = nofl_pop_empty_block_with_lock(space, lock);
    if (nofl_block_is_null(block)) break;
    nofl_block_list_push(&space->evacuation_targets, block);
  }
}

static void
nofl_space_promote_blocks(struct nofl_space *space) {
  struct nofl_block_ref block;
  while (!nofl_block_is_null(block = nofl_block_list_pop(&space->promoted))) {
    block.summary->hole_count = 0;
    block.summary->hole_granules = 0;
    block.summary->holes_with_fragmentation = 0;
    block.summary->fragmentation_granules = 0;
    struct nofl_allocator alloc = { block.addr, block.addr, block };
    nofl_allocator_finish_sweeping_in_block(&alloc, space->current_mark);
    atomic_fetch_add(&space->old_generation_granules,
                     NOFL_GRANULES_PER_BLOCK - block.summary->hole_granules);
    nofl_block_list_push(&space->old, block);
  }
}

static inline size_t
nofl_size_to_granules(size_t size) {
  return (size + NOFL_GRANULE_SIZE - 1) >> NOFL_GRANULE_SIZE_LOG_2;
}

static inline enum gc_trace_kind
nofl_metadata_byte_trace_kind(struct nofl_space *space, uint8_t byte)
{
  uint8_t mask = NOFL_METADATA_BYTE_TRACE_KIND_MASK;
  if (!nofl_space_heap_has_ambiguous_edges (space))
    mask &= ~NOFL_METADATA_BYTE_TRACE_CONSERVATIVELY;

  switch (byte & mask) {
  case NOFL_METADATA_BYTE_TRACE_PRECISELY:
    return GC_TRACE_PRECISELY;
  case NOFL_METADATA_BYTE_TRACE_CONSERVATIVELY:
    return GC_TRACE_CONSERVATIVELY;
  case NOFL_METADATA_BYTE_TRACE_NONE:
    return GC_TRACE_NONE;
  default:
    /* Untagged pointerless objects are allocated with the PINNED bit,
       because we can't relocate them, because we don't have a tag word
       to hold the forwarding state.  Fortunately, this bit pattern is
       different from NOFL_METADATA_BYTE_TRACE_CONSERVATIVELY; we can
       just leave it as-is.  */
    GC_ASSERT_EQ (byte & mask,
                  NOFL_METADATA_BYTE_TRACE_NONE | NOFL_METADATA_BYTE_PINNED);
    return GC_TRACE_NONE;
  }
}

static void
nofl_assert_not_forwarded(struct gc_ref ref)
{
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_relaxed);
  GC_ASSERT(!nofl_metadata_byte_has_mark(byte, NOFL_METADATA_BYTE_FORWARDED));
}

static void
nofl_space_verify_sweepable_blocks(struct nofl_space *space,
                                   struct nofl_block_list *list)
{
  for (struct nofl_block_ref b = nofl_block_for_addr(list->blocks);
       !nofl_block_is_null(b);
       b = nofl_block_next(b)) {
    // Iterate objects in the block, verifying that the END bytes correspond to
    // the measured object size.
    uintptr_t addr = b.addr;
    uintptr_t limit = addr + NOFL_BLOCK_SIZE;
    uint8_t *meta = nofl_metadata_byte_for_addr(b.addr);
    while (addr < limit) {
      uint8_t byte = meta[0];
      if (nofl_metadata_byte_has_mark(byte, space->current_mark)) {
        struct gc_ref obj = gc_ref(addr);
        size_t granules = 1;
        while (addr + granules * NOFL_GRANULE_SIZE < limit
               && !(meta[granules - 1] & NOFL_METADATA_BYTE_END))
          granules++;
        GC_ASSERT(meta[granules - 1] & NOFL_METADATA_BYTE_END);

        if (nofl_metadata_byte_trace_kind(space, byte) == GC_TRACE_PRECISELY) {
          size_t trace_bytes;
          gc_trace_object(obj, NULL, NULL, NULL, &trace_bytes);
          size_t trace_granules = nofl_size_to_granules(trace_bytes);
          GC_ASSERT_EQ(granules, trace_granules);

          nofl_assert_not_forwarded(obj);
        }

        meta += granules;
        addr += granules * NOFL_GRANULE_SIZE;
      } else if (byte & NOFL_METADATA_BYTE_MARK_MASK) {
        size_t granules = 1;
        while (addr + granules * NOFL_GRANULE_SIZE < limit
               && !(meta[granules - 1] & NOFL_METADATA_BYTE_END)) {
          granules++;
          GC_ASSERT_EQ(meta[granules - 1] & NOFL_METADATA_BYTE_MARK_MASK, 0);
        }
        GC_ASSERT(meta[granules - 1] & NOFL_METADATA_BYTE_END);
        meta += granules;
        addr += granules * NOFL_GRANULE_SIZE;
      } else {
        GC_ASSERT_EQ(byte & NOFL_METADATA_BYTE_END, 0);
        meta++;
        addr += NOFL_GRANULE_SIZE;
      }
    }
    GC_ASSERT(addr == limit);
  }
}

static void
nofl_space_verify_swept_blocks(struct nofl_space *space,
                               struct nofl_block_list *list) {
  for (struct nofl_block_ref b = nofl_block_for_addr(list->blocks);
       !nofl_block_is_null(b);
       b = nofl_block_next(b)) {
    // Iterate objects in the block, verifying that the END bytes correspond to
    // the measured object size.
    uintptr_t addr = b.addr;
    uintptr_t limit = addr + NOFL_BLOCK_SIZE;
    uint8_t *meta = nofl_metadata_byte_for_addr(addr);
    while (addr < limit) {
      uint8_t byte = meta[0];
      if (byte) {
        GC_ASSERT(nofl_metadata_byte_has_mark(byte, space->current_mark));
        struct gc_ref obj = gc_ref(addr);
        size_t granules = 1;
        while (addr + granules * NOFL_GRANULE_SIZE < limit
               && !(meta[granules - 1] & NOFL_METADATA_BYTE_END))
          granules++;
        GC_ASSERT(meta[granules - 1] & NOFL_METADATA_BYTE_END);

        if (nofl_metadata_byte_trace_kind(space, byte) == GC_TRACE_PRECISELY) {
          size_t trace_bytes;
          gc_trace_object(obj, NULL, NULL, NULL, &trace_bytes);
          size_t trace_granules = nofl_size_to_granules(trace_bytes);
          GC_ASSERT_EQ(granules, trace_granules);

          nofl_assert_not_forwarded(obj);
        }

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

static void
nofl_space_verify_empty_blocks(struct nofl_space *space,
                               struct nofl_block_list *list,
                               int paged_in) {
  for (struct nofl_block_ref b = nofl_block_for_addr(list->blocks);
       !nofl_block_is_null(b);
       b = nofl_block_next(b)) {
    // Iterate objects in the block, verifying that the END bytes correspond to
    // the measured object size.
    uintptr_t addr = b.addr;
    uintptr_t limit = addr + NOFL_BLOCK_SIZE;
    uint8_t *meta = nofl_metadata_byte_for_addr(addr);
    while (addr < limit) {
      GC_ASSERT_EQ(*meta, 0);
      if (paged_in && nofl_block_has_flag(b, NOFL_BLOCK_ZERO)) {
        char zeroes[NOFL_GRANULE_SIZE] = { 0, };
        GC_ASSERT_EQ(memcmp((char*)addr, zeroes, NOFL_GRANULE_SIZE), 0);
      }
      meta++;
      addr += NOFL_GRANULE_SIZE;
    }
    GC_ASSERT(addr == limit);
  }
}

static void
nofl_space_verify_before_restart(struct nofl_space *space) {
  nofl_space_verify_sweepable_blocks(space, &space->to_sweep);
  nofl_space_verify_sweepable_blocks(space, &space->promoted);
  // If there are full or partly full blocks, they were filled during
  // evacuation.
  nofl_space_verify_swept_blocks(space, &space->partly_full.list);
  nofl_space_verify_swept_blocks(space, &space->full);
  nofl_space_verify_swept_blocks(space, &space->old);
  nofl_space_verify_empty_blocks(space, &space->empty.list, 1);
  nofl_space_verify_empty_blocks(space, &space->evacuation_targets, 1);
  for (int age = 0; age < NOFL_PAGE_OUT_QUEUE_SIZE; age++)
    nofl_space_verify_empty_blocks(space, &space->paged_out[age].list, 0);
  // GC_ASSERT(space->last_collection_was_minor || !nofl_block_count(&space->old));
}

static void
nofl_space_finish_gc(struct nofl_space *space,
                     enum gc_collection_kind gc_kind) {
  nofl_holeset_clear(&space->holes);
  space->last_collection_was_minor = (gc_kind == GC_COLLECTION_MINOR);
  struct gc_lock lock = nofl_space_lock(space);
  if (space->evacuating)
    nofl_space_finish_evacuation(space, &lock);
  else {
    space->evacuation_reserve = space->evacuation_minimum_reserve;
    // If we were evacuating and preferentially allocated empty blocks
    // to the evacuation reserve, return those blocks to the empty set
    // for allocation by the mutator.
    size_t active = nofl_active_block_count(space);
    size_t target = ceil(space->evacuation_minimum_reserve * active);
    size_t reserve = nofl_evacuation_block_count(space);
    while (reserve-- > target)
      nofl_push_empty_block(space,
                            nofl_block_list_pop(&space->evacuation_targets),
                            &lock);
  }

  {
    struct nofl_block_list to_sweep = {0,};
    struct nofl_block_ref block;
    while (!nofl_block_is_null(block = nofl_block_list_pop(&space->to_sweep))) {
      if (nofl_block_is_marked(block.addr)) {
        nofl_block_list_push(&to_sweep, block);
      } else {
        // Block is empty.
        memset(nofl_metadata_byte_for_addr(block.addr), 0,
               NOFL_GRANULES_PER_BLOCK);
        if (!nofl_push_evacuation_target_if_possible(space, block))
          nofl_push_empty_block(space, block, &lock);
      }
    }
    atomic_store_explicit(&space->to_sweep.count, to_sweep.count,
                          memory_order_release);
    atomic_store_explicit(&space->to_sweep.blocks, to_sweep.blocks,
                          memory_order_release);
  }

  // FIXME: Promote concurrently instead of during the pause.
  gc_lock_release(&lock);
  nofl_space_promote_blocks(space);
  nofl_space_reset_statistics(space);
  space->survivor_mark = space->current_mark;
  if (GC_DEBUG)
    nofl_space_verify_before_restart(space);
}

static ssize_t
nofl_space_request_release_memory(struct nofl_space *space, size_t bytes) {
  return atomic_fetch_add(&space->pending_unavailable_bytes, bytes) + bytes;
}

static ssize_t
nofl_space_maybe_reacquire_memory(struct nofl_space *space, size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->pending_unavailable_bytes, bytes) - bytes;
  struct gc_lock lock = nofl_space_lock(space);
  while (pending + NOFL_BLOCK_SIZE <= 0) {
    struct nofl_block_ref block = nofl_pop_unavailable_block(space, &lock);
    if (nofl_block_is_null(block)) break;
    if (!nofl_push_evacuation_target_if_needed(space, block))
      nofl_push_empty_block(space, block, &lock);
    pending = atomic_fetch_add(&space->pending_unavailable_bytes, NOFL_BLOCK_SIZE)
      + NOFL_BLOCK_SIZE;
  }
  gc_lock_release(&lock);
  return pending;
}

static inline int
nofl_space_should_evacuate(struct nofl_space *space, uint8_t metadata_byte,
                           struct gc_ref obj) {
  if (nofl_space_heap_has_ambiguous_edges (space))
    return 0;
  if (!space->evacuating)
    return 0;
  if (metadata_byte & NOFL_METADATA_BYTE_PINNED)
    return 0;
  return nofl_block_has_flag(nofl_block_for_addr(gc_ref_value(obj)),
                             NOFL_BLOCK_EVACUATE);
}

static inline int
nofl_space_set_mark(struct nofl_space *space, uint8_t *metadata,
                    uint8_t byte) {
  uint8_t mask = NOFL_METADATA_BYTE_MARK_MASK;
  byte = (byte & ~mask) | space->current_mark;
  uint8_t prev = atomic_exchange_explicit(metadata, byte, memory_order_relaxed);
  return byte != prev;
}

static inline int
nofl_space_set_nonempty_mark(struct nofl_space *space, uint8_t *metadata,
                             uint8_t byte, struct gc_ref ref) {
  // FIXME: Check that relaxed atomics are actually worth it.
  if (nofl_space_set_mark(space, metadata, byte)) {
    GC_ASSERT(nofl_metadata_byte_is_young_or_has_mark(byte,
                                                      space->survivor_mark));
    nofl_block_set_mark(gc_ref_value(ref));
    return 1;
  }
  return 0;
}

static inline void
nofl_space_pin_object(struct nofl_space *space, struct gc_ref ref) {
  // For the heap-conservative configuration, all objects are pinned, and we use
  // the pinned bit instead to indicate conservatively-traced objects.
  if (nofl_space_heap_has_ambiguous_edges (space))
    return;
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_relaxed);
  if (byte & NOFL_METADATA_BYTE_PINNED)
    return;
  uint8_t new_byte;
  do {
    new_byte = byte | NOFL_METADATA_BYTE_PINNED;
  } while (!atomic_compare_exchange_weak_explicit(metadata, &byte, new_byte,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
}

static inline uint8_t
clear_logged_bits_in_evacuated_object(uint8_t head, uint8_t *metadata,
                                      size_t count) {
  // On a major collection, it could be that we evacuate an object that
  // has one or more fields in the old-to-new remembered set.  Because
  // the young generation is empty after a major collection, we know the
  // old-to-new remembered set will be empty also.  To clear the
  // remembered set, we call gc_field_set_clear, which will end up
  // visiting all remembered edges and clearing their logged bits.  But
  // that doesn't work for evacuated objects, because their edges move:
  // gc_field_set_clear will frob the pre-evacuation metadata bytes of
  // the object.  So here we explicitly clear logged bits for evacuated
  // objects.  That the bits for the pre-evacuation location are also
  // frobbed by gc_field_set_clear doesn't cause a problem, as that
  // memory will be swept and cleared later.
  //
  // This concern doesn't apply to minor collections: there we will
  // never evacuate an object in the remembered set, because old objects
  // aren't traced during a minor collection.
  uint8_t mask = NOFL_METADATA_BYTE_LOGGED_0 | NOFL_METADATA_BYTE_LOGGED_1;
  for (size_t i = 1; i < count; i++) {
    if (metadata[i] & mask)
      metadata[i] &= ~mask;
  }    
  return head & ~mask;
}


static inline int
nofl_space_evacuate(struct nofl_space *space, uint8_t *metadata, uint8_t byte,
                    struct gc_edge edge,
                    struct gc_ref old_ref,
                    struct nofl_allocator *evacuate) {
  uint8_t mark = byte & NOFL_METADATA_BYTE_MARK_MASK;
  uint8_t busy_byte = (byte - mark) | NOFL_METADATA_BYTE_BUSY;
  uint8_t forwarded_byte = (byte - mark) | NOFL_METADATA_BYTE_FORWARDED;
  uint8_t marked_byte = (byte - mark) | space->current_mark;

  int claimed =
    nofl_metadata_byte_is_young_or_has_mark(byte, space->survivor_mark)
    && atomic_compare_exchange_strong(metadata, &byte, busy_byte);

  if (claimed) {
    // It's up to us to shade the object.
    size_t object_granules = nofl_space_live_object_granules(metadata);
    struct gc_ref new_ref = nofl_evacuation_allocate(evacuate, space,
                                                     object_granules);
    if (gc_ref_is_null(new_ref)) {
      // Well shucks; allocation failed.  Mark in place and then release the
      // object.
      atomic_store_explicit(metadata, marked_byte, memory_order_release);
      nofl_block_set_mark(gc_ref_value(old_ref));
    } else {
      // Whee, it works!  Copy object contents before committing, as we don't
      // know what part of the object (if any) will be overwritten by the
      // commit.
      memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(old_ref),
             object_granules * NOFL_GRANULE_SIZE);
      gc_object_forward_nonatomic(old_ref, new_ref);
      atomic_store_explicit(metadata, forwarded_byte, memory_order_release);

      // Now update extent metadata, and indicate to the caller that
      // the object's fields need to be traced.
      uint8_t *new_metadata = nofl_metadata_byte_for_addr(gc_ref_value(new_ref));
      memcpy(new_metadata + 1, metadata + 1, object_granules - 1);
      byte = marked_byte;
      if (GC_GENERATIONAL)
        byte = clear_logged_bits_in_evacuated_object(byte, new_metadata,
                                                     object_granules);
      *new_metadata = byte;
      nofl_block_set_mark(gc_ref_value(new_ref));
      gc_edge_update(edge, new_ref);
    }
    // Either way, since we claimed the object, we shaded it grey.
    return 1;
  }

  // If we failed to claim the object, someone else shaded it grey (or
  // is in the process of doing so).  Wait for that to complete, if
  // needed, then update our edge if the object was forwarded.
  for (size_t spin_count = 0; byte == busy_byte; spin_count++) {
    yield_for_spin(spin_count);
    byte = atomic_load_explicit(metadata, memory_order_acquire);
  }

  if (byte == forwarded_byte) {
    struct gc_ref new_ref = gc_ref(gc_object_forwarded_nonatomic(old_ref));
    GC_ASSERT(!gc_ref_is_null(new_ref));
    gc_edge_update(edge, new_ref);
  }

  return 0;
}

static inline int
nofl_space_evacuate_or_mark_object(struct nofl_space *space,
                                   struct gc_edge edge,
                                   struct gc_ref old_ref,
                                   struct nofl_allocator *evacuate) {
  uint8_t *metadata = nofl_metadata_byte_for_object(old_ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_acquire);
  if (nofl_metadata_byte_has_mark(byte, space->current_mark))
    return 0;

  if (nofl_space_should_evacuate(space, byte, old_ref))
    return nofl_space_evacuate(space, metadata, byte, edge, old_ref,
                               evacuate);

  return nofl_space_set_nonempty_mark(space, metadata, byte, old_ref);
}

static inline int
nofl_space_mark_object(struct nofl_space *space, struct gc_ref ref,
                       struct nofl_allocator *evacuate) {
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_acquire);
  if (nofl_metadata_byte_has_mark(byte, space->current_mark))
    return 0;

  GC_ASSERT(!nofl_space_should_evacuate(space, byte, ref));

  return nofl_space_set_nonempty_mark(space, metadata, byte, ref);
}

static int
nofl_space_forward_or_mark_if_traced(struct nofl_space *space,
                                     struct gc_edge edge,
                                     struct gc_ref ref) {
  uint8_t *metadata = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(metadata, memory_order_acquire);
  uint8_t mark = byte & NOFL_METADATA_BYTE_MARK_MASK;
  uint8_t busy_byte = (byte - mark) | NOFL_METADATA_BYTE_BUSY;
  uint8_t forwarded_byte = (byte - mark) | NOFL_METADATA_BYTE_FORWARDED;

  for (size_t spin_count = 0; byte == busy_byte; spin_count++) {
    yield_for_spin(spin_count);
    byte = atomic_load_explicit(metadata, memory_order_acquire);
  }

  if (byte == forwarded_byte) {
    gc_edge_update(edge, gc_ref(gc_object_forwarded_nonatomic(ref)));
    return 1;
  }

  return nofl_metadata_byte_has_mark(byte, space->current_mark);
}

struct nofl_resolved_conservative_ref {
  uintptr_t addr;
  uint8_t *metadata;
  uint8_t byte;
};

static inline struct nofl_resolved_conservative_ref
nofl_space_resolve_conservative_ref_with_metadata(struct nofl_space *space,
                                                  struct gc_conservative_ref ref,
                                                  int possibly_interior) {
  uintptr_t addr = gc_conservative_ref_value(ref);
  struct nofl_resolved_conservative_ref not_an_object = { 0, };

  if (possibly_interior) {
    addr = align_down(addr, NOFL_GRANULE_SIZE);
  } else {
    // Addr not an aligned granule?  Not an object.
    uintptr_t displacement = addr & (NOFL_GRANULE_SIZE - 1);
    if (!gc_is_valid_conservative_ref_displacement(displacement))
      return not_an_object;
    addr -= displacement;
  }

  // Addr in meta block?  Not an object.
  if ((addr & (NOFL_SLAB_SIZE - 1)) < NOFL_META_BLOCKS_PER_SLAB * NOFL_BLOCK_SIZE)
    return not_an_object;

  // Addr in block that has been paged out?  Not an object.
  if (nofl_block_has_flag(nofl_block_for_addr(addr), NOFL_BLOCK_UNAVAILABLE))
    return not_an_object;

  uint8_t *loc = nofl_metadata_byte_for_addr(addr);
  uint8_t byte = atomic_load_explicit(loc, memory_order_relaxed);

  // Not pointing to the start of an object?  Scan backwards if the ref
  // is possibly interior, otherwise bail.
  if ((byte & NOFL_METADATA_BYTE_MARK_MASK) == 0) {
    if (!possibly_interior)
      return not_an_object;

    uintptr_t block_base = align_down(addr, NOFL_BLOCK_SIZE);
    uint8_t *loc_base = nofl_metadata_byte_for_addr(block_base);
    uint8_t mask = NOFL_METADATA_BYTE_MARK_MASK | NOFL_METADATA_BYTE_END;
    loc = scan_backwards_for_byte_with_bits(loc, loc_base, mask);

    if (!loc)
      return not_an_object;

    byte = atomic_load_explicit(loc, memory_order_relaxed);
    GC_ASSERT(byte & mask);
    // Ran into the end of some other allocation?  Not an object, then.
    if (byte & NOFL_METADATA_BYTE_END)
      return not_an_object;
    // Found object start, and object is unmarked; adjust addr.
    addr = block_base + (loc - loc_base) * NOFL_GRANULE_SIZE;
  }

  return (struct nofl_resolved_conservative_ref) {addr, loc, byte};
}

static inline struct gc_ref
nofl_space_resolve_conservative_ref(struct nofl_space *space,
                                    struct gc_conservative_ref ref,
                                    int possibly_interior) {
  struct nofl_resolved_conservative_ref resolved =
    nofl_space_resolve_conservative_ref_with_metadata(space, ref,
                                                      possibly_interior);

  // Possibly null.
  return gc_ref(resolved.addr);
}

static inline struct gc_ref
nofl_space_mark_conservative_ref(struct nofl_space *space,
                                 struct gc_conservative_ref ref,
                                 int possibly_interior) {
  struct nofl_resolved_conservative_ref resolved =
    nofl_space_resolve_conservative_ref_with_metadata(space, ref,
                                                      possibly_interior);

  if (!resolved.addr)
    return gc_ref_null();

  // Object already marked?  Nothing to do.
  if (nofl_metadata_byte_has_mark(resolved.byte, space->current_mark))
    return gc_ref_null();

  GC_ASSERT(nofl_metadata_byte_is_young_or_has_mark(resolved.byte,
                                                    space->survivor_mark));

  if (nofl_space_set_nonempty_mark(space, resolved.metadata, resolved.byte,
                                   gc_ref(resolved.addr)))
    return gc_ref(resolved.addr);

  return gc_ref_null();
}

static inline size_t
nofl_space_object_size(struct nofl_space *space, struct gc_ref ref) {
  uint8_t *loc = nofl_metadata_byte_for_object(ref);
  size_t granules = nofl_space_live_object_granules(loc);
  return granules * NOFL_GRANULE_SIZE;
}

static inline struct gc_trace_plan
nofl_space_object_trace_plan(struct nofl_space *space, struct gc_ref ref) {
  uint8_t *loc = nofl_metadata_byte_for_object(ref);
  uint8_t byte = atomic_load_explicit(loc, memory_order_relaxed);
  enum gc_trace_kind kind = nofl_metadata_byte_trace_kind (space, byte);
  switch (kind) {
    case GC_TRACE_PRECISELY:
    case GC_TRACE_NONE:
      return (struct gc_trace_plan){ kind, };
    case GC_TRACE_CONSERVATIVELY: {
      size_t granules = nofl_space_live_object_granules(loc);
      return (struct gc_trace_plan){ kind, granules * NOFL_GRANULE_SIZE };
    }
    default:
      GC_CRASH();
  }
}

static struct nofl_slab*
nofl_allocate_slabs(size_t nslabs) {
  return gc_platform_acquire_memory(nslabs * NOFL_SLAB_SIZE, NOFL_SLAB_SIZE);
}

static void
nofl_space_add_slabs(struct nofl_space *space, struct nofl_slab *slabs,
                     size_t nslabs) {
  size_t old_size = space->nslabs * sizeof(struct nofl_slab*);
  size_t additional_size = nslabs * sizeof(struct nofl_slab*);
  space->extents = extents_adjoin(space->extents, slabs,
                                  nslabs * sizeof(struct nofl_slab));
  space->slabs = realloc(space->slabs, old_size + additional_size);
  if (!space->slabs)
    GC_CRASH();
  while (nslabs--)
    space->slabs[space->nslabs++] = slabs++;
}

static size_t
nofl_space_shrink(struct nofl_space *space, size_t bytes) {
  ssize_t pending = nofl_space_request_release_memory(space, bytes);
  struct gc_lock lock = nofl_space_lock(space);

  // First try to shrink by unmapping previously-identified empty blocks.
  while (pending > 0) {
    struct nofl_block_ref block = nofl_pop_empty_block_with_lock(space, &lock);
    if (nofl_block_is_null(block))
      break;
    nofl_push_unavailable_block(space, block, &lock);
    pending = atomic_fetch_sub(&space->pending_unavailable_bytes,
                               NOFL_BLOCK_SIZE);
    pending -= NOFL_BLOCK_SIZE;
  }
  
  // If we still need to shrink, steal from the evacuation reserve, if it's more
  // than the minimum.  Not racy: evacuation target lists are built during eager
  // lazy sweep, which is mutually exclusive with consumption, itself either
  // during trace, synchronously from gc_heap_sizer_on_gc, or async but subject
  // to the heap lock.
  if (pending > 0) {
    size_t active = nofl_active_block_count(space);
    size_t target = ceil(space->evacuation_minimum_reserve * active);
    ssize_t avail = nofl_evacuation_block_count(space);
    while (avail-- > target && pending > 0) {
      struct nofl_block_ref block =
        nofl_block_list_pop(&space->evacuation_targets);
      GC_ASSERT(!nofl_block_is_null(block));
      nofl_push_unavailable_block(space, block, &lock);
      pending = atomic_fetch_sub(&space->pending_unavailable_bytes,
                                 NOFL_BLOCK_SIZE);
      pending -= NOFL_BLOCK_SIZE;
    }
  }

  gc_lock_release(&lock);

  // It still may be the case we need to page out more blocks.  Only evacuation
  // can help us then!
  return pending <= 0 ? 0 : pending;
}
      
static void
nofl_space_expand(struct nofl_space *space, size_t bytes) {
  double overhead = ((double)NOFL_META_BLOCKS_PER_SLAB) / NOFL_BLOCKS_PER_SLAB;
  ssize_t to_acquire = -nofl_space_maybe_reacquire_memory(space, bytes);
  if (to_acquire < NOFL_BLOCK_SIZE) return;
  to_acquire *= (1 + overhead);
  size_t reserved = align_up(to_acquire, NOFL_SLAB_SIZE);
  size_t nslabs = reserved / NOFL_SLAB_SIZE;
  struct nofl_slab *slabs = nofl_allocate_slabs(nslabs);
  nofl_space_add_slabs(space, slabs, nslabs);

  struct gc_lock lock = nofl_space_lock(space);
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < NOFL_NONMETA_BLOCKS_PER_SLAB; idx++) {
      uintptr_t addr = (uintptr_t)slabs[slab].blocks[idx].data;
      struct nofl_block_ref block = nofl_block_for_addr(addr);
      nofl_block_set_flag(block, NOFL_BLOCK_ZERO | NOFL_BLOCK_PAGED_OUT);
      nofl_push_unavailable_block(space, block, &lock);
    }
  }
  gc_lock_release(&lock);
  nofl_space_maybe_reacquire_memory(space, 0);
}

static void
nofl_space_advance_page_out_queue(void *data) {
  // When the nofl space goes to return a block to the OS, it goes on the head
  // of the page-out queue.  Every second, the background thread will age the
  // queue, moving all blocks from index 0 to index 1, and so on.  When a block
  // reaches the end of the queue it is paged out (and stays at the end of the
  // queue).  In this task, invoked by the background thread, we age queue
  // items, except that we don't page out yet, as it could be that some other
  // background task will need to pull pages back in.
  struct nofl_space *space = data;
  struct gc_lock lock = nofl_space_lock(space);
  for (int age = NOFL_PAGE_OUT_QUEUE_SIZE - 3; age >= 0; age--) {
    struct nofl_block_ref block =
      nofl_block_stack_pop(&space->paged_out[age], &lock);
    if (nofl_block_is_null(block))
      break;
    nofl_block_stack_push(&space->paged_out[age+1], block, &lock);
  }
  gc_lock_release(&lock);
}

static void
nofl_space_page_out_blocks(void *data) {
  // This task is invoked by the background thread after other tasks.  It
  // actually pages out blocks that reached the end of the queue.
  struct nofl_space *space = data;
  struct gc_lock lock = nofl_space_lock(space);
  int age = NOFL_PAGE_OUT_QUEUE_SIZE - 2;
  while (1) {
    struct nofl_block_ref block =
      nofl_block_stack_pop(&space->paged_out[age], &lock);
    if (nofl_block_is_null(block))
      break;
    nofl_block_set_flag(block, NOFL_BLOCK_ZERO | NOFL_BLOCK_PAGED_OUT);
    gc_platform_discard_memory((void*)block.addr, NOFL_BLOCK_SIZE);
    nofl_block_stack_push(&space->paged_out[age + 1], block, &lock);
  }
  gc_lock_release(&lock);
}

static int
nofl_space_init(struct nofl_space *space, size_t size, int atomic,
                double promotion_threshold,
                struct gc_background_thread *thread) {
  size = align_up(size, NOFL_BLOCK_SIZE);
  size_t reserved = align_up(size, NOFL_SLAB_SIZE);
  size_t nslabs = reserved / NOFL_SLAB_SIZE;
  struct nofl_slab *slabs = nofl_allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  space->current_mark = space->survivor_mark = NOFL_METADATA_BYTE_MARK_0;
  space->extents = extents_allocate(10);
  space->heap_has_ambiguous_edges = GC_CONSERVATIVE_TRACE;
  nofl_space_add_slabs(space, slabs, nslabs);
  pthread_mutex_init(&space->lock, NULL);
  space->evacuation_minimum_reserve = GC_CONSERVATIVE_TRACE ? 0.0 : 0.02;
  space->evacuation_reserve = space->evacuation_minimum_reserve;
  space->promotion_threshold = promotion_threshold;
  struct gc_lock lock = nofl_space_lock(space);
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t idx = 0; idx < NOFL_NONMETA_BLOCKS_PER_SLAB; idx++) {
      uintptr_t addr = (uintptr_t)slabs[slab].blocks[idx].data;
      struct nofl_block_ref block = nofl_block_for_addr(addr);
      nofl_block_set_flag(block, NOFL_BLOCK_ZERO | NOFL_BLOCK_PAGED_OUT);
      if (reserved > size) {
        nofl_push_unavailable_block(space, block, &lock);
        reserved -= NOFL_BLOCK_SIZE;
      } else {
        if (!nofl_push_evacuation_target_if_needed(space, block))
          nofl_push_empty_block(space, block, &lock);
      }
    }
  }
  gc_lock_release(&lock);
  gc_background_thread_add_task(thread, GC_BACKGROUND_TASK_START,
                                nofl_space_advance_page_out_queue,
                                space);
  gc_background_thread_add_task(thread, GC_BACKGROUND_TASK_END,
                                nofl_space_page_out_blocks,
                                space);
  return 1;
}

#endif // NOFL_SPACE_H
