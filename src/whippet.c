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
#if GC_PARALLEL
#include "parallel-tracer.h"
#else
#include "serial-tracer.h"
#endif
#include "spin.h"
#include "whippet-attrs.h"

#define GRANULE_SIZE 16
#define GRANULE_SIZE_LOG_2 4
#define MEDIUM_OBJECT_THRESHOLD 256
#define MEDIUM_OBJECT_GRANULE_THRESHOLD 16
#define LARGE_OBJECT_THRESHOLD 8192
#define LARGE_OBJECT_GRANULE_THRESHOLD 512

STATIC_ASSERT_EQ(GRANULE_SIZE, 1 << GRANULE_SIZE_LOG_2);
STATIC_ASSERT_EQ(MEDIUM_OBJECT_THRESHOLD,
                 MEDIUM_OBJECT_GRANULE_THRESHOLD * GRANULE_SIZE);
STATIC_ASSERT_EQ(LARGE_OBJECT_THRESHOLD,
                 LARGE_OBJECT_GRANULE_THRESHOLD * GRANULE_SIZE);

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
// will have this bit set.  However, whippet never needs to check for
// the young bit; if it weren't for the need to identify conservative
// roots, we wouldn't need a young bit at all.  Perhaps in an
// all-precise system, we would be able to avoid the overhead of
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
enum metadata_byte {
  METADATA_BYTE_NONE = 0,
  METADATA_BYTE_YOUNG = 1,
  METADATA_BYTE_MARK_0 = 2,
  METADATA_BYTE_MARK_1 = 4,
  METADATA_BYTE_MARK_2 = 8,
  METADATA_BYTE_END = 16,
  METADATA_BYTE_EPHEMERON = 32,
  METADATA_BYTE_PINNED = 64,
  METADATA_BYTE_UNUSED_1 = 128
};

static uint8_t rotate_dead_survivor_marked(uint8_t mask) {
  uint8_t all =
    METADATA_BYTE_MARK_0 | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  return ((mask << 1) | (mask >> 2)) & all;
}

#define SLAB_SIZE (4 * 1024 * 1024)
#define BLOCK_SIZE (64 * 1024)
#define METADATA_BYTES_PER_BLOCK (BLOCK_SIZE / GRANULE_SIZE)
#define BLOCKS_PER_SLAB (SLAB_SIZE / BLOCK_SIZE)
#define META_BLOCKS_PER_SLAB (METADATA_BYTES_PER_BLOCK * BLOCKS_PER_SLAB / BLOCK_SIZE)
#define NONMETA_BLOCKS_PER_SLAB (BLOCKS_PER_SLAB - META_BLOCKS_PER_SLAB)
#define METADATA_BYTES_PER_SLAB (NONMETA_BLOCKS_PER_SLAB * METADATA_BYTES_PER_BLOCK)
#define SLACK_METADATA_BYTES_PER_SLAB (META_BLOCKS_PER_SLAB * METADATA_BYTES_PER_BLOCK)
#define REMSET_BYTES_PER_BLOCK (SLACK_METADATA_BYTES_PER_SLAB / BLOCKS_PER_SLAB)
#define REMSET_BYTES_PER_SLAB (REMSET_BYTES_PER_BLOCK * NONMETA_BLOCKS_PER_SLAB)
#define SLACK_REMSET_BYTES_PER_SLAB (REMSET_BYTES_PER_BLOCK * META_BLOCKS_PER_SLAB)
#define SUMMARY_BYTES_PER_BLOCK (SLACK_REMSET_BYTES_PER_SLAB / BLOCKS_PER_SLAB)
#define SUMMARY_BYTES_PER_SLAB (SUMMARY_BYTES_PER_BLOCK * NONMETA_BLOCKS_PER_SLAB)
#define SLACK_SUMMARY_BYTES_PER_SLAB (SUMMARY_BYTES_PER_BLOCK * META_BLOCKS_PER_SLAB)
#define HEADER_BYTES_PER_SLAB SLACK_SUMMARY_BYTES_PER_SLAB

struct slab;

struct slab_header {
  union {
    struct {
      struct slab *next;
      struct slab *prev;
    };
    uint8_t padding[HEADER_BYTES_PER_SLAB];
  };
};
STATIC_ASSERT_EQ(sizeof(struct slab_header), HEADER_BYTES_PER_SLAB);

// Sometimes we want to put a block on a singly-linked list.  For that
// there's a pointer reserved in the block summary.  But because the
// pointer is aligned (32kB on 32-bit, 64kB on 64-bit), we can portably
// hide up to 15 flags in the low bits.  These flags can be accessed
// non-atomically by the mutator when it owns a block; otherwise they
// need to be accessed atomically.
enum block_summary_flag {
  BLOCK_OUT_FOR_THREAD = 0x1,
  BLOCK_HAS_PIN = 0x2,
  BLOCK_PAGED_OUT = 0x4,
  BLOCK_NEEDS_SWEEP = 0x8,
  BLOCK_UNAVAILABLE = 0x10,
  BLOCK_EVACUATE = 0x20,
  BLOCK_VENERABLE = 0x40,
  BLOCK_VENERABLE_AFTER_SWEEP = 0x80,
  BLOCK_FLAG_UNUSED_8 = 0x100,
  BLOCK_FLAG_UNUSED_9 = 0x200,
  BLOCK_FLAG_UNUSED_10 = 0x400,
  BLOCK_FLAG_UNUSED_11 = 0x800,
  BLOCK_FLAG_UNUSED_12 = 0x1000,
  BLOCK_FLAG_UNUSED_13 = 0x2000,
  BLOCK_FLAG_UNUSED_14 = 0x4000,
};

