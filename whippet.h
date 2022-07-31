#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "inline.h"
#include "large-object-space.h"
#include "precise-roots.h"
#ifdef GC_PARALLEL_TRACE
#include "parallel-tracer.h"
#else
#include "serial-tracer.h"
#endif
#include "spin.h"

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

// Each granule has one metadata byte stored in a side table, used for
// mark bits but also for other per-object metadata.  Already we were
// using a byte instead of a bit to facilitate parallel marking.
// (Parallel markers are allowed to race.)  Turns out we can put a
// pinned bit there too, for objects that can't be moved (perhaps
// because they have been passed to unmanaged C code).  (Objects can
// also be temporarily pinned if they are referenced by a conservative
// root, but that doesn't need a separate bit; we can just use the mark
// bit.)  Then there's a "remembered" bit, indicating that the object
// should be scanned for references to the nursery.  If the remembered
// bit is set, the corresponding remset byte should also be set in the
// slab (see below).
//
// Getting back to mark bits -- because we want to allow for
// conservative roots, we need to know whether an address indicates an
// object or not.  That means that when an object is allocated, it has
// to set a bit, somewhere.  In our case we use the metadata byte, and
// set the "young" bit.  In future we could use this for generational
// GC, with the sticky mark bit strategy.
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
  METADATA_BYTE_PINNED = 32,
  METADATA_BYTE_REMEMBERED = 64,
  METADATA_BYTE_UNUSED = 128
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
  BLOCK_FLAG_UNUSED_6 = 0x40,
  BLOCK_FLAG_UNUSED_7 = 0x80,
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
  uint8_t remsets[REMSET_BYTES_PER_SLAB];
  uint8_t metadata[METADATA_BYTES_PER_SLAB];
  struct block blocks[NONMETA_BLOCKS_PER_SLAB];
};
STATIC_ASSERT_EQ(sizeof(struct slab), SLAB_SIZE);

static struct slab *object_slab(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = addr & ~(SLAB_SIZE - 1);
  return (struct slab*) base;
}

static uint8_t *object_metadata_byte(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = addr & ~(SLAB_SIZE - 1);
  uintptr_t granule = (addr & (SLAB_SIZE - 1)) >> GRANULE_SIZE_LOG_2;
  return (uint8_t*) (base + granule);
}

#define GRANULES_PER_BLOCK (BLOCK_SIZE / GRANULE_SIZE)
#define GRANULES_PER_REMSET_BYTE (GRANULES_PER_BLOCK / REMSET_BYTES_PER_BLOCK)
static uint8_t *object_remset_byte(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = addr & ~(SLAB_SIZE - 1);
  uintptr_t granule = (addr & (SLAB_SIZE - 1)) >> GRANULE_SIZE_LOG_2;
  uintptr_t remset_byte = granule / GRANULES_PER_REMSET_BYTE;
  return (uint8_t*) (base + remset_byte);
}

static struct block_summary* block_summary_for_addr(uintptr_t addr) {
  uintptr_t base = addr & ~(SLAB_SIZE - 1);
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
  return summary->next_and_flags & ~(BLOCK_SIZE - 1);
}
static void block_summary_set_next(struct block_summary *summary,
                                   uintptr_t next) {
  ASSERT((next & (BLOCK_SIZE - 1)) == 0);
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

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}

static inline size_t size_to_granules(size_t size) {
  return (size + GRANULE_SIZE - 1) >> GRANULE_SIZE_LOG_2;
}

// Alloc kind is in bits 1-7, for live objects.
static const uintptr_t gcobj_alloc_kind_mask = 0x7f;
static const uintptr_t gcobj_alloc_kind_shift = 1;
static const uintptr_t gcobj_forwarded_mask = 0x1;
static const uintptr_t gcobj_not_forwarded_bit = 0x1;
static inline uint8_t tag_live_alloc_kind(uintptr_t tag) {
  return (tag >> gcobj_alloc_kind_shift) & gcobj_alloc_kind_mask;
}
static inline uintptr_t tag_live(uint8_t alloc_kind) {
  return ((uintptr_t)alloc_kind << gcobj_alloc_kind_shift)
    | gcobj_not_forwarded_bit;
}
static inline uintptr_t tag_forwarded(struct gcobj *new_addr) {
  return (uintptr_t)new_addr;
}

struct gcobj {
  union {
    uintptr_t tag;
    uintptr_t words[0];
    void *pointers[0];
  };
};

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
  double evacuation_reserve;
  ssize_t pending_unavailable_bytes; // atomically
  struct evacuation_allocator evacuation_allocator;
  struct slab *slabs;
  size_t nslabs;
  uintptr_t granules_freed_by_last_collection; // atomically
  uintptr_t fragmentation_granules_since_last_collection; // atomically
};

enum gc_kind {
  GC_KIND_MARK_IN_PLACE,
  GC_KIND_COMPACT
};

struct heap {
  struct mark_space mark_space;
  struct large_object_space large_object_space;
  size_t large_object_pages;
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  size_t size;
  int collecting;
  enum gc_kind gc_kind;
  int multithreaded;
  size_t active_mutator_count;
  size_t mutator_count;
  struct handle *global_roots;
  struct mutator *mutator_trace_list;
  long count;
  struct mutator *deactivated_mutators;
  struct tracer tracer;
  double fragmentation_low_threshold;
  double fragmentation_high_threshold;
};

struct mutator_mark_buf {
  size_t size;
  size_t capacity;
  struct gcobj **objects;
};