struct block_summary {
  union {
    struct {
      //struct block *next;
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
    uint8_t padding[SUMMARY_BYTES_PER_BLOCK];
  };
};
STATIC_ASSERT_EQ(sizeof(struct block_summary), SUMMARY_BYTES_PER_BLOCK);

struct block {
  char data[BLOCK_SIZE];
};

struct slab {
  struct slab_header header;
  struct block_summary summaries[NONMETA_BLOCKS_PER_SLAB];
  uint8_t remembered_set[REMSET_BYTES_PER_SLAB];
  uint8_t metadata[METADATA_BYTES_PER_SLAB];
  struct block blocks[NONMETA_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct slab), SLAB_SIZE);

static struct slab *object_slab(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = align_down(addr, SLAB_SIZE);
  return (struct slab*) base;
}

static uint8_t *metadata_byte_for_addr(uintptr_t addr) {
  uintptr_t base = align_down(addr, SLAB_SIZE);
  uintptr_t granule = (addr & (SLAB_SIZE - 1)) >> GRANULE_SIZE_LOG_2;
  return (uint8_t*) (base + granule);
}

static uint8_t *metadata_byte_for_object(struct gc_ref ref) {
  return metadata_byte_for_addr(gc_ref_value(ref));
}

#define GRANULES_PER_BLOCK (BLOCK_SIZE / GRANULE_SIZE)
#define GRANULES_PER_REMSET_BYTE (GRANULES_PER_BLOCK / REMSET_BYTES_PER_BLOCK)

static struct block_summary* block_summary_for_addr(uintptr_t addr) {
  uintptr_t base = align_down(addr, SLAB_SIZE);
  uintptr_t block = (addr & (SLAB_SIZE - 1)) / BLOCK_SIZE;
  return (struct block_summary*) (base + block * sizeof(struct block_summary));
}

static uintptr_t block_summary_has_flag(struct block_summary *summary,
                                        enum block_summary_flag flag) {
  return summary->next_and_flags & flag;
}
static void block_summary_set_flag(struct block_summary *summary,
                                   enum block_summary_flag flag) {
  summary->next_and_flags |= flag;
}
static void block_summary_clear_flag(struct block_summary *summary,
                                     enum block_summary_flag flag) {
  summary->next_and_flags &= ~(uintptr_t)flag;
}
static uintptr_t block_summary_next(struct block_summary *summary) {
  return align_down(summary->next_and_flags, BLOCK_SIZE);
}
static void block_summary_set_next(struct block_summary *summary,
                                   uintptr_t next) {
  GC_ASSERT((next & (BLOCK_SIZE - 1)) == 0);
  summary->next_and_flags =
    (summary->next_and_flags & (BLOCK_SIZE - 1)) | next;
}

// Lock-free block list.
struct block_list {
  size_t count;
  uintptr_t blocks;
};

static void push_block(struct block_list *list, uintptr_t block) {
  atomic_fetch_add_explicit(&list->count, 1, memory_order_acq_rel);
  struct block_summary *summary = block_summary_for_addr(block);
  uintptr_t next = atomic_load_explicit(&list->blocks, memory_order_acquire);
  do {
    block_summary_set_next(summary, next);
  } while (!atomic_compare_exchange_weak(&list->blocks, &next, block));
}

static uintptr_t pop_block(struct block_list *list) {
  uintptr_t head = atomic_load_explicit(&list->blocks, memory_order_acquire);
  struct block_summary *summary;
  uintptr_t next;
  do {
    if (!head)
      return 0;
    summary = block_summary_for_addr(head);
    next = block_summary_next(summary);
  } while (!atomic_compare_exchange_weak(&list->blocks, &head, next));
  block_summary_set_next(summary, 0);
  atomic_fetch_sub_explicit(&list->count, 1, memory_order_acq_rel);
  return head;
}

static inline size_t size_to_granules(size_t size) {
  return (size + GRANULE_SIZE - 1) >> GRANULE_SIZE_LOG_2;
}

struct evacuation_allocator {
  size_t allocated; // atomically
  size_t limit;
  uintptr_t block_cursor; // atomically
};

struct mark_space {
  uint64_t sweep_mask;
  uint8_t live_mask;
  uint8_t marked_mask;
  uint8_t evacuating;
  uintptr_t low_addr;
  size_t extent;
  size_t heap_size;
  uintptr_t next_block;   // atomically
  struct block_list empty;
  struct block_list unavailable;
  struct block_list evacuation_targets;
  double evacuation_minimum_reserve;
  double evacuation_reserve;
  double venerable_threshold;
  ssize_t pending_unavailable_bytes; // atomically
  struct evacuation_allocator evacuation_allocator;
  struct slab *slabs;
  size_t nslabs;
  uintptr_t granules_freed_by_last_collection; // atomically
  uintptr_t fragmentation_granules_since_last_collection; // atomically
};

enum gc_kind {
  GC_KIND_FLAG_MINOR = GC_GENERATIONAL, // 0 or 1
  GC_KIND_FLAG_EVACUATING = 0x2,
  GC_KIND_MINOR_IN_PLACE = GC_KIND_FLAG_MINOR,
  GC_KIND_MINOR_EVACUATING = GC_KIND_FLAG_MINOR | GC_KIND_FLAG_EVACUATING,
  GC_KIND_MAJOR_IN_PLACE = 0,
  GC_KIND_MAJOR_EVACUATING = GC_KIND_FLAG_EVACUATING,
};

struct gc_heap {
  struct mark_space mark_space;
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
  enum gc_kind gc_kind;
  int multithreaded;
  size_t active_mutator_count;
  size_t mutator_count;
  struct gc_heap_roots *roots;
  struct gc_mutator *mutator_trace_list;
  long count;
  long minor_count;
  uint8_t last_collection_was_minor;
  struct gc_mutator *deactivated_mutators;
  struct tracer tracer;
  double fragmentation_low_threshold;
  double fragmentation_high_threshold;
  double minor_gc_yield_threshold;
  double major_gc_yield_threshold;
  double minimum_major_gc_yield_threshold;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
};

struct gc_mutator_mark_buf {
  size_t size;
  size_t capacity;
  struct gc_ref *objects;
};

struct gc_mutator {
  // Bump-pointer allocation into holes.
  uintptr_t alloc;
  uintptr_t sweep;
  uintptr_t block;
  struct gc_heap *heap;
  struct gc_stack stack;
  struct gc_mutator_roots *roots;
  struct gc_mutator_mark_buf mark_buf;
  // Three uses for this in-object linked-list pointer:
  //  - inactive (blocked in syscall) mutators
  //  - grey objects when stopping active mutators for mark-in-place
  //  - untraced mutators when stopping active mutators for evacuation
  struct gc_mutator *next;
};

static inline struct tracer* heap_tracer(struct gc_heap *heap) {
  return &heap->tracer;
}
static inline struct mark_space* heap_mark_space(struct gc_heap *heap) {
  return &heap->mark_space;
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

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct gc_mutator *mut) GC_NEVER_INLINE;

static inline uint64_t load_eight_aligned_bytes(uint8_t *mark) {
  GC_ASSERT(((uintptr_t)mark & 7) == 0);
  uint8_t * __attribute__((aligned(8))) aligned_mark = mark;
  uint64_t word;
  memcpy(&word, aligned_mark, 8);
#ifdef WORDS_BIGENDIAN
  word = __builtin_bswap64(word);
#endif
  return word;
}

static inline size_t count_zero_bytes(uint64_t bytes) {
  return bytes ? (__builtin_ctzll(bytes) / 8) : sizeof(bytes);
}

static uint64_t broadcast_byte(uint8_t byte) {
  uint64_t result = byte;
  return result * 0x0101010101010101ULL;
}

static size_t next_mark(uint8_t *mark, size_t limit, uint64_t sweep_mask) {
  size_t n = 0;
  // If we have a hole, it is likely to be more that 8 granules long.
  // Assuming that it's better to make aligned loads, first we align the
  // sweep pointer, then we load aligned mark words.
  size_t unaligned = ((uintptr_t) mark) & 7;
  if (unaligned) {
    uint64_t bytes = load_eight_aligned_bytes(mark - unaligned) >> (unaligned * 8);
    bytes &= sweep_mask;
    if (bytes)
      return count_zero_bytes(bytes);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_eight_aligned_bytes(mark + n);
    bytes &= sweep_mask;
    if (bytes)
      return n + count_zero_bytes(bytes);
  }

  return limit;
}

static size_t mark_space_live_object_granules(uint8_t *metadata) {
  return next_mark(metadata, -1, broadcast_byte(METADATA_BYTE_END)) + 1;
}

static inline int mark_space_mark_object(struct mark_space *space,
                                         struct gc_ref ref) {
  uint8_t *loc = metadata_byte_for_object(ref);
  uint8_t byte = *loc;
  if (byte & space->marked_mask)
    return 0;
  uint8_t mask = METADATA_BYTE_YOUNG | METADATA_BYTE_MARK_0
    | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  *loc = (byte & ~mask) | space->marked_mask;
  return 1;
}

static uintptr_t make_evacuation_allocator_cursor(uintptr_t block,
                                                  size_t allocated) {
  GC_ASSERT(allocated < (BLOCK_SIZE - 1) * (uint64_t) BLOCK_SIZE);
  return align_down(block, BLOCK_SIZE) | (allocated / BLOCK_SIZE);
}

static void prepare_evacuation_allocator(struct evacuation_allocator *alloc,
                                         struct block_list *targets) {
  uintptr_t first_block = targets->blocks;
  atomic_store_explicit(&alloc->allocated, 0, memory_order_release);
  alloc->limit =
    atomic_load_explicit(&targets->count, memory_order_acquire) * BLOCK_SIZE;
  atomic_store_explicit(&alloc->block_cursor,
                        make_evacuation_allocator_cursor(first_block, 0),
                        memory_order_release);
}

static void clear_remaining_metadata_bytes_in_block(uintptr_t block,
                                                    uintptr_t allocated) {
  GC_ASSERT((allocated & (GRANULE_SIZE - 1)) == 0);
  uintptr_t base = block + allocated;
  uintptr_t limit = block + BLOCK_SIZE;
  uintptr_t granules = (limit - base) >> GRANULE_SIZE_LOG_2;
  GC_ASSERT(granules <= GRANULES_PER_BLOCK);
  memset(metadata_byte_for_addr(base), 0, granules);
}

static void finish_evacuation_allocator_block(uintptr_t block,
                                              uintptr_t allocated) {
  GC_ASSERT(allocated <= BLOCK_SIZE);
  struct block_summary *summary = block_summary_for_addr(block);
  block_summary_set_flag(summary, BLOCK_NEEDS_SWEEP);
  size_t fragmentation = (BLOCK_SIZE - allocated) >> GRANULE_SIZE_LOG_2;
  summary->hole_count = 1;
  summary->free_granules = GRANULES_PER_BLOCK;
  summary->holes_with_fragmentation = fragmentation ? 1 : 0;
  summary->fragmentation_granules = fragmentation;
  if (fragmentation)
    clear_remaining_metadata_bytes_in_block(block, allocated);
}

static void finish_evacuation_allocator(struct evacuation_allocator *alloc,
                                        struct block_list *targets,
                                        struct block_list *empties,
                                        size_t reserve) {
  // Blocks that we used for evacuation get returned to the mutator as
  // sweepable blocks.  Blocks that we didn't get to use go to the
  // empties.
  size_t allocated = atomic_load_explicit(&alloc->allocated,
                                          memory_order_acquire);
  atomic_store_explicit(&alloc->allocated, 0, memory_order_release);
  if (allocated > alloc->limit)
    allocated = alloc->limit;
  while (allocated >= BLOCK_SIZE) {
    uintptr_t block = pop_block(targets);
    GC_ASSERT(block);
    allocated -= BLOCK_SIZE;
  }
  if (allocated) {
    // Finish off the last partially-filled block.
    uintptr_t block = pop_block(targets);
    GC_ASSERT(block);
    finish_evacuation_allocator_block(block, allocated);
  }
  size_t remaining = atomic_load_explicit(&targets->count, memory_order_acquire);
  while (remaining-- > reserve)
    push_block(empties, pop_block(targets));
}

static struct gc_ref evacuation_allocate(struct mark_space *space,
                                         size_t granules) {
  // All collector threads compete to allocate from what is logically a
  // single bump-pointer arena, which is actually composed of a linked
  // list of blocks.
  struct evacuation_allocator *alloc = &space->evacuation_allocator;
  uintptr_t cursor = atomic_load_explicit(&alloc->block_cursor,
                                          memory_order_acquire);
  size_t bytes = granules * GRANULE_SIZE;
  size_t prev = atomic_load_explicit(&alloc->allocated, memory_order_acquire);
  size_t block_mask = (BLOCK_SIZE - 1);
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
  // and continuing for BLOCK_SIZE bytes.
  uintptr_t base = seq * BLOCK_SIZE;

  while ((base ^ next) & ~block_mask) {
    GC_ASSERT(base < next);
    if (base + BLOCK_SIZE > prev) {
      // The allocation straddles a block boundary, and the cursor has
      // caught up so that we identify the block for the previous
      // allocation pointer.  Finish the previous block, probably
      // leaving a small hole at the end.
      finish_evacuation_allocator_block(block, prev - base);
    }
    // Cursor lags; advance it.
    block = block_summary_next(block_summary_for_addr(block));
    base += BLOCK_SIZE;
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
                          make_evacuation_allocator_cursor(block, base),
                          memory_order_release);
  }

  uintptr_t addr = block + (next & block_mask) - bytes;
  return gc_ref(addr);
}

static inline int mark_space_evacuate_or_mark_object(struct mark_space *space,
                                                     struct gc_edge edge,
                                                     struct gc_ref old_ref) {
  uint8_t *metadata = metadata_byte_for_object(old_ref);
  uint8_t byte = *metadata;
  if (byte & space->marked_mask)
    return 0;
  if (space->evacuating &&
      block_summary_has_flag(block_summary_for_addr(gc_ref_value(old_ref)),
                             BLOCK_EVACUATE)) {
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
      size_t object_granules = mark_space_live_object_granules(metadata);
      struct gc_ref new_ref = evacuation_allocate(space, object_granules);
      if (gc_ref_is_heap_object(new_ref)) {
        // Copy object contents before committing, as we don't know what
        // part of the object (if any) will be overwritten by the
        // commit.
        memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(old_ref),
               object_granules * GRANULE_SIZE);
        gc_atomic_forward_commit(&fwd, new_ref);
        // Now update extent metadata, and indicate to the caller that
        // the object's fields need to be traced.
        uint8_t *new_metadata = metadata_byte_for_object(new_ref);
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

  uint8_t mask = METADATA_BYTE_YOUNG | METADATA_BYTE_MARK_0
    | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  *metadata = (byte & ~mask) | space->marked_mask;
  return 1;
}

static inline int mark_space_contains_address(struct mark_space *space,
                                              uintptr_t addr) {
  return addr - space->low_addr < space->extent;
}

static inline int mark_space_contains_conservative_ref(struct mark_space *space,
                                                       struct gc_conservative_ref ref) {
  return mark_space_contains_address(space, gc_conservative_ref_value(ref));
}

static inline int mark_space_contains(struct mark_space *space,
                                      struct gc_ref ref) {
  return mark_space_contains_address(space, gc_ref_value(ref));
}

static inline int do_trace(struct gc_heap *heap, struct gc_edge edge,
                           struct gc_ref ref) {
  if (!gc_ref_is_heap_object(ref))
    return 0;
  if (GC_LIKELY(mark_space_contains(heap_mark_space(heap), ref))) {
    if (heap_mark_space(heap)->evacuating)
      return mark_space_evacuate_or_mark_object(heap_mark_space(heap), edge,
                                                ref);
    return mark_space_mark_object(heap_mark_space(heap), ref);
  }
  else if (large_object_space_contains(heap_large_object_space(heap), ref))
    return large_object_space_mark_object(heap_large_object_space(heap),
                                          ref);
  else
    return gc_extern_space_visit(heap_extern_space(heap), edge, ref);
}

static inline int trace_edge(struct gc_heap *heap, struct gc_edge edge) {
  struct gc_ref ref = gc_edge_ref(edge);
  int is_new = do_trace(heap, edge, ref);

  if (GC_UNLIKELY(atomic_load_explicit(&heap->check_pending_ephemerons,
                                       memory_order_relaxed)))
    gc_resolve_pending_ephemerons(ref, heap);

  return is_new;
}

int gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  if (!gc_ref_is_heap_object(ref))
    return 0;
  if (GC_LIKELY(mark_space_contains(heap_mark_space(heap), ref))) {
    struct mark_space *space = heap_mark_space(heap);
    uint8_t *metadata = metadata_byte_for_object(ref);
    uint8_t byte = *metadata;
    if (byte & space->marked_mask)
      return 1;

    if (!space->evacuating)
      return 0;
    if (!block_summary_has_flag(block_summary_for_addr(gc_ref_value(ref)),
                                BLOCK_EVACUATE))
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
  } else if (large_object_space_contains(heap_large_object_space(heap), ref)) {
    return large_object_space_is_copied(heap_large_object_space(heap), ref);
  }
  GC_CRASH();
}

static inline struct gc_ref mark_space_mark_conservative_ref(struct mark_space *space,
                                                             struct gc_conservative_ref ref,
                                                             int possibly_interior) {
  uintptr_t addr = gc_conservative_ref_value(ref);

  if (possibly_interior) {
    addr = align_down(addr, GRANULE_SIZE);
  } else {
    // Addr not an aligned granule?  Not an object.
    uintptr_t displacement = addr & (GRANULE_SIZE - 1);
    if (!gc_is_valid_conservative_ref_displacement(displacement))
      return gc_ref_null();
    addr -= displacement;
  }

  // Addr in meta block?  Not an object.
  if ((addr & (SLAB_SIZE - 1)) < META_BLOCKS_PER_SLAB * BLOCK_SIZE)
    return gc_ref_null();

  // Addr in block that has been paged out?  Not an object.
  struct block_summary *summary = block_summary_for_addr(addr);
  if (block_summary_has_flag(summary, BLOCK_UNAVAILABLE))
    return gc_ref_null();

  uint8_t *loc = metadata_byte_for_addr(addr);
  uint8_t byte = atomic_load_explicit(loc, memory_order_relaxed);

  // Already marked object?  Nothing to do.
  if (byte & space->marked_mask)
    return gc_ref_null();

  // Addr is the not start of an unmarked object?  Search backwards if
  // we have interior pointers, otherwise not an object.
  uint8_t object_start_mask = space->live_mask | METADATA_BYTE_YOUNG;
  if (!(byte & object_start_mask)) {
    if (!possibly_interior)
      return gc_ref_null();

    uintptr_t block_base = align_down(addr, BLOCK_SIZE);
    uint8_t *loc_base = metadata_byte_for_addr(block_base);
    do {
      // Searched past block?  Not an object.
      if (loc-- == loc_base)
        return gc_ref_null();

      byte = atomic_load_explicit(loc, memory_order_relaxed);

      // Ran into the end of some other allocation?  Not an object, then.
      if (byte & METADATA_BYTE_END)
        return gc_ref_null();

      // Continue until we find object start.
    } while (!(byte & object_start_mask));

    // Found object start, and object is unmarked; adjust addr.
    addr = block_base + (loc - loc_base) * GRANULE_SIZE;
  }

  uint8_t mask = METADATA_BYTE_YOUNG | METADATA_BYTE_MARK_0
    | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  atomic_store_explicit(loc, (byte & ~mask) | space->marked_mask,
                        memory_order_relaxed);

  return gc_ref(addr);
}

static inline struct gc_ref do_trace_conservative_ref(struct gc_heap *heap,
                                                      struct gc_conservative_ref ref,
                                                      int possibly_interior) {
  if (!gc_conservative_ref_might_be_a_heap_object(ref, possibly_interior))
    return gc_ref_null();

  if (GC_LIKELY(mark_space_contains_conservative_ref(heap_mark_space(heap), ref)))
    return mark_space_mark_conservative_ref(heap_mark_space(heap), ref,
                                            possibly_interior);
  else
    return large_object_space_mark_conservative_ref(heap_large_object_space(heap),
                                                    ref, possibly_interior);
}

static inline struct gc_ref trace_conservative_ref(struct gc_heap *heap,
                                                   struct gc_conservative_ref ref,
                                                   int possibly_interior) {
  struct gc_ref ret = do_trace_conservative_ref(heap, ref, possibly_interior);

  if (gc_ref_is_heap_object(ret) &&
      GC_UNLIKELY(atomic_load_explicit(&heap->check_pending_ephemerons,
                                       memory_order_relaxed)))
    gc_resolve_pending_ephemerons(ret, heap);

  return ret;
}

static inline size_t mark_space_object_size(struct mark_space *space,
                                            struct gc_ref ref) {
  uint8_t *loc = metadata_byte_for_object(ref);
  size_t granules = mark_space_live_object_granules(loc);
  return granules * GRANULE_SIZE;
}

static inline size_t gc_object_allocation_size(struct gc_heap *heap,
                                               struct gc_ref ref) {
  if (GC_LIKELY(mark_space_contains(heap_mark_space(heap), ref)))
    return mark_space_object_size(heap_mark_space(heap), ref);
  return large_object_space_object_size(heap_large_object_space(heap), ref);
}

static int heap_has_multiple_mutators(struct gc_heap *heap) {
  return atomic_load_explicit(&heap->multithreaded, memory_order_relaxed);
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

static void add_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  mut->heap = heap;
  heap_lock(heap);
  // We have no roots.  If there is a GC currently in progress, we have
  // nothing to add.  Just wait until it's done.
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  if (heap->mutator_count == 1)
    heap->multithreaded = 1;
  heap->active_mutator_count++;
  heap->mutator_count++;
  heap_unlock(heap);
}