struct mutator {
  // Bump-pointer allocation into holes.
  uintptr_t alloc;
  uintptr_t sweep;
  uintptr_t block;
  struct heap *heap;
  struct handle *roots;
  struct mutator_mark_buf mark_buf;
  // Three uses for this in-object linked-list pointer:
  //  - inactive (blocked in syscall) mutators
  //  - grey objects when stopping active mutators for mark-in-place
  //  - untraced mutators when stopping active mutators for evacuation
  struct mutator *next;
};

static inline struct tracer* heap_tracer(struct heap *heap) {
  return &heap->tracer;
}
static inline struct mark_space* heap_mark_space(struct heap *heap) {
  return &heap->mark_space;
}
static inline struct large_object_space* heap_large_object_space(struct heap *heap) {
  return &heap->large_object_space;
}
static inline struct heap* mutator_heap(struct mutator *mutator) {
  return mutator->heap;
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

enum gc_reason {
  GC_REASON_SMALL_ALLOCATION,
  GC_REASON_LARGE_ALLOCATION
};

static void collect(struct mutator *mut, enum gc_reason reason) NEVER_INLINE;

static inline uint8_t* mark_byte(struct mark_space *space, struct gcobj *obj) {
  return object_metadata_byte(obj);
}

static size_t mark_space_live_object_granules(uint8_t *metadata) {
  size_t n = 0;
  while ((metadata[n] & METADATA_BYTE_END) == 0)
    n++;
  return n + 1;
}

static inline int mark_space_mark_object(struct mark_space *space,
                                         struct gc_edge edge) {
  struct gcobj *obj = dereference_edge(edge);
  uint8_t *loc = object_metadata_byte(obj);
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
  ASSERT(allocated < (BLOCK_SIZE - 1) * (uint64_t) BLOCK_SIZE);
  return (block & ~(BLOCK_SIZE - 1)) | (allocated / BLOCK_SIZE);
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
  ASSERT((allocated & (GRANULE_SIZE - 1)) == 0);
  uintptr_t base = block + allocated;
  uintptr_t limit = block + BLOCK_SIZE;
  uintptr_t granules = (limit - base) >> GRANULE_SIZE_LOG_2;
  ASSERT(granules <= GRANULES_PER_BLOCK);
  memset(object_metadata_byte((void*)base), 0, granules);
}

static void finish_evacuation_allocator_block(uintptr_t block,
                                              uintptr_t allocated) {
  ASSERT(allocated <= BLOCK_SIZE);
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
                                        struct block_list *empties) {
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
    ASSERT(block);
    allocated -= BLOCK_SIZE;
  }
  if (allocated) {
    // Finish off the last partially-filled block.
    uintptr_t block = pop_block(targets);
    ASSERT(block);
    finish_evacuation_allocator_block(block, allocated);
  }
  while (1) {
    uintptr_t block = pop_block(targets);
    if (!block)
      break;
    push_block(empties, block);
  }
}

static struct gcobj *evacuation_allocate(struct mark_space *space,
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
      return NULL;
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
    ASSERT(base < next);
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
      ASSERT(!block);
      return NULL;
    }
    ASSERT(block);
    // This store can race with other allocators, but that's OK as long
    // as it never advances the cursor beyond the allocation pointer,
    // which it won't because we updated the allocation pointer already.
    atomic_store_explicit(&alloc->block_cursor,
                          make_evacuation_allocator_cursor(block, base),
                          memory_order_release);
  }

  uintptr_t addr = block + (next & block_mask) - bytes;
  return (struct gcobj*) addr;
}

static inline int mark_space_evacuate_or_mark_object(struct mark_space *space,
                                                     struct gc_edge edge) {
  struct gcobj *obj = dereference_edge(edge);
  uint8_t *metadata = object_metadata_byte(obj);
  uint8_t byte = *metadata;
  if (byte & space->marked_mask)
    return 0;
  if (space->evacuating &&
      block_summary_has_flag(block_summary_for_addr((uintptr_t)obj),
                             BLOCK_EVACUATE) &&
      ((byte & METADATA_BYTE_PINNED) == 0)) {
    // This is an evacuating collection, and we are attempting to
    // evacuate this block, and this particular object isn't pinned.
    // First, see if someone evacuated this object already.
    uintptr_t header_word = atomic_load_explicit(&obj->tag,
                                                 memory_order_relaxed);
    uintptr_t busy_header_word = 0;
    if (header_word != busy_header_word &&
        (header_word & gcobj_not_forwarded_bit) == 0) {
      // The object has been evacuated already.  Update the edge;
      // whoever forwarded the object will make sure it's eventually
      // traced.
      struct gcobj *forwarded = (struct gcobj*) header_word;
      update_edge(edge, forwarded);
      return 0;
    }
    // Otherwise try to claim it for evacuation.
    if (header_word != busy_header_word &&
        atomic_compare_exchange_strong(&obj->tag, &header_word,
                                       busy_header_word)) {
      // We claimed the object successfully; evacuating is up to us.
      size_t object_granules = mark_space_live_object_granules(metadata);
      struct gcobj *new_obj = evacuation_allocate(space, object_granules);
      if (new_obj) {
        // We were able to reserve space in which to evacuate this object.
        // Commit the evacuation by overwriting the tag.
        uintptr_t new_header_word = tag_forwarded(new_obj);
        atomic_store_explicit(&obj->tag, new_header_word,
                              memory_order_release);
        // Now copy the object contents, update extent metadata, and
        // indicate to the caller that the object's fields need to be
        // traced.
        new_obj->tag = header_word;
        memcpy(&new_obj->words[1], &obj->words[1],
               object_granules * GRANULE_SIZE - sizeof(header_word));
        uint8_t *new_metadata = object_metadata_byte(new_obj);
        memcpy(new_metadata + 1, metadata + 1, object_granules - 1);
        update_edge(edge, new_obj);
        obj = new_obj;
        metadata = new_metadata;
        // Fall through to set mark bits.
      } else {
        // Well shucks; allocation failed, marking the end of
        // opportunistic evacuation.  No future evacuation of this
        // object will succeed.  Restore the original header word and
        // mark instead.
        atomic_store_explicit(&obj->tag, header_word,
                              memory_order_release);
      }
    } else {
      // Someone else claimed this object first.  Spin until new address
      // known, or evacuation aborts.
      for (size_t spin_count = 0;; spin_count++) {
        header_word = atomic_load_explicit(&obj->tag, memory_order_acquire);
        if (header_word)
          break;
        yield_for_spin(spin_count);
      }
      if ((header_word & gcobj_not_forwarded_bit) == 0) {
        struct gcobj *forwarded = (struct gcobj*) header_word;
        update_edge(edge, forwarded);
      }
      // Either way, the other party is responsible for adding the
      // object to the mark queue.
      return 0;
    }
  }
  uint8_t mask = METADATA_BYTE_YOUNG | METADATA_BYTE_MARK_0
    | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  *metadata = (byte & ~mask) | space->marked_mask;
  return 1;
}