static void remove_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  mut->heap = NULL;
  heap_lock(heap);
  heap->active_mutator_count--;
  heap->mutator_count--;
  // We have no roots.  If there is a GC stop currently in progress,
  // maybe tell the controller it can continue.
  if (mutators_are_stopping(heap) && heap->active_mutator_count == 0)
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void request_mutators_to_stop(struct gc_heap *heap) {
  GC_ASSERT(!mutators_are_stopping(heap));
  atomic_store_explicit(&heap->collecting, 1, memory_order_relaxed);
}

static void allow_mutators_to_continue(struct gc_heap *heap) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(heap->active_mutator_count == 0);
  heap->active_mutator_count++;
  atomic_store_explicit(&heap->collecting, 0, memory_order_relaxed);
  GC_ASSERT(!mutators_are_stopping(heap));
  pthread_cond_broadcast(&heap->mutator_cond);
}

static void push_unavailable_block(struct mark_space *space, uintptr_t block) {
  struct block_summary *summary = block_summary_for_addr(block);
  GC_ASSERT(!block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP));
  GC_ASSERT(!block_summary_has_flag(summary, BLOCK_UNAVAILABLE));
  block_summary_set_flag(summary, BLOCK_UNAVAILABLE);
  madvise((void*)block, BLOCK_SIZE, MADV_DONTNEED);
  push_block(&space->unavailable, block);
}

static uintptr_t pop_unavailable_block(struct mark_space *space) {
  uintptr_t block = pop_block(&space->unavailable);
  if (!block)
    return 0;
  struct block_summary *summary = block_summary_for_addr(block);
  GC_ASSERT(block_summary_has_flag(summary, BLOCK_UNAVAILABLE));
  block_summary_clear_flag(summary, BLOCK_UNAVAILABLE);
  return block;
}

static uintptr_t pop_empty_block(struct mark_space *space) {
  return pop_block(&space->empty);
}

static int maybe_push_evacuation_target(struct mark_space *space,
                                        uintptr_t block, double reserve) {
  GC_ASSERT(!block_summary_has_flag(block_summary_for_addr(block),
                                 BLOCK_NEEDS_SWEEP));
  size_t targets = atomic_load_explicit(&space->evacuation_targets.count,
                                        memory_order_acquire);
  size_t total = space->nslabs * NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = atomic_load_explicit(&space->unavailable.count,
                                            memory_order_acquire);
  if (targets >= (total - unavailable) * reserve)
    return 0;

  push_block(&space->evacuation_targets, block);
  return 1;
}

static int push_evacuation_target_if_needed(struct mark_space *space,
                                            uintptr_t block) {
  return maybe_push_evacuation_target(space, block,
                                      space->evacuation_minimum_reserve);
}

static int push_evacuation_target_if_possible(struct mark_space *space,
                                              uintptr_t block) {
  return maybe_push_evacuation_target(space, block,
                                      space->evacuation_reserve);
}

static void push_empty_block(struct mark_space *space, uintptr_t block) {
  GC_ASSERT(!block_summary_has_flag(block_summary_for_addr(block),
                                 BLOCK_NEEDS_SWEEP));
  push_block(&space->empty, block);
}

static ssize_t mark_space_request_release_memory(struct mark_space *space,
                                                 size_t bytes) {
  return atomic_fetch_add(&space->pending_unavailable_bytes, bytes) + bytes;
}

static void mark_space_reacquire_memory(struct mark_space *space,
                                        size_t bytes) {
  ssize_t pending =
    atomic_fetch_sub(&space->pending_unavailable_bytes, bytes) - bytes;
  while (pending + BLOCK_SIZE <= 0) {
    uintptr_t block = pop_unavailable_block(space);
    GC_ASSERT(block);
    if (push_evacuation_target_if_needed(space, block))
      continue;
    push_empty_block(space, block);
    pending = atomic_fetch_add(&space->pending_unavailable_bytes, BLOCK_SIZE)
      + BLOCK_SIZE;
  }
}

static size_t next_hole(struct gc_mutator *mut);

static int sweep_until_memory_released(struct gc_mutator *mut) {
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  ssize_t pending = atomic_load_explicit(&space->pending_unavailable_bytes,
                                         memory_order_acquire);
  // First try to unmap previously-identified empty blocks.  If pending
  // > 0 and other mutators happen to identify empty blocks, they will
  // be unmapped directly and moved to the unavailable list.
  while (pending > 0) {
    uintptr_t block = pop_empty_block(space);
    if (!block)
      break;
    // Note that we may have competing uses; if we're evacuating,
    // perhaps we should push this block to the evacuation target list.
    // That would enable us to reach a fragmentation low water-mark in
    // fewer cycles.  But maybe evacuation started in order to obtain
    // free blocks for large objects; in that case we should just reap
    // the fruits of our labor.  Probably this second use-case is more
    // important.
    push_unavailable_block(space, block);
    pending = atomic_fetch_sub(&space->pending_unavailable_bytes, BLOCK_SIZE);
    pending -= BLOCK_SIZE;
  }
  // Otherwise, sweep, transitioning any empty blocks to unavailable and
  // throwing away any non-empty block.  A bit wasteful but hastening
  // the next collection is a reasonable thing to do here.
  while (pending > 0) {
    if (!next_hole(mut))
      return 0;
    pending = atomic_load_explicit(&space->pending_unavailable_bytes,
                                   memory_order_acquire);
  }
  return pending <= 0;
}

static void heap_reset_large_object_pages(struct gc_heap *heap, size_t npages) {
  size_t previous = heap->large_object_pages;
  heap->large_object_pages = npages;
  GC_ASSERT(npages <= previous);
  size_t bytes = (previous - npages) <<
    heap_large_object_space(heap)->page_size_log2;
  mark_space_reacquire_memory(heap_mark_space(heap), bytes);
}

static void mutator_mark_buf_grow(struct gc_mutator_mark_buf *buf) {
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

static void mutator_mark_buf_push(struct gc_mutator_mark_buf *buf,
                                  struct gc_ref ref) {
  if (GC_UNLIKELY(buf->size == buf->capacity))
    mutator_mark_buf_grow(buf);
  buf->objects[buf->size++] = ref;
}

static void mutator_mark_buf_release(struct gc_mutator_mark_buf *buf) {
  size_t bytes = buf->size * sizeof(struct gc_ref);
  if (bytes >= getpagesize())
    madvise(buf->objects, align_up(bytes, getpagesize()), MADV_DONTNEED);
  buf->size = 0;
}

static void mutator_mark_buf_destroy(struct gc_mutator_mark_buf *buf) {
  size_t bytes = buf->capacity * sizeof(struct gc_ref);
  if (bytes)
    munmap(buf->objects, bytes);
}

static void enqueue_mutator_for_tracing(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mut->next == NULL);
  struct gc_mutator *next =
    atomic_load_explicit(&heap->mutator_trace_list, memory_order_acquire);
  do {
    mut->next = next;
  } while (!atomic_compare_exchange_weak(&heap->mutator_trace_list,
                                         &next, mut));
}

static int heap_should_mark_while_stopping(struct gc_heap *heap) {
  return atomic_load_explicit(&heap->mark_while_stopping, memory_order_acquire);
}

static int mutator_should_mark_while_stopping(struct gc_mutator *mut) {
  return heap_should_mark_while_stopping(mutator_heap(mut));
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

static void trace_and_enqueue_locally(struct gc_edge edge,
                                      struct gc_heap *heap,
                                      void *data) {
  struct gc_mutator *mut = data;
  if (trace_edge(heap, edge))
    mutator_mark_buf_push(&mut->mark_buf, gc_edge_ref(edge));
}

static inline void do_trace_conservative_ref_and_enqueue_locally(struct gc_conservative_ref ref,
                                                                 struct gc_heap *heap,
                                                                 void *data,
                                                                 int possibly_interior) {
  struct gc_mutator *mut = data;
  struct gc_ref object = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(object))
    mutator_mark_buf_push(&mut->mark_buf, object);
}

static void trace_possibly_interior_conservative_ref_and_enqueue_locally
    (struct gc_conservative_ref ref, struct gc_heap *heap, void *data) {
  return do_trace_conservative_ref_and_enqueue_locally(ref, heap, data, 1);
}

static void trace_conservative_ref_and_enqueue_locally
    (struct gc_conservative_ref ref, struct gc_heap *heap, void *data) {
  return do_trace_conservative_ref_and_enqueue_locally(ref, heap, data, 0);
}

static void trace_and_enqueue_globally(struct gc_edge edge,
                                       struct gc_heap *heap,
                                       void *unused) {
  if (trace_edge(heap, edge))
    tracer_enqueue_root(&heap->tracer, gc_edge_ref(edge));
}

static inline void do_trace_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
                                                                  struct gc_heap *heap,
                                                                  void *data,
                                                                  int possibly_interior) {
  struct gc_ref object = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(object))
    tracer_enqueue_root(&heap->tracer, object);
}

static void trace_possibly_interior_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
                                                                          struct gc_heap *heap,
                                                                          void *data) {
  return do_trace_conservative_ref_and_enqueue_globally(ref, heap, data, 1);
}