static inline int mark_space_contains(struct mark_space *space,
                                      struct gcobj *obj) {
  uintptr_t addr = (uintptr_t)obj;
  return addr - space->low_addr < space->extent;
}

static inline int large_object_space_mark_object(struct large_object_space *space,
                                                 struct gcobj *obj) {
  return large_object_space_copy(space, (uintptr_t)obj);
}

static inline int trace_edge(struct heap *heap, struct gc_edge edge) {
  struct gcobj *obj = dereference_edge(edge);
  if (!obj)
    return 0;
  else if (LIKELY(mark_space_contains(heap_mark_space(heap), obj))) {
    if (heap_mark_space(heap)->evacuating)
      return mark_space_evacuate_or_mark_object(heap_mark_space(heap), edge);
    return mark_space_mark_object(heap_mark_space(heap), edge);
  }
  else if (large_object_space_contains(heap_large_object_space(heap), obj))
    return large_object_space_mark_object(heap_large_object_space(heap),
                                          obj);
  else
    abort();
}

static inline void trace_one(struct gcobj *obj, void *mark_data) {
  switch (tag_live_alloc_kind(obj->tag)) {
#define SCAN_OBJECT(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      visit_##name##_fields((Name*)obj, tracer_visit, mark_data); \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    abort ();
  }
}

static int heap_has_multiple_mutators(struct heap *heap) {
  return atomic_load_explicit(&heap->multithreaded, memory_order_relaxed);
}

static int mutators_are_stopping(struct heap *heap) {
  return atomic_load_explicit(&heap->collecting, memory_order_relaxed);
}

static inline void heap_lock(struct heap *heap) {
  pthread_mutex_lock(&heap->lock);
}
static inline void heap_unlock(struct heap *heap) {
  pthread_mutex_unlock(&heap->lock);
}

static void add_mutator(struct heap *heap, struct mutator *mut) {
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

static void remove_mutator(struct heap *heap, struct mutator *mut) {
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

static void request_mutators_to_stop(struct heap *heap) {
  ASSERT(!mutators_are_stopping(heap));
  atomic_store_explicit(&heap->collecting, 1, memory_order_relaxed);
}

static void allow_mutators_to_continue(struct heap *heap) {
  ASSERT(mutators_are_stopping(heap));
  ASSERT(heap->active_mutator_count == 0);
  heap->active_mutator_count++;
  atomic_store_explicit(&heap->collecting, 0, memory_order_relaxed);
  ASSERT(!mutators_are_stopping(heap));
  pthread_cond_broadcast(&heap->mutator_cond);
}

static void push_unavailable_block(struct mark_space *space, uintptr_t block) {
  struct block_summary *summary = block_summary_for_addr(block);
  ASSERT(!block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP));
  ASSERT(!block_summary_has_flag(summary, BLOCK_UNAVAILABLE));
  block_summary_set_flag(summary, BLOCK_UNAVAILABLE);
  madvise((void*)block, BLOCK_SIZE, MADV_DONTNEED);
  push_block(&space->unavailable, block);
}

static uintptr_t pop_unavailable_block(struct mark_space *space) {
  uintptr_t block = pop_block(&space->unavailable);
  if (!block)
    return 0;
  struct block_summary *summary = block_summary_for_addr(block);
  ASSERT(block_summary_has_flag(summary, BLOCK_UNAVAILABLE));
  block_summary_clear_flag(summary, BLOCK_UNAVAILABLE);
  return block;
}

static uintptr_t pop_empty_block(struct mark_space *space) {
  return pop_block(&space->empty);
}

static void push_empty_block(struct mark_space *space, uintptr_t block) {
  ASSERT(!block_summary_has_flag(block_summary_for_addr(block),
                                 BLOCK_NEEDS_SWEEP));
  push_block(&space->empty, block);
}

static int maybe_push_evacuation_target(struct mark_space *space,
                                        uintptr_t block) {
  size_t targets = atomic_load_explicit(&space->evacuation_targets.count,
                                        memory_order_acquire);
  size_t total = space->nslabs * NONMETA_BLOCKS_PER_SLAB;
  size_t unavailable = atomic_load_explicit(&space->unavailable.count,
                                            memory_order_acquire);
  if (targets >= (total - unavailable) * space->evacuation_reserve)
    return 0;

  // We reached the end of the allocation cycle and just obtained a
  // known-empty block from the empties list.  If the last cycle was an
  // evacuating collection, put this block back on the list of
  // evacuation target blocks.
  push_block(&space->evacuation_targets, block);
  return 1;
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
    ASSERT(block);
    push_empty_block(space, block);
    pending = atomic_fetch_add(&space->pending_unavailable_bytes, BLOCK_SIZE)
      + BLOCK_SIZE;
  }
}