static void trace_conservative_ref_and_enqueue_globally(struct gc_conservative_ref ref,
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

static inline void tracer_trace_conservative_ref(struct gc_conservative_ref ref,
                                                 struct gc_heap *heap,
                                                 void *data) {
  int possibly_interior = 0;
  struct gc_ref resolved = trace_conservative_ref(heap, ref, possibly_interior);
  if (gc_ref_is_heap_object(resolved))
    tracer_enqueue(resolved, heap, data);
}

static inline void trace_one_conservatively(struct gc_ref ref,
                                            struct gc_heap *heap,
                                            void *mark_data) {
  size_t bytes;
  if (GC_LIKELY(mark_space_contains(heap_mark_space(heap), ref))) {
    // Generally speaking we trace conservatively and don't allow much
    // in the way of incremental precise marking on a
    // conservative-by-default heap.  But, we make an exception for
    // ephemerons.
    uint8_t meta = *metadata_byte_for_addr(gc_ref_value(ref));
    if (GC_UNLIKELY(meta & METADATA_BYTE_EPHEMERON)) {
      gc_trace_ephemeron(gc_ref_heap_object(ref), tracer_visit, heap,
                         mark_data);
      return;
    }
    bytes = mark_space_object_size(heap_mark_space(heap), ref);
  } else {
    bytes = large_object_space_object_size(heap_large_object_space(heap), ref);
  }
  trace_conservative_edges(gc_ref_value(ref),
                           gc_ref_value(ref) + bytes,
                           tracer_trace_conservative_ref, heap,
                           mark_data);
}

static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             void *mark_data) {
  if (gc_has_conservative_intraheap_edges())
    trace_one_conservatively(ref, heap, mark_data);
  else
    gc_trace_object(ref, tracer_visit, heap, mark_data, NULL);
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
static void trace_stopping_mutator_roots(struct gc_mutator *mut) {
  GC_ASSERT(mutator_should_mark_while_stopping(mut));
  struct gc_heap *heap = mutator_heap(mut);
  trace_mutator_conservative_roots(mut,
                                   mark_and_locally_enqueue_mutator_conservative_roots,
                                   heap, mut);
  gc_trace_mutator_roots(mut->roots, trace_and_enqueue_locally, heap, mut);
}

static void trace_mutator_conservative_roots_with_lock(struct gc_mutator *mut) {
  trace_mutator_conservative_roots(mut,
                                   mark_and_globally_enqueue_mutator_conservative_roots,
                                   mutator_heap(mut),
                                   NULL);
}

static void trace_mutator_roots_with_lock(struct gc_mutator *mut) {
  trace_mutator_conservative_roots_with_lock(mut);
  gc_trace_mutator_roots(mut->roots, trace_and_enqueue_globally,
                         mutator_heap(mut), NULL);
}

static void trace_mutator_roots_with_lock_before_stop(struct gc_mutator *mut) {
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    trace_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
}

static void release_stopping_mutator_roots(struct gc_mutator *mut) {
  mutator_mark_buf_release(&mut->mark_buf);
}

static void wait_for_mutators_to_stop(struct gc_heap *heap) {
  heap->active_mutator_count--;
  while (heap->active_mutator_count)
    pthread_cond_wait(&heap->collector_cond, &heap->lock);
}

static void finish_sweeping(struct gc_mutator *mut);
static void finish_sweeping_in_block(struct gc_mutator *mut);

static void trace_mutator_conservative_roots_after_stop(struct gc_heap *heap) {
  int active_mutators_already_marked = heap_should_mark_while_stopping(heap);
  if (!active_mutators_already_marked)
    for (struct gc_mutator *mut = atomic_load(&heap->mutator_trace_list);
         mut;
         mut = mut->next)
      trace_mutator_conservative_roots_with_lock(mut);

  for (struct gc_mutator *mut = heap->deactivated_mutators;
       mut;
       mut = mut->next)
    trace_mutator_conservative_roots_with_lock(mut);
}

static void trace_mutator_roots_after_stop(struct gc_heap *heap) {
  struct gc_mutator *mut = atomic_load(&heap->mutator_trace_list);
  int active_mutators_already_marked = heap_should_mark_while_stopping(heap);
  while (mut) {
    // Also collect any already-marked grey objects and put them on the
    // global trace queue.
    if (active_mutators_already_marked)
      tracer_enqueue_roots(&heap->tracer, mut->mark_buf.objects,
                           mut->mark_buf.size);
    else
      trace_mutator_roots_with_lock(mut);
    // Also unlink mutator_trace_list chain.
    struct gc_mutator *next = mut->next;
    mut->next = NULL;
    mut = next;
  }
  atomic_store(&heap->mutator_trace_list, NULL);

  for (struct gc_mutator *mut = heap->deactivated_mutators; mut; mut = mut->next) {
    finish_sweeping_in_block(mut);
    trace_mutator_roots_with_lock(mut);
  }
}

static void trace_global_conservative_roots(struct gc_heap *heap) {
  if (gc_has_global_conservative_roots())
    gc_platform_visit_global_conservative_roots
      (mark_and_globally_enqueue_heap_conservative_roots, heap, NULL);
}

static void enqueue_generational_root(struct gc_ref ref, struct gc_heap *heap) {
  tracer_enqueue_root(&heap->tracer, ref);
}

// Note that it's quite possible (and even likely) that any given remset
// byte doesn't hold any roots, if all stores were to nursery objects.
STATIC_ASSERT_EQ(GRANULES_PER_REMSET_BYTE % 8, 0);
static void mark_space_trace_card(struct mark_space *space,
                                  struct gc_heap *heap, struct slab *slab,
                                  size_t card) {
  uintptr_t first_addr_in_slab = (uintptr_t) &slab->blocks[0];
  size_t granule_base = card * GRANULES_PER_REMSET_BYTE;
  for (size_t granule_in_remset = 0;
       granule_in_remset < GRANULES_PER_REMSET_BYTE;
       granule_in_remset += 8, granule_base += 8) {
    uint64_t mark_bytes = load_eight_aligned_bytes(slab->metadata + granule_base);
    mark_bytes &= space->sweep_mask;
    while (mark_bytes) {
      size_t granule_offset = count_zero_bytes(mark_bytes);
      mark_bytes &= ~(((uint64_t)0xff) << (granule_offset * 8));
      size_t granule = granule_base + granule_offset;
      uintptr_t addr = first_addr_in_slab + granule * GRANULE_SIZE;
      GC_ASSERT(metadata_byte_for_addr(addr) == &slab->metadata[granule]);
      enqueue_generational_root(gc_ref(addr), heap);
    }
  }
}

static void mark_space_trace_remembered_set(struct mark_space *space,
                                            struct gc_heap *heap) {
  GC_ASSERT(!space->evacuating);
  for (size_t s = 0; s < space->nslabs; s++) {
    struct slab *slab = &space->slabs[s];
    uint8_t *remset = slab->remembered_set;
    for (size_t card_base = 0;
         card_base < REMSET_BYTES_PER_SLAB;
         card_base += 8) {
      uint64_t remset_bytes = load_eight_aligned_bytes(remset + card_base);
      if (!remset_bytes) continue;
      memset(remset + card_base, 0, 8);
      while (remset_bytes) {
        size_t card_offset = count_zero_bytes(remset_bytes);
        remset_bytes &= ~(((uint64_t)0xff) << (card_offset * 8));
        mark_space_trace_card(space, heap, slab, card_base + card_offset);
      }
    }
  }
}

static void mark_space_clear_remembered_set(struct mark_space *space) {
  if (!GC_GENERATIONAL) return;
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    memset(space->slabs[slab].remembered_set, 0, REMSET_BYTES_PER_SLAB);
  }
}

void gc_write_barrier_extern(struct gc_ref obj, size_t obj_size,
                             struct gc_edge edge, struct gc_ref new_val) {
  GC_ASSERT(size > gc_allocator_large_threshold());
  gc_object_set_remembered(obj);
}

static void trace_generational_roots(struct gc_heap *heap) {
  // TODO: Add lospace nursery.
  if (atomic_load(&heap->gc_kind) & GC_KIND_FLAG_MINOR) {
    mark_space_trace_remembered_set(heap_mark_space(heap), heap);
    large_object_space_trace_remembered_set(heap_large_object_space(heap),
                                            enqueue_generational_root,
                                            heap);
  } else {
    mark_space_clear_remembered_set(heap_mark_space(heap));
    large_object_space_clear_remembered_set(heap_large_object_space(heap));
  }
}

static void pause_mutator_for_collection(struct gc_heap *heap) GC_NEVER_INLINE;
static void pause_mutator_for_collection(struct gc_heap *heap) {
  GC_ASSERT(mutators_are_stopping(heap));
  GC_ASSERT(heap->active_mutator_count);
  heap->active_mutator_count--;
  if (heap->active_mutator_count == 0)
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

  heap->active_mutator_count++;
}

static void pause_mutator_for_collection_with_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static void pause_mutator_for_collection_with_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  finish_sweeping_in_block(mut);
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    // No need to collect results in mark buf; we can enqueue roots directly.
    trace_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
  pause_mutator_for_collection(heap);
}

static void pause_mutator_for_collection_without_lock(struct gc_mutator *mut) GC_NEVER_INLINE;
static void pause_mutator_for_collection_without_lock(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  GC_ASSERT(mutators_are_stopping(heap));
  finish_sweeping(mut);
  gc_stack_capture_hot(&mut->stack);
  if (mutator_should_mark_while_stopping(mut))
    trace_stopping_mutator_roots(mut);
  enqueue_mutator_for_tracing(mut);
  heap_lock(heap);
  pause_mutator_for_collection(heap);
  heap_unlock(heap);
  release_stopping_mutator_roots(mut);
}