static size_t next_hole(struct mutator *mut);

static int sweep_until_memory_released(struct mutator *mut) {
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

static void heap_reset_large_object_pages(struct heap *heap, size_t npages) {
  size_t previous = heap->large_object_pages;
  heap->large_object_pages = npages;
  ASSERT(npages <= previous);
  size_t bytes = (previous - npages) <<
    heap_large_object_space(heap)->page_size_log2;
  mark_space_reacquire_memory(heap_mark_space(heap), bytes);
}

static void mutator_mark_buf_grow(struct mutator_mark_buf *buf) {
  size_t old_capacity = buf->capacity;
  size_t old_bytes = old_capacity * sizeof(struct gcobj*);

  size_t new_bytes = old_bytes ? old_bytes * 2 : getpagesize();
  size_t new_capacity = new_bytes / sizeof(struct gcobj*);

  void *mem = mmap(NULL, new_bytes, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("allocating mutator mark buffer failed");
    abort();
  }
  if (old_bytes) {
    memcpy(mem, buf->objects, old_bytes);
    munmap(buf->objects, old_bytes);
  }
  buf->objects = mem;
  buf->capacity = new_capacity;
}

static void mutator_mark_buf_push(struct mutator_mark_buf *buf,
                                  struct gcobj *val) {
  if (UNLIKELY(buf->size == buf->capacity))
    mutator_mark_buf_grow(buf);
  buf->objects[buf->size++] = val;
}

static void mutator_mark_buf_release(struct mutator_mark_buf *buf) {
  size_t bytes = buf->size * sizeof(struct gcobj*);
  if (bytes >= getpagesize())
    madvise(buf->objects, align_up(bytes, getpagesize()), MADV_DONTNEED);
  buf->size = 0;
}

static void mutator_mark_buf_destroy(struct mutator_mark_buf *buf) {
  size_t bytes = buf->capacity * sizeof(struct gcobj*);
  if (bytes)
    munmap(buf->objects, bytes);
}

static void enqueue_mutator_for_tracing(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  ASSERT(mut->next == NULL);
  struct mutator *next =
    atomic_load_explicit(&heap->mutator_trace_list, memory_order_acquire);
  do {
    mut->next = next;
  } while (!atomic_compare_exchange_weak(&heap->mutator_trace_list,
                                         &next, mut));
}

static int heap_should_mark_while_stopping(struct heap *heap) {
  return atomic_load(&heap->gc_kind) == GC_KIND_MARK_IN_PLACE;
}

static int mutator_should_mark_while_stopping(struct mutator *mut) {
  // If we are marking in place, we allow mutators to mark their own
  // stacks before pausing.  This is a limited form of concurrent
  // marking, as other mutators might be running, not having received
  // the signal to stop yet.  We can't do this for a compacting
  // collection, however, as that would become concurrent evacuation,
  // which is a different kettle of fish.
  return heap_should_mark_while_stopping(mutator_heap(mut));
}

// Mark the roots of a mutator that is stopping for GC.  We can't
// enqueue them directly, so we send them to the controller in a buffer.
static void mark_stopping_mutator_roots(struct mutator *mut) {
  ASSERT(mutator_should_mark_while_stopping(mut));
  struct heap *heap = mutator_heap(mut);
  struct mutator_mark_buf *local_roots = &mut->mark_buf;
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gc_edge root = gc_edge(&h->v);
    if (trace_edge(heap, root))
      mutator_mark_buf_push(local_roots, dereference_edge(root));
  }
}

// Precondition: the caller holds the heap lock.
static void mark_mutator_roots_with_lock(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gc_edge root = gc_edge(&h->v);
    if (trace_edge(heap, root))
      tracer_enqueue_root(&heap->tracer, dereference_edge(root));
  }
}

static void trace_mutator_roots_with_lock(struct mutator *mut) {
  mark_mutator_roots_with_lock(mut);
}

static void trace_mutator_roots_with_lock_before_stop(struct mutator *mut) {
  if (mutator_should_mark_while_stopping(mut))
    mark_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
}

static void release_stopping_mutator_roots(struct mutator *mut) {
  mutator_mark_buf_release(&mut->mark_buf);
}

static void wait_for_mutators_to_stop(struct heap *heap) {
  heap->active_mutator_count--;
  while (heap->active_mutator_count)
    pthread_cond_wait(&heap->collector_cond, &heap->lock);
}

static void finish_sweeping(struct mutator *mut);
static void finish_sweeping_in_block(struct mutator *mut);

static void trace_mutator_roots_after_stop(struct heap *heap) {
  struct mutator *mut = atomic_load(&heap->mutator_trace_list);
  int active_mutators_already_marked = heap_should_mark_while_stopping(heap);
  while (mut) {
    if (active_mutators_already_marked)
      tracer_enqueue_roots(&heap->tracer,
                           mut->mark_buf.objects, mut->mark_buf.size);
    else
      trace_mutator_roots_with_lock(mut);
    struct mutator *next = mut->next;
    mut->next = NULL;
    mut = next;
  }
  atomic_store(&heap->mutator_trace_list, NULL);

  for (struct mutator *mut = heap->deactivated_mutators; mut; mut = mut->next) {
    finish_sweeping_in_block(mut);
    trace_mutator_roots_with_lock(mut);
  }
}

static void trace_global_roots(struct heap *heap) {
  for (struct handle *h = heap->global_roots; h; h = h->next) {
    struct gc_edge edge = gc_edge(&h->v);
    if (trace_edge(heap, edge))
      tracer_enqueue_root(&heap->tracer, dereference_edge(edge));
  }
}