static inline void maybe_pause_mutator_for_collection(struct gc_mutator *mut) {
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static void reset_sweeper(struct mark_space *space) {
  space->next_block = (uintptr_t) &space->slabs[0].blocks;
}

static void update_mark_patterns(struct mark_space *space,
                                 int advance_mark_mask) {
  uint8_t survivor_mask = space->marked_mask;
  uint8_t next_marked_mask = rotate_dead_survivor_marked(survivor_mask);
  if (advance_mark_mask)
    space->marked_mask = next_marked_mask;
  space->live_mask = survivor_mask | next_marked_mask;
  space->sweep_mask = broadcast_byte(space->live_mask);
}

static void reset_statistics(struct mark_space *space) {
  space->granules_freed_by_last_collection = 0;
  space->fragmentation_granules_since_last_collection = 0;
}

static int maybe_grow_heap(struct gc_heap *heap) {
  return 0;
}

static double heap_last_gc_yield(struct gc_heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  size_t mark_space_yield = mark_space->granules_freed_by_last_collection;
  mark_space_yield <<= GRANULE_SIZE_LOG_2;
  size_t evacuation_block_yield =
    atomic_load_explicit(&mark_space->evacuation_targets.count,
                         memory_order_acquire) * BLOCK_SIZE;
  size_t minimum_evacuation_block_yield =
    heap->size * mark_space->evacuation_minimum_reserve;
  if (evacuation_block_yield < minimum_evacuation_block_yield)
    evacuation_block_yield = 0;
  else
    evacuation_block_yield -= minimum_evacuation_block_yield;
  struct large_object_space *lospace = heap_large_object_space(heap);
  size_t lospace_yield = lospace->pages_freed_by_last_collection;
  lospace_yield <<= lospace->page_size_log2;

  double yield = mark_space_yield + lospace_yield + evacuation_block_yield;
  return yield / heap->size;
}

static double heap_fragmentation(struct gc_heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  size_t fragmentation_granules =
    mark_space->fragmentation_granules_since_last_collection;
  size_t heap_granules = heap->size >> GRANULE_SIZE_LOG_2;

  return ((double)fragmentation_granules) / heap_granules;
}

static void detect_out_of_memory(struct gc_heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);

  if (heap->count == 0)
    return;

  double last_yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);

  double yield_epsilon = BLOCK_SIZE * 1.0 / heap->size;
  double fragmentation_epsilon = LARGE_OBJECT_THRESHOLD * 1.0 / BLOCK_SIZE;

  if (last_yield - fragmentation > yield_epsilon)
    return;

  if (fragmentation > fragmentation_epsilon
      && atomic_load(&mark_space->evacuation_targets.count))
    return;

  // No yield in last gc and we do not expect defragmentation to
  // be able to yield more space: out of memory.
  fprintf(stderr, "ran out of space, heap size %zu (%zu slabs)\n",
          heap->size, mark_space->nslabs);
  GC_CRASH();
}

static double clamp_major_gc_yield_threshold(struct gc_heap *heap,
                                             double threshold) {
  if (threshold < heap->minimum_major_gc_yield_threshold)
    threshold = heap->minimum_major_gc_yield_threshold;
  double one_block = BLOCK_SIZE * 1.0 / heap->size;
  if (threshold < one_block)
    threshold = one_block;
  return threshold;
}

static enum gc_kind determine_collection_kind(struct gc_heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  enum gc_kind previous_gc_kind = atomic_load(&heap->gc_kind);
  enum gc_kind gc_kind;
  int mark_while_stopping = 1;
  double yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);
  ssize_t pending = atomic_load_explicit(&mark_space->pending_unavailable_bytes,
                                         memory_order_acquire);

  if (heap->count == 0) {
    DEBUG("first collection is always major\n");
    gc_kind = GC_KIND_MAJOR_IN_PLACE;
  } else if (pending > 0) {
    DEBUG("evacuating due to need to reclaim %zd bytes\n", pending);
    // During the last cycle, a large allocation could not find enough
    // free blocks, and we decided not to expand the heap.  Let's do an
    // evacuating major collection to maximize the free block yield.
    gc_kind = GC_KIND_MAJOR_EVACUATING;

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
  } else if (previous_gc_kind == GC_KIND_MAJOR_EVACUATING
             && fragmentation >= heap->fragmentation_low_threshold) {
    DEBUG("continuing evacuation due to fragmentation %.2f%% > %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_low_threshold * 100.);
    // For some reason, we already decided to compact in the past,
    // and fragmentation hasn't yet fallen below a low-water-mark.
    // Keep going.
    gc_kind = GC_KIND_MAJOR_EVACUATING;
  } else if (fragmentation > heap->fragmentation_high_threshold) {
    // Switch to evacuation mode if the heap is too fragmented.
    DEBUG("triggering compaction due to fragmentation %.2f%% > %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_high_threshold * 100.);
    gc_kind = GC_KIND_MAJOR_EVACUATING;
  } else if (previous_gc_kind == GC_KIND_MAJOR_EVACUATING) {
    // We were evacuating, but we're good now.  Go back to minor
    // collections.
    DEBUG("returning to in-place collection, fragmentation %.2f%% < %.2f%%\n",
          fragmentation * 100.,
          heap->fragmentation_low_threshold * 100.);
    gc_kind = GC_KIND_MINOR_IN_PLACE;
  } else if (previous_gc_kind != GC_KIND_MINOR_IN_PLACE) {
    DEBUG("returning to minor collection after major collection\n");
    // Go back to minor collections.
    gc_kind = GC_KIND_MINOR_IN_PLACE;
  } else if (yield < heap->major_gc_yield_threshold) {
    DEBUG("collection yield too low, triggering major collection\n");
    // Nursery is getting tight; trigger a major GC.
    gc_kind = GC_KIND_MAJOR_IN_PLACE;
  } else {
    DEBUG("keeping on with minor GC\n");
    // Nursery has adequate space; keep trucking with minor GCs.
    GC_ASSERT(previous_gc_kind == GC_KIND_MINOR_IN_PLACE);
    gc_kind = GC_KIND_MINOR_IN_PLACE;
  }

  if (gc_has_conservative_intraheap_edges() &&
      (gc_kind & GC_KIND_FLAG_EVACUATING)) {
    DEBUG("welp.  conservative heap scanning, no evacuation for you\n");
    gc_kind = GC_KIND_MAJOR_IN_PLACE;
    mark_while_stopping = 1;
  }

  // If this is the first in a series of minor collections, reset the
  // threshold at which we should do a major GC.
  if ((gc_kind & GC_KIND_FLAG_MINOR) &&
      (previous_gc_kind & GC_KIND_FLAG_MINOR) != GC_KIND_FLAG_MINOR) {
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

static void release_evacuation_target_blocks(struct mark_space *space) {
  // Move excess evacuation target blocks back to empties.
  size_t total = space->nslabs * NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = atomic_load_explicit(&space->unavailable.count,
                                            memory_order_acquire);
  size_t reserve = space->evacuation_minimum_reserve * (total - unavailable);
  finish_evacuation_allocator(&space->evacuation_allocator,
                              &space->evacuation_targets, &space->empty,
                              reserve);
}

static void prepare_for_evacuation(struct gc_heap *heap) {
  struct mark_space *space = heap_mark_space(heap);

  if ((heap->gc_kind & GC_KIND_FLAG_EVACUATING) == 0) {
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

  size_t target_granules = target_blocks * GRANULES_PER_BLOCK;
  // Compute histogram where domain is the number of granules in a block
  // that survived the last collection, aggregated into 33 buckets, and
  // range is number of blocks in that bucket.  (Bucket 0 is for blocks
  // that were found to be completely empty; such blocks may be on the
  // evacuation target list.)
  const size_t bucket_count = 33;
  size_t histogram[33] = {0,};
  size_t bucket_size = GRANULES_PER_BLOCK / 32;
  size_t empties = 0;
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    for (size_t block = 0; block < NONMETA_BLOCKS_PER_SLAB; block++) {
      struct block_summary *summary = &space->slabs[slab].summaries[block];
      if (block_summary_has_flag(summary, BLOCK_UNAVAILABLE))
        continue;
      if (!block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP)) {
        empties++;
        continue;
      }
      size_t survivor_granules = GRANULES_PER_BLOCK - summary->free_granules;
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
    for (size_t block = 0; block < NONMETA_BLOCKS_PER_SLAB; block++) {
      struct block_summary *summary = &space->slabs[slab].summaries[block];
      if (block_summary_has_flag(summary, BLOCK_UNAVAILABLE))
        continue;
      if (!block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP))
        continue;
      size_t survivor_granules = GRANULES_PER_BLOCK - summary->free_granules;
      size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
      if (histogram[bucket]) {
        block_summary_set_flag(summary, BLOCK_EVACUATE);
        histogram[bucket]--;
      } else {
        block_summary_clear_flag(summary, BLOCK_EVACUATE);
      }
    }
  }

  // We are ready to evacuate!
  prepare_evacuation_allocator(&space->evacuation_allocator,
                               &space->evacuation_targets);
  space->evacuating = 1;
}

static void trace_conservative_roots_after_stop(struct gc_heap *heap) {
  GC_ASSERT(!heap_mark_space(heap)->evacuating);
  if (gc_has_mutator_conservative_roots())
    trace_mutator_conservative_roots_after_stop(heap);
  if (gc_has_global_conservative_roots())
    trace_global_conservative_roots(heap);
}

static void trace_pinned_roots_after_stop(struct gc_heap *heap) {
  GC_ASSERT(!heap_mark_space(heap)->evacuating);
  trace_conservative_roots_after_stop(heap);
}

static void trace_roots_after_stop(struct gc_heap *heap) {
  trace_mutator_roots_after_stop(heap);
  gc_trace_heap_roots(heap->roots, trace_and_enqueue_globally, heap, NULL);
  trace_generational_roots(heap);
}

static void mark_space_finish_gc(struct mark_space *space,
                                 enum gc_kind gc_kind) {
  space->evacuating = 0;
  reset_sweeper(space);
  update_mark_patterns(space, 0);
  reset_statistics(space);
  release_evacuation_target_blocks(space);
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
  return gc_pop_resolved_ephemerons(heap, trace_and_enqueue_globally,
                                    NULL);
}

static void sweep_ephemerons(struct gc_heap *heap) {
  return gc_sweep_pending_ephemerons(heap->pending_ephemerons, 0, 1);
}

static void collect(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  struct mark_space *space = heap_mark_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);
  struct gc_extern_space *exspace = heap_extern_space(heap);
  if (maybe_grow_heap(heap)) {
    DEBUG("grew heap instead of collecting #%ld:\n", heap->count);
    return;
  }
  DEBUG("start collect #%ld:\n", heap->count);
  enum gc_kind gc_kind = determine_collection_kind(heap);
  update_mark_patterns(space, !(gc_kind & GC_KIND_FLAG_MINOR));
  large_object_space_start_gc(lospace, gc_kind & GC_KIND_FLAG_MINOR);
  gc_extern_space_start_gc(exspace, gc_kind & GC_KIND_FLAG_MINOR);
  resolve_ephemerons_lazily(heap);
  tracer_prepare(heap);
  request_mutators_to_stop(heap);
  trace_mutator_roots_with_lock_before_stop(mut);
  finish_sweeping(mut);
  wait_for_mutators_to_stop(heap);
  double yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);
  fprintf(stderr, "last gc yield: %f; fragmentation: %f\n", yield, fragmentation);
  detect_out_of_memory(heap);
  trace_pinned_roots_after_stop(heap);
  prepare_for_evacuation(heap);
  trace_roots_after_stop(heap);
  tracer_trace(heap);
  resolve_ephemerons_eagerly(heap);
  while (enqueue_resolved_ephemerons(heap))
    tracer_trace(heap);
  sweep_ephemerons(heap);
  tracer_release(heap);
  mark_space_finish_gc(space, gc_kind);
  large_object_space_finish_gc(lospace, gc_kind & GC_KIND_FLAG_MINOR);
  gc_extern_space_finish_gc(exspace, gc_kind & GC_KIND_FLAG_MINOR);
  heap->count++;
  heap->last_collection_was_minor = gc_kind & GC_KIND_FLAG_MINOR;
  if (heap->last_collection_was_minor)
    heap->minor_count++;
  heap_reset_large_object_pages(heap, lospace->live_pages_at_last_collection);
  allow_mutators_to_continue(heap);
  DEBUG("collect done\n");
}