static void pause_mutator_for_collection(struct heap *heap) NEVER_INLINE;
static void pause_mutator_for_collection(struct heap *heap) {
  ASSERT(mutators_are_stopping(heap));
  ASSERT(heap->active_mutator_count);
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

static void pause_mutator_for_collection_with_lock(struct mutator *mut) NEVER_INLINE;
static void pause_mutator_for_collection_with_lock(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  ASSERT(mutators_are_stopping(heap));
  finish_sweeping_in_block(mut);
  if (mutator_should_mark_while_stopping(mut))
    // No need to collect results in mark buf; we can enqueue roots directly.
    mark_mutator_roots_with_lock(mut);
  else
    enqueue_mutator_for_tracing(mut);
  pause_mutator_for_collection(heap);
}

static void pause_mutator_for_collection_without_lock(struct mutator *mut) NEVER_INLINE;
static void pause_mutator_for_collection_without_lock(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  ASSERT(mutators_are_stopping(heap));
  finish_sweeping(mut);
  if (mutator_should_mark_while_stopping(mut))
    mark_stopping_mutator_roots(mut);
  enqueue_mutator_for_tracing(mut);
  heap_lock(heap);
  pause_mutator_for_collection(heap);
  heap_unlock(heap);
  release_stopping_mutator_roots(mut);
}

static inline void maybe_pause_mutator_for_collection(struct mutator *mut) {
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static void reset_sweeper(struct mark_space *space) {
  space->next_block = (uintptr_t) &space->slabs[0].blocks;
}

static uint64_t broadcast_byte(uint8_t byte) {
  uint64_t result = byte;
  return result * 0x0101010101010101ULL;
}

static void rotate_mark_bytes(struct mark_space *space) {
  space->live_mask = rotate_dead_survivor_marked(space->live_mask);
  space->marked_mask = rotate_dead_survivor_marked(space->marked_mask);
  space->sweep_mask = broadcast_byte(space->live_mask);
}

static void reset_statistics(struct mark_space *space) {
  space->granules_freed_by_last_collection = 0;
  space->fragmentation_granules_since_last_collection = 0;
}

static int maybe_grow_heap(struct heap *heap, enum gc_reason reason) {
  return 0;
}

static double heap_last_gc_yield(struct heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  size_t mark_space_yield = mark_space->granules_freed_by_last_collection;
  mark_space_yield <<= GRANULE_SIZE_LOG_2;
  struct large_object_space *lospace = heap_large_object_space(heap);
  size_t lospace_yield = lospace->pages_freed_by_last_collection;
  lospace_yield <<= lospace->page_size_log2;

  double yield = mark_space_yield + lospace_yield;
  return yield / heap->size;
}

static double heap_fragmentation(struct heap *heap) {
  struct mark_space *mark_space = heap_mark_space(heap);
  size_t mark_space_blocks = mark_space->nslabs * NONMETA_BLOCKS_PER_SLAB;
  mark_space_blocks -= atomic_load(&mark_space->unavailable.count);
  size_t mark_space_granules = mark_space_blocks * GRANULES_PER_BLOCK;
  size_t fragmentation_granules =
    mark_space->fragmentation_granules_since_last_collection;

  struct large_object_space *lospace = heap_large_object_space(heap);
  size_t lospace_pages = lospace->total_pages - lospace->free_pages;
  size_t lospace_granules =
    lospace_pages << (lospace->page_size_log2 - GRANULE_SIZE_LOG_2);

  size_t heap_granules = mark_space_granules + lospace_granules;

  return ((double)fragmentation_granules) / heap_granules;
}

static void determine_collection_kind(struct heap *heap,
                                      enum gc_reason reason) {
  switch (reason) {
    case GC_REASON_LARGE_ALLOCATION:
      // We are collecting because a large allocation could not find
      // enough free blocks, and we decided not to expand the heap.
      // Let's evacuate to maximize the free block yield.
      heap->gc_kind = GC_KIND_COMPACT;
      break;
    case GC_REASON_SMALL_ALLOCATION: {
      // We are making a small allocation and ran out of blocks.
      // Evacuate if the heap is "too fragmented", where fragmentation
      // is measured as a percentage of granules that couldn't be used
      // for allocations in the last cycle.
      double fragmentation = heap_fragmentation(heap);
      if (atomic_load(&heap->gc_kind) == GC_KIND_COMPACT) {
        // For some reason, we already decided to compact in the past.
        // Keep going until we measure that wasted space due to
        // fragmentation is below a low-water-mark.
        if (fragmentation < heap->fragmentation_low_threshold) {
          DEBUG("returning to in-place collection, fragmentation %.2f%% < %.2f%%\n",
                fragmentation * 100.,
                heap->fragmentation_low_threshold * 100.);
          atomic_store(&heap->gc_kind, GC_KIND_MARK_IN_PLACE);
        }
      } else {
        // Otherwise switch to evacuation mode if the heap is too
        // fragmented.
        if (fragmentation > heap->fragmentation_high_threshold) {
          DEBUG("triggering compaction due to fragmentation %.2f%% > %.2f%%\n",
                fragmentation * 100.,
                heap->fragmentation_high_threshold * 100.);
          atomic_store(&heap->gc_kind, GC_KIND_COMPACT);
        }
      }
      break;
    }
  }
}

static void release_evacuation_target_blocks(struct mark_space *space) {
  // Move any collected evacuation target blocks back to empties.
  finish_evacuation_allocator(&space->evacuation_allocator,
                              &space->evacuation_targets, &space->empty);
}

static void prepare_for_evacuation(struct heap *heap) {
  struct mark_space *space = heap_mark_space(heap);

  if (heap->gc_kind == GC_KIND_MARK_IN_PLACE) {
    space->evacuating = 0;
    space->evacuation_reserve = 0.02;
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
  for (size_t slab = 0; slab < space->nslabs; slab++) {
    for (size_t block = 0; block < NONMETA_BLOCKS_PER_SLAB; block++) {
      struct block_summary *summary = &space->slabs[slab].summaries[block];
      if (block_summary_has_flag(summary, BLOCK_UNAVAILABLE))
        continue;
      size_t survivor_granules = GRANULES_PER_BLOCK - summary->free_granules;
      size_t bucket = (survivor_granules + bucket_size - 1) / bucket_size;
      histogram[bucket]++;
    }
  }

  // Evacuation targets must be in bucket 0.  These blocks will later be
  // marked also as evacuation candidates, but that's not a problem,
  // because they contain no source objects.
  ASSERT(histogram[0] >= target_blocks);

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

static void trace_conservative_roots_after_stop(struct heap *heap) {
  // FIXME: Visit conservative roots, if the collector is configured in
  // that way.  Mark them in place, preventing any subsequent
  // evacuation.
}

static void trace_precise_roots_after_stop(struct heap *heap) {
  trace_mutator_roots_after_stop(heap);
  trace_global_roots(heap);
}

static void mark_space_finish_gc(struct mark_space *space) {
  space->evacuating = 0;
  reset_sweeper(space);
  rotate_mark_bytes(space);
  reset_statistics(space);
  release_evacuation_target_blocks(space);
}

static void collect(struct mutator *mut, enum gc_reason reason) {
  struct heap *heap = mutator_heap(mut);
  struct mark_space *space = heap_mark_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);
  if (maybe_grow_heap(heap, reason)) {
    DEBUG("grew heap instead of collecting #%ld:\n", heap->count);
    return;
  }
  DEBUG("start collect #%ld:\n", heap->count);
  determine_collection_kind(heap, reason);
  large_object_space_start_gc(lospace);
  tracer_prepare(heap);
  request_mutators_to_stop(heap);
  trace_mutator_roots_with_lock_before_stop(mut);
  finish_sweeping(mut);
  wait_for_mutators_to_stop(heap);
  double yield = heap_last_gc_yield(heap);
  double fragmentation = heap_fragmentation(heap);
  fprintf(stderr, "last gc yield: %f; fragmentation: %f\n", yield, fragmentation);
  trace_conservative_roots_after_stop(heap);
  prepare_for_evacuation(heap);
  trace_precise_roots_after_stop(heap);
  tracer_trace(heap);
  tracer_release(heap);
  mark_space_finish_gc(space);
  large_object_space_finish_gc(lospace);
  heap->count++;
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

static inline uint64_t load_mark_bytes(uint8_t *mark) {
  ASSERT(((uintptr_t)mark & 7) == 0);
  uint8_t * __attribute__((aligned(8))) aligned_mark = mark;
  uint64_t word;
  memcpy(&word, aligned_mark, 8);
#ifdef WORDS_BIGENDIAN
  word = __builtin_bswap64(word);
#endif
  return word;
}

static inline size_t count_zero_bytes(uint64_t bytes) {
  return bytes ? (__builtin_ctz(bytes) / 8) : sizeof(bytes);
}

static size_t next_mark(uint8_t *mark, size_t limit, uint64_t sweep_mask) {
  size_t n = 0;
  // If we have a hole, it is likely to be more that 8 granules long.
  // Assuming that it's better to make aligned loads, first we align the
  // sweep pointer, then we load aligned mark words.
  size_t unaligned = ((uintptr_t) mark) & 7;
  if (unaligned) {
    uint64_t bytes = load_mark_bytes(mark - unaligned) >> (unaligned * 8);
    bytes &= sweep_mask;
    if (bytes)
      return count_zero_bytes(bytes);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_mark_bytes(mark + n);
    bytes &= sweep_mask;
    if (bytes)
      return n + count_zero_bytes(bytes);
  }

  return limit;
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

static void finish_block(struct mutator *mut) {
  ASSERT(mut->block);
  struct block_summary *block = block_summary_for_addr(mut->block);
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  atomic_fetch_add(&space->granules_freed_by_last_collection,
                   block->free_granules);
  atomic_fetch_add(&space->fragmentation_granules_since_last_collection,
                   block->fragmentation_granules);

  mut->block = mut->alloc = mut->sweep = 0;
}

// Sweep some heap to reclaim free space, resetting mut->alloc and
// mut->sweep.  Return the size of the hole in granules.
static size_t next_hole_in_block(struct mutator *mut) {
  uintptr_t sweep = mut->sweep;
  if (sweep == 0)
    return 0;
  uintptr_t limit = mut->block + BLOCK_SIZE;
  uintptr_t sweep_mask = heap_mark_space(mutator_heap(mut))->sweep_mask;

  while (sweep != limit) {
    ASSERT((sweep & (GRANULE_SIZE - 1)) == 0);
    uint8_t* metadata = object_metadata_byte((struct gcobj*)sweep);
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
    ASSERT(free_granules);
    ASSERT(free_granules <= limit_granules);

    struct block_summary *summary = block_summary_for_addr(sweep);
    summary->hole_count++;
    summary->free_granules += free_granules;

    size_t free_bytes = free_granules * GRANULE_SIZE;
    mut->alloc = sweep;
    mut->sweep = sweep + free_bytes;
    return free_granules;
  }

  finish_block(mut);
  return 0;
}

static void finish_hole(struct mutator *mut) {
  size_t granules = (mut->sweep - mut->alloc) / GRANULE_SIZE;
  if (granules) {
    struct block_summary *summary = block_summary_for_addr(mut->block);
    summary->holes_with_fragmentation++;
    summary->fragmentation_granules += granules;
    uint8_t *metadata = object_metadata_byte((void*)mut->alloc);
    memset(metadata, 0, granules);
    mut->alloc = mut->sweep;
  }
  // FIXME: add to fragmentation
}

static int maybe_release_swept_empty_block(struct mutator *mut) {
  ASSERT(mut->block);
  struct mark_space *space = heap_mark_space(mutator_heap(mut));
  uintptr_t block = mut->block;
  if (atomic_load_explicit(&space->pending_unavailable_bytes,
                           memory_order_acquire) <= 0)
    return 0;

  block_summary_clear_flag(block_summary_for_addr(block), BLOCK_NEEDS_SWEEP);
  push_unavailable_block(space, block);
  atomic_fetch_sub(&space->pending_unavailable_bytes, BLOCK_SIZE);
  mut->alloc = mut->sweep = mut->block = 0;
  return 1;
}

static size_t next_hole(struct mutator *mut) {
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
      // Sweeping found a completely empty block.  If we have pending
      // pages to release to the OS, we should unmap this block.
      if (maybe_release_swept_empty_block(mut))
        continue;
      // Otherwise if we've already returned lots of empty blocks to the
      // freelist, give this block to the mutator.
      if (!empties_countdown)
        return granules;
      // Otherwise we push to the empty blocks list.
      struct block_summary *summary = block_summary_for_addr(mut->block);
      block_summary_clear_flag(summary, BLOCK_NEEDS_SWEEP);
      push_empty_block(space, mut->block);
      mut->alloc = mut->sweep = mut->block = 0;
      empties_countdown--;
    }
    ASSERT(mut->block == 0);
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
        if (block_summary_has_flag(summary, BLOCK_NEEDS_SWEEP)) {
          // This block was marked in the last GC and needs sweeping.
          // As we sweep we'll want to record how many bytes were live
          // at the last collection.  As we allocate we'll record how
          // many granules were wasted because of fragmentation.
          summary->hole_count = 0;
          summary->free_granules = 0;
          summary->holes_with_fragmentation = 0;
          summary->fragmentation_granules = 0;
          // Prepare to sweep the block for holes.
          mut->alloc = mut->sweep = mut->block = block;
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
        if (maybe_push_evacuation_target(space, block))
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

static void finish_sweeping_in_block(struct mutator *mut) {
  while (next_hole_in_block(mut))
    finish_hole(mut);
}

// Another thread is triggering GC.  Before we stop, finish clearing the
// dead mark bytes for the mutator's block, and release the block.
static void finish_sweeping(struct mutator *mut) {
  while (next_hole(mut))
    finish_hole(mut);
}

static void out_of_memory(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  fprintf(stderr, "ran out of space, heap size %zu (%zu slabs)\n",
          heap->size, heap_mark_space(heap)->nslabs);
  abort();
}

static void* allocate_large(struct mutator *mut, enum alloc_kind kind,
                            size_t granules) {
  struct heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);

  size_t size = granules * GRANULE_SIZE;
  size_t npages = large_object_space_npages(space, size);

  mark_space_request_release_memory(heap_mark_space(heap),
                                    npages << space->page_size_log2);
  if (!sweep_until_memory_released(mut)) {
    heap_lock(heap);
    if (mutators_are_stopping(heap))
      pause_mutator_for_collection_with_lock(mut);
    else
      collect(mut, GC_REASON_LARGE_ALLOCATION);
    heap_unlock(heap);
    if (!sweep_until_memory_released(mut))
      out_of_memory(mut);
  }
  atomic_fetch_add(&heap->large_object_pages, npages);

  void *ret = large_object_space_alloc(space, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(space, npages);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    abort();
  }

  *(uintptr_t*)ret = tag_live(kind);
  return ret;
}

static void* allocate_small_slow(struct mutator *mut, enum alloc_kind kind,
                                 size_t granules) NEVER_INLINE;
static void* allocate_small_slow(struct mutator *mut, enum alloc_kind kind,
                                 size_t granules) {
  int swept_from_beginning = 0;
  while (1) {
    size_t hole = next_hole(mut);
    if (hole >= granules) {
      clear_memory(mut->alloc, hole * GRANULE_SIZE);
      break;
    }
    if (!hole) {
      struct heap *heap = mutator_heap(mut);
      if (swept_from_beginning) {
        out_of_memory(mut);
      } else {
        heap_lock(heap);
        if (mutators_are_stopping(heap))
          pause_mutator_for_collection_with_lock(mut);
        else
          collect(mut, GC_REASON_SMALL_ALLOCATION);
        heap_unlock(heap);
        swept_from_beginning = 1;
      }
    }
  }
  struct gcobj* ret = (struct gcobj*)mut->alloc;
  mut->alloc += granules * GRANULE_SIZE;
  return ret;
}

static inline void* allocate_small(struct mutator *mut, enum alloc_kind kind,
                                   size_t granules) {
  ASSERT(granules > 0); // allocating 0 granules would be silly
  uintptr_t alloc = mut->alloc;
  uintptr_t sweep = mut->sweep;
  uintptr_t new_alloc = alloc + granules * GRANULE_SIZE;
  struct gcobj *obj;
  if (new_alloc <= sweep) {
    mut->alloc = new_alloc;
    obj = (struct gcobj *)alloc;
  } else {
    obj = allocate_small_slow(mut, kind, granules);
  }
  obj->tag = tag_live(kind);
  uint8_t *metadata = object_metadata_byte(obj);
  if (granules == 1) {
    metadata[0] = METADATA_BYTE_YOUNG | METADATA_BYTE_END;
  } else {
    metadata[0] = METADATA_BYTE_YOUNG;
    if (granules > 2)
      memset(metadata + 1, 0, granules - 2);
    metadata[granules - 1] = METADATA_BYTE_END;
  }
  return obj;
}

static inline void* allocate_medium(struct mutator *mut, enum alloc_kind kind,
                                    size_t granules) {
  return allocate_small(mut, kind, granules);
}

static inline void* allocate(struct mutator *mut, enum alloc_kind kind,
                             size_t size) {
  size_t granules = size_to_granules(size);
  if (granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD)
    return allocate_small(mut, kind, granules);
  if (granules <= LARGE_OBJECT_GRANULE_THRESHOLD)
    return allocate_medium(mut, kind, granules);
  return allocate_large(mut, kind, granules);
}
static inline void* allocate_pointerless(struct mutator *mut,
                                         enum alloc_kind kind,
                                         size_t size) {
  return allocate(mut, kind, size);
}

static inline void init_field(void *obj, void **addr, void *val) {
  *addr = val;
}
static inline void set_field(void *obj, void **addr, void *val) {
  *addr = val;
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

static int heap_init(struct heap *heap, size_t size) {
  // *heap is already initialized to 0.

  pthread_mutex_init(&heap->lock, NULL);
  pthread_cond_init(&heap->mutator_cond, NULL);
  pthread_cond_init(&heap->collector_cond, NULL);
  heap->size = size;

  if (!tracer_init(heap))
    abort();

  heap->fragmentation_low_threshold = 0.05;
  heap->fragmentation_high_threshold = 0.10;

  return 1;
}

static int mark_space_init(struct mark_space *space, struct heap *heap) {
  size_t size = align_up(heap->size, SLAB_SIZE);
  size_t nslabs = size / SLAB_SIZE;
  struct slab *slabs = allocate_slabs(nslabs);
  if (!slabs)
    return 0;

  uint8_t dead = METADATA_BYTE_MARK_0;
  uint8_t survived = METADATA_BYTE_MARK_1;
  uint8_t marked = METADATA_BYTE_MARK_2;
  space->marked_mask = marked;
  space->live_mask = survived | marked;
  rotate_mark_bytes(space);
  space->slabs = slabs;
  space->nslabs = nslabs;
  space->low_addr = (uintptr_t) slabs;
  space->extent = size;
  space->next_block = 0;
  space->evacuation_reserve = 0.02;
  for (size_t slab = 0; slab < nslabs; slab++) {
    for (size_t block = 0; block < NONMETA_BLOCKS_PER_SLAB; block++) {
      uintptr_t addr = (uintptr_t)slabs[slab].blocks[block].data;
      if (size > heap->size) {
        push_unavailable_block(space, addr);
        size -= BLOCK_SIZE;
      } else {
        push_empty_block(space, addr);
      }
    }
  }
  return 1;
}

static int initialize_gc(size_t size, struct heap **heap,
                         struct mutator **mut) {
  *heap = calloc(1, sizeof(struct heap));
  if (!*heap) abort();

  if (!heap_init(*heap, size))
    abort();

  struct mark_space *space = heap_mark_space(*heap);
  if (!mark_space_init(space, *heap)) {
    free(*heap);
    *heap = NULL;
    return 0;
  }
  
  if (!large_object_space_init(heap_large_object_space(*heap), *heap))
    abort();

  *mut = calloc(1, sizeof(struct mutator));
  if (!*mut) abort();
  add_mutator(*heap, *mut);
  return 1;
}

static struct mutator* initialize_gc_for_thread(uintptr_t *stack_base,
                                                struct heap *heap) {
  struct mutator *ret = calloc(1, sizeof(struct mutator));
  if (!ret)
    abort();
  add_mutator(heap, ret);
  return ret;
}

static void finish_gc_for_thread(struct mutator *mut) {
  remove_mutator(mutator_heap(mut), mut);
  mutator_mark_buf_destroy(&mut->mark_buf);
  free(mut);
}

static void deactivate_mutator(struct heap *heap, struct mutator *mut) {
  ASSERT(mut->next == NULL);
  heap_lock(heap);
  mut->next = heap->deactivated_mutators;
  heap->deactivated_mutators = mut;
  heap->active_mutator_count--;
  if (!heap->active_mutator_count && mutators_are_stopping(heap))
    pthread_cond_signal(&heap->collector_cond);
  heap_unlock(heap);
}

static void reactivate_mutator(struct heap *heap, struct mutator *mut) {
  heap_lock(heap);
  while (mutators_are_stopping(heap))
    pthread_cond_wait(&heap->mutator_cond, &heap->lock);
  struct mutator **prev = &heap->deactivated_mutators;
  while (*prev != mut)
    prev = &(*prev)->next;
  *prev = mut->next;
  mut->next = NULL;
  heap->active_mutator_count++;
  heap_unlock(heap);
}

static void* call_without_gc(struct mutator *mut, void* (*f)(void*),
                             void *data) NEVER_INLINE;
static void* call_without_gc(struct mutator *mut,
                             void* (*f)(void*),
                             void *data) {
  struct heap *heap = mutator_heap(mut);
  deactivate_mutator(heap, mut);
  void *ret = f(data);
  reactivate_mutator(heap, mut);
  return ret;
}

static inline void print_start_gc_stats(struct heap *heap) {
}

static inline void print_end_gc_stats(struct heap *heap) {
  printf("Completed %ld collections\n", heap->count);
  printf("Heap size with overhead is %zd (%zu slabs)\n",
         heap->size, heap_mark_space(heap)->nslabs);
}