static int sweep_byte(uint8_t *loc, uintptr_t sweep_mask) {
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

static int sweep_word(uintptr_t *loc, uintptr_t sweep_mask) {
  uintptr_t metadata = atomic_load_explicit(loc, memory_order_relaxed);
  if (metadata) {
    if (metadata & sweep_mask)
      return 1;
    atomic_store_explicit(loc, 0, memory_order_relaxed);
  }
  return 0;
}

static uintptr_t mark_space_next_block_to_sweep(struct mark_space *space) {
  uintptr_t block = atomic_load_explicit(&space->next_block,
                                         memory_order_acquire);
  uintptr_t next_block;
  do {
    if (block == 0)
      return 0;

    next_block = block + BLOCK_SIZE;
    if (next_block % SLAB_SIZE == 0) {
      uintptr_t hi_addr = space->low_addr + space->extent;
      if (next_block == hi_addr)
        next_block = 0;
      else
        next_block += META_BLOCKS_PER_SLAB * BLOCK_SIZE;
    }
  } while (!atomic_compare_exchange_weak(&space->next_block, &block,
                                         next_block));
  return block;
}

static void finish_block(struct gc_mutator *mut) {
  GC_ASSERT(mut->block);
  struct block_summary *block = block_summary_for_addr(mut->block);
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  atomic_fetch_add(&space->granules_freed_by_last_collection,
                   block->free_granules);
  atomic_fetch_add(&space->fragmentation_granules_since_last_collection,
                   block->fragmentation_granules);

  // If this block has mostly survivors, we should avoid sweeping it and
  // trying to allocate into it for a minor GC.  Sweep it next time to
  // clear any garbage allocated in this cycle and mark it as
  // "venerable" (i.e., old).
  GC_ASSERT(!block_summary_has_flag(block, BLOCK_VENERABLE));
  if (!block_summary_has_flag(block, BLOCK_VENERABLE_AFTER_SWEEP) &&
      block->free_granules < GRANULES_PER_BLOCK * space->venerable_threshold)
    block_summary_set_flag(block, BLOCK_VENERABLE_AFTER_SWEEP);

  mut->block = mut->alloc = mut->sweep = 0;
}

// Sweep some heap to reclaim free space, resetting mut->alloc and
// mut->sweep.  Return the size of the hole in granules.
static size_t next_hole_in_block(struct gc_mutator *mut) {
  uintptr_t sweep = mut->sweep;
  if (sweep == 0)
    return 0;
  uintptr_t limit = mut->block + BLOCK_SIZE;
  uintptr_t sweep_mask = heap_mark_space(mutator_heap(mut))->sweep_mask;

  while (sweep != limit) {
    GC_ASSERT((sweep & (GRANULE_SIZE - 1)) == 0);
    uint8_t* metadata = metadata_byte_for_addr(sweep);
    size_t limit_granules = (limit - sweep) >> GRANULE_SIZE_LOG_2;

    // Except for when we first get a block, mut->sweep is positioned
    // right after a hole, which can point to either the end of the
    // block or to a live object.  Assume that a live object is more
    // common.
    {
      size_t live_granules = 0;
      while (limit_granules && (metadata[0] & sweep_mask)) {
        // Object survived collection; skip over it and continue sweeping.
        size_t object_granules = mark_space_live_object_granules(metadata);
        live_granules += object_granules;
        limit_granules -= object_granules;
        metadata += object_granules;
      }
      if (!limit_granules)
        break;
      sweep += live_granules * GRANULE_SIZE;
    }

    size_t free_granules = next_mark(metadata, limit_granules, sweep_mask);
    GC_ASSERT(free_granules);
    GC_ASSERT(free_granules <= limit_granules);

    struct block_summary *summary = block_summary_for_addr(sweep);
    summary->hole_count++;
    GC_ASSERT(free_granules <= GRANULES_PER_BLOCK - summary->free_granules);
    summary->free_granules += free_granules;

    size_t free_bytes = free_granules * GRANULE_SIZE;
    mut->alloc = sweep;
    mut->sweep = sweep + free_bytes;
    return free_granules;
  }

  finish_block(mut);
  return 0;
}

static void finish_hole(struct gc_mutator *mut) {
  size_t granules = (mut->sweep - mut->alloc) / GRANULE_SIZE;
  if (granules) {
    struct block_summary *summary = block_summary_for_addr(mut->block);
    summary->holes_with_fragmentation++;
    summary->fragmentation_granules += granules;
    uint8_t *metadata = metadata_byte_for_addr(mut->alloc);
    memset(metadata, 0, granules);
    mut->alloc = mut->sweep;
  }
  // FIXME: add to fragmentation
}

static int maybe_release_swept_empty_block(struct gc_mutator *mut) {
  GC_ASSERT(mut->block);
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  uintptr_t block = mut->block;
  if (atomic_load_explicit(&space->pending_unavailable_bytes,
                           memory_order_acquire) <= 0)
    return 0;

  push_unavailable_block(space, block);
  atomic_fetch_sub(&space->pending_unavailable_bytes, BLOCK_SIZE);
  mut->alloc = mut->sweep = mut->block = 0;
  return 1;
}

static size_t next_hole(struct gc_mutator *mut) {
  finish_hole(mut);
  // As we sweep if we find that a block is empty, we return it to the
  // empties list.  Empties are precious.  But if we return 10 blocks in
  // a row, and still find an 11th empty, go ahead and use it.
  size_t empties_countdown = 10;
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  while (1) {
    // Sweep current block for a hole.
    size_t granules = next_hole_in_block(mut);
    if (granules) {
      // If the hole spans only part of a block, give it to the mutator.
      if (granules < GRANULES_PER_BLOCK)
        return granules;
      struct block_summary *summary = block_summary_for_addr(mut->block);
      block_summary_clear_flag(summary, BLOCK_NEEDS_SWEEP);
      // Sweeping found a completely empty block.  If we are below the
      // minimum evacuation reserve, take the block.
      if (push_evacuation_target_if_needed(space, mut->block)) {
        mut->alloc = mut->sweep = mut->block = 0;
        continue;
      }
      // If we have pending pages to release to the OS, we should unmap
      // this block.
      if (maybe_release_swept_empty_block(mut))
        continue;
      // Otherwise if we've already returned lots of empty blocks to the
      // freelist, give this block to the mutator.
      if (!empties_countdown) {
        // After this block is allocated into, it will need to be swept.
        block_summary_set_flag(summary, BLOCK_NEEDS_SWEEP);
        return granules;
      }
      // Otherwise we push to the empty blocks list.
      push_empty_block(space, mut->block);
      mut->alloc = mut->sweep = mut->block = 0;
      empties_countdown--;
    }
    GC_ASSERT(mut->block == 0);
    while (1) {
      uintptr_t block = mark_space_next_block_to_sweep(space);
      if (block) {
        // Sweeping found a block.  We might take it for allocation, or
        // we might send it back.
        struct block_summary *summary = block_summary_for_addr(block);
        // If it's marked unavailable, it's already on a list of
        // unavailable blocks, so skip and get the next block.
        if (block_summary_has_flag(summary, BLOCK_UNAVAILABLE))
          continue;
        if (block_summary_has_flag(summary, BLOCK_VENERABLE)) {
          // Skip venerable blocks after a minor GC -- we don't need to
          // sweep as they weren't allocated into last cycle, and the
          // mark bytes didn't rotate, so we have no cleanup to do; and
          // we shouldn't try to allocate into them as it's not worth
          // it.  Any wasted space is measured as fragmentation.
          if (mutator_heap(mut)->last_collection_was_minor)
            continue;
          else
            block_summary_clear_flag(summary, BLOCK_VENERABLE);
        }
        if (block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP)) {
          // Prepare to sweep the block for holes.
          mut->alloc = mut->sweep = mut->block = block;
          if (block_summary_has_flag(summary, BLOCK_VENERABLE_AFTER_SWEEP)) {
            // In the last cycle we noted that this block consists of
            // mostly old data.  Sweep any garbage, commit the mark as
            // venerable, and avoid allocating into it.
            block_summary_clear_flag(summary, BLOCK_VENERABLE_AFTER_SWEEP);
            if (mutator_heap(mut)->last_collection_was_minor) {
              finish_sweeping_in_block(mut);
              block_summary_set_flag(summary, BLOCK_VENERABLE);
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
        block = pop_empty_block(space);
        // No empty block?  Return 0 to cause collection.
        if (!block)
          return 0;

        // Maybe we should use this empty as a target for evacuation.
        if (push_evacuation_target_if_possible(space, block))
          continue;

        // Otherwise return the block to the mutator.
        struct block_summary *summary = block_summary_for_addr(block);
        block_summary_set_flag(summary, BLOCK_NEEDS_SWEEP);
        summary->hole_count = 1;
        summary->free_granules = GRANULES_PER_BLOCK;
        summary->holes_with_fragmentation = 0;
        summary->fragmentation_granules = 0;
        mut->block = block;
        mut->alloc = block;
        mut->sweep = block + BLOCK_SIZE;
        return GRANULES_PER_BLOCK;
      }
    }
  }
}

static void finish_sweeping_in_block(struct gc_mutator *mut) {
  while (next_hole_in_block(mut))
    finish_hole(mut);
}

// Another thread is triggering GC.  Before we stop, finish clearing the
// dead mark bytes for the mutator's block, and release the block.
static void finish_sweeping(struct gc_mutator *mut) {
  while (next_hole(mut))
    finish_hole(mut);
}

static void trigger_collection(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  heap_lock(heap);
  if (mutators_are_stopping(heap))
    pause_mutator_for_collection_with_lock(mut);
  else
    collect(mut);
  heap_unlock(heap);
}

void gc_collect(struct gc_mutator *mut) {
  trigger_collection(mut);
}

static void* allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);

  size_t npages = large_object_space_npages(space, size);

  mark_space_request_release_memory(heap_mark_space(heap),
                                    npages << space->page_size_log2);

  while (!sweep_until_memory_released(mut))
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

  size = align_up(size, GRANULE_SIZE);
  uintptr_t alloc = mut->alloc;
  uintptr_t sweep = mut->sweep;
  uintptr_t new_alloc = alloc + size;
  struct gc_ref ret;
  if (new_alloc <= sweep) {
    mut->alloc = new_alloc;
    ret = gc_ref(alloc);
  } else {
    size_t granules = size >> GRANULE_SIZE_LOG_2;
    while (1) {
      size_t hole = next_hole(mut);
      if (hole >= granules) {
        clear_memory(mut->alloc, hole * GRANULE_SIZE);
        break;
      }
      if (!hole)
        trigger_collection(mut);
    }
    ret = gc_ref(mut->alloc);
    mut->alloc += size;
  }
  gc_update_alloc_table(mut, ret, size);
  return gc_ref_heap_object(ret);
}

void* gc_allocate_pointerless(struct gc_mutator *mut, size_t size) {
  return gc_allocate(mut, size);
}

struct gc_ref gc_allocate_ephemeron(struct gc_mutator *mut) {
  struct gc_ref ret =
    gc_ref_from_heap_object(gc_allocate(mut, gc_ephemeron_size()));
  if (gc_has_conservative_intraheap_edges()) {
    uint8_t *metadata = metadata_byte_for_addr(gc_ref_value(ret));
    *metadata |= METADATA_BYTE_EPHEMERON;
  }
  return ret;
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
  // No write barrier: we require that the ephemeron be newer than the
  // key or the value.
}

struct gc_pending_ephemerons *gc_heap_pending_ephemerons(struct gc_heap *heap) {
  return heap->pending_ephemerons;
}

unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return heap->count;
}

static struct slab* allocate_slabs(size_t nslabs) {
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

  return (struct slab*) aligned_base;
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

  if (!tracer_init(heap, options->common.parallelism))
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

  return 1;
}

static int mark_space_init(struct mark_space *space, struct gc_heap *heap) {
  size_t size = align_up(heap->size, SLAB_SIZE);
  size_t nslabs = size / SLAB_SIZE;
  struct slab *slabs = allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  space->marked_mask = METADATA_BYTE_MARK_0;
  update_mark_patterns(space, 0);
  space->slabs = slabs;
  space->nslabs = nslabs;
  space->low_addr = (uintptr_t) slabs;
  space->extent = size;
  space->next_block = 0;
  space->evacuation_minimum_reserve = 0.02;
  space->evacuation_reserve = space->evacuation_minimum_reserve;
  space->venerable_threshold = heap->fragmentation_low_threshold;
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t block = 0; block < NONMETA_BLOCKS_PER_SLAB; block++) {
      uintptr_t addr = (uintptr_t)slabs[slab].blocks[block].data;
      if (size > heap->size) {
        push_unavailable_block(space, addr);
        size -= BLOCK_SIZE;
      } else {
        if (!push_evacuation_target_if_needed(space, addr))
          push_empty_block(space, addr);
      }
    }
  }
  return 1;
}

int gc_init(const struct gc_options *options, struct gc_stack_addr *stack_base,
            struct gc_heap **heap, struct gc_mutator **mut) {
  GC_ASSERT_EQ(gc_allocator_small_granule_size(), GRANULE_SIZE);
  GC_ASSERT_EQ(gc_allocator_large_threshold(), LARGE_OBJECT_THRESHOLD);
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct gc_mutator, alloc));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct gc_mutator, sweep));
  GC_ASSERT_EQ(gc_allocator_alloc_table_alignment(), SLAB_SIZE);
  GC_ASSERT_EQ(gc_allocator_alloc_table_begin_pattern(), METADATA_BYTE_YOUNG);
  GC_ASSERT_EQ(gc_allocator_alloc_table_end_pattern(), METADATA_BYTE_END);
  if (GC_GENERATIONAL) {
    GC_ASSERT_EQ(gc_small_write_barrier_card_table_alignment(), SLAB_SIZE);
    GC_ASSERT_EQ(gc_small_write_barrier_card_size(),
                 BLOCK_SIZE / REMSET_BYTES_PER_BLOCK);
  }

  if (options->common.heap_size_policy != GC_HEAP_SIZE_FIXED) {
    fprintf(stderr, "fixed heap size is currently required\n");
    return 0;
  }

  *heap = calloc(1, sizeof(struct gc_heap));
  if (!*heap) GC_CRASH();

  if (!heap_init(*heap, options))
    GC_CRASH();

  struct mark_space *space = heap_mark_space(*heap);
  if (!mark_space_init(space, *heap)) {
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
  heap_lock(heap);
  mut->next = heap->deactivated_mutators;
  heap->deactivated_mutators = mut;
  heap->active_mutator_count--;
  gc_stack_capture_hot(&mut->stack);
  if (!heap->active_mutator_count && mutators_are_stopping(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void reactivate_mutator(struct gc_heap *heap, struct gc_mutator *mut) {
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  struct gc_mutator **prev = &heap->deactivated_mutators;
  while (*prev != mut)
    prev = &(*prev)->next;
  *prev = mut->next;
  mut->next = NULL;
  heap->active_mutator_count++;
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

void gc_print_stats(struct gc_heap *heap) {
  printf("Completed %ld collections (%ld major)\n",
         heap->count, heap->count - heap->minor_count);
  printf("Heap size with overhead is %zd (%zu slabs)\n",
         heap->size, heap_mark_space(heap)->nslabs);
}
