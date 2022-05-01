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
#ifdef GC_PARALLEL_MARK
#include "parallel-tracer.h"
#else
#include "serial-tracer.h"
#endif

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
// pinned bit there too, for objects that can't be moved.  Actually
// there are two pinned bits: one that's managed by the collector, which
// pins referents of conservative roots, and one for pins managed
// externally (maybe because the mutator requested a pin.)  Then there's
// a "remembered" bit, indicating that the object should be scanned for
// references to the nursery.  If the remembered bit is set, the
// corresponding remset byte should also be set in the slab (see below).
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
  METADATA_BYTE_PERMAPINNED = 64,
  METADATA_BYTE_REMEMBERED = 128
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

struct block_summary {
  union {
    struct {
      uint16_t wasted_granules;
      uint16_t wasted_spans;
      uint8_t out_for_thread;
      uint8_t has_pin;
      uint8_t paged_out;
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

static struct block_summary* object_block_summary(void *obj) {
  uintptr_t addr = (uintptr_t) obj;
  uintptr_t base = addr & ~(SLAB_SIZE - 1);
  uintptr_t block = (addr & (SLAB_SIZE - 1)) / BLOCK_SIZE;
  return (struct block_summary*) (base + block * sizeof(struct block_summary));
}

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}

static inline size_t size_to_granules(size_t size) {
  return (size + GRANULE_SIZE - 1) >> GRANULE_SIZE_LOG_2;
}

// Alloc kind is in bits 0-7, for live objects.
static const uintptr_t gcobj_alloc_kind_mask = 0xff;
static const uintptr_t gcobj_alloc_kind_shift = 0;
static inline uint8_t tag_live_alloc_kind(uintptr_t tag) {
  return (tag >> gcobj_alloc_kind_shift) & gcobj_alloc_kind_mask;
}
static inline uintptr_t tag_live(uint8_t alloc_kind) {
  return ((uintptr_t)alloc_kind << gcobj_alloc_kind_shift);
}

struct gcobj_free {
  struct gcobj_free *next;
};

// Objects larger than MEDIUM_OBJECT_GRANULE_THRESHOLD.
struct gcobj_free_medium {
  struct gcobj_free_medium *next;
  size_t granules;
};

struct gcobj {
  union {
    uintptr_t tag;
    struct gcobj_free free;
    struct gcobj_free_medium free_medium;
    uintptr_t words[0];
    void *pointers[0];
  };
};

struct mark_space {
  uint8_t sweep_live_mask;
  uint8_t marked_mask;
  uintptr_t low_addr;
  size_t extent;
  size_t heap_size;
  uintptr_t next_block;
  struct slab *slabs;
  size_t nslabs;
};

struct heap {
  struct mark_space mark_space;
  struct large_object_space large_object_space;
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  size_t size;
  int collecting;
  int multithreaded;
  size_t active_mutator_count;
  size_t mutator_count;
  struct handle *global_roots;
  struct mutator_mark_buf *mutator_roots;
  long count;
  struct mutator *deactivated_mutators;
  struct tracer tracer;
};

struct mutator_mark_buf {
  struct mutator_mark_buf *next;
  size_t size;
  size_t capacity;
  struct gcobj **objects;
};

struct mutator {
  // Segregated freelists of small objects.
  struct gcobj_free *small_objects[MEDIUM_OBJECT_GRANULE_THRESHOLD];
  // Unordered list of medium objects.
  struct gcobj_free_medium *medium_objects;
  uintptr_t sweep;
  struct heap *heap;
  struct handle *roots;
  struct mutator_mark_buf mark_buf;
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

static inline struct gcobj_free**
get_small_object_freelist(struct mutator *mut, size_t granules) {
  ASSERT(granules > 0 && granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD);
  return &mut->small_objects[granules - 1];
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct mutator *mut) NEVER_INLINE;

static inline uint8_t* mark_byte(struct mark_space *space, struct gcobj *obj) {
  return object_metadata_byte(obj);
}

static inline int mark_space_trace_object(struct mark_space *space,
                                          struct gcobj *obj) {
  uint8_t *loc = object_metadata_byte(obj);
  uint8_t byte = *loc;
  if (byte & space->marked_mask)
    return 0;
  uint8_t mask = METADATA_BYTE_YOUNG | METADATA_BYTE_MARK_0
    | METADATA_BYTE_MARK_1 | METADATA_BYTE_MARK_2;
  *loc = (byte & ~mask) | space->marked_mask;
  return 1;
}

static inline int mark_space_contains(struct mark_space *space,
                                      struct gcobj *obj) {
  uintptr_t addr = (uintptr_t)obj;
  return addr - space->low_addr < space->extent;
}

static inline int large_object_space_trace_object(struct large_object_space *space,
                                                  struct gcobj *obj) {
  return large_object_space_copy(space, (uintptr_t)obj);
}

static inline int trace_object(struct heap *heap, struct gcobj *obj) {
  if (LIKELY(mark_space_contains(heap_mark_space(heap), obj)))
    return mark_space_trace_object(heap_mark_space(heap), obj);
  else if (large_object_space_contains(heap_large_object_space(heap), obj))
    return large_object_space_trace_object(heap_large_object_space(heap), obj);
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

static void clear_mutator_freelists(struct mutator *mut) {
  for (int i = 0; i < MEDIUM_OBJECT_GRANULE_THRESHOLD; i++)
    mut->small_objects[i] = NULL;
  mut->medium_objects = NULL;
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

static int heap_steal_pages(struct heap *heap, size_t npages) {
  // FIXME: When we have a block-structured mark space, actually return
  // pages to the OS, and limit to the current heap size.
  return 1;
}
static void heap_reset_stolen_pages(struct heap *heap, size_t npages) {
  // FIXME: Possibly reclaim blocks from the reclaimed set.
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

// Mark the roots of a mutator that is stopping for GC.  We can't
// enqueue them directly, so we send them to the controller in a buffer.
static void mark_stopping_mutator_roots(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  struct mutator_mark_buf *local_roots = &mut->mark_buf;
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gcobj *root = h->v;
    if (root && trace_object(heap, root))
      mutator_mark_buf_push(local_roots, root);
  }

  // Post to global linked-list of thread roots.
  struct mutator_mark_buf *next =
    atomic_load_explicit(&heap->mutator_roots, memory_order_acquire);
  do {
    local_roots->next = next;
  } while (!atomic_compare_exchange_weak(&heap->mutator_roots,
                                         &next, local_roots));
}

// Mark the roots of the mutator that causes GC.
static void mark_controlling_mutator_roots(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gcobj *root = h->v;
    if (root && trace_object(heap, root))
      tracer_enqueue_root(&heap->tracer, root);
  }
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

static void mark_inactive_mutators(struct heap *heap) {
  for (struct mutator *mut = heap->deactivated_mutators; mut; mut = mut->next) {
    finish_sweeping(mut);
    mark_controlling_mutator_roots(mut);
  }
}

static void mark_global_roots(struct heap *heap) {
  for (struct handle *h = heap->global_roots; h; h = h->next) {
    struct gcobj *obj = h->v;
    if (obj && trace_object(heap, obj))
      tracer_enqueue_root(&heap->tracer, obj);
  }

  struct mutator_mark_buf *roots = atomic_load(&heap->mutator_roots);
  for (; roots; roots = roots->next)
    tracer_enqueue_roots(&heap->tracer, roots->objects, roots->size);
  atomic_store(&heap->mutator_roots, NULL);
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
  finish_sweeping(mut);
  mark_controlling_mutator_roots(mut);
  pause_mutator_for_collection(heap);
  clear_mutator_freelists(mut);
}

static void pause_mutator_for_collection_without_lock(struct mutator *mut) NEVER_INLINE;
static void pause_mutator_for_collection_without_lock(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  ASSERT(mutators_are_stopping(heap));
  finish_sweeping(mut);
  mark_stopping_mutator_roots(mut);
  heap_lock(heap);
  pause_mutator_for_collection(heap);
  heap_unlock(heap);
  release_stopping_mutator_roots(mut);
  clear_mutator_freelists(mut);
}

static inline void maybe_pause_mutator_for_collection(struct mutator *mut) {
  while (mutators_are_stopping(mutator_heap(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static void reset_sweeper(struct mark_space *space) {
  space->next_block = (uintptr_t) &space->slabs[0].blocks;
}

static void rotate_mark_bytes(struct mark_space *space) {
  space->sweep_live_mask = rotate_dead_survivor_marked(space->sweep_live_mask);
  space->marked_mask = rotate_dead_survivor_marked(space->marked_mask);
}

static void collect(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  struct mark_space *space = heap_mark_space(heap);
  struct large_object_space *lospace = heap_large_object_space(heap);
  DEBUG("start collect #%ld:\n", heap->count);
  large_object_space_start_gc(lospace);
  tracer_prepare(heap);
  request_mutators_to_stop(heap);
  mark_controlling_mutator_roots(mut);
  wait_for_mutators_to_stop(heap);
  mark_inactive_mutators(heap);
  mark_global_roots(heap);
  tracer_trace(heap);
  tracer_release(heap);
  reset_sweeper(space);
  rotate_mark_bytes(space);
  heap->count++;
  large_object_space_finish_gc(lospace);
  heap_reset_stolen_pages(heap, lospace->live_pages_at_last_collection);
  allow_mutators_to_continue(heap);
  clear_mutator_freelists(mut);
  DEBUG("collect done\n");
}

static void push_free(struct gcobj_free **loc, struct gcobj_free *obj) {
  obj->next = *loc;
  *loc = obj;
}

static void push_small(struct mutator *mut, void *region,
                       size_t granules, size_t region_granules) {
  uintptr_t addr = (uintptr_t) region;
  struct gcobj_free **loc = get_small_object_freelist(mut, granules);
  while (granules <= region_granules) {
    push_free(loc, (struct gcobj_free*) addr);
    region_granules -= granules;
    addr += granules * GRANULE_SIZE;
  }
  // Fit any remaining granules into smaller freelist.
  if (region_granules)
    push_free(get_small_object_freelist(mut, region_granules),
              (struct gcobj_free*) addr);
}

static void push_medium(struct mutator *mut, void *region, size_t granules) {
  struct gcobj_free_medium *medium = region;
  medium->next = mut->medium_objects;
  medium->granules = granules;
  mut->medium_objects = medium;
}

static void reclaim(struct mutator *mut,
                    size_t small_object_granules,
                    void *region,
                    size_t region_granules) {
  if (small_object_granules == 0)
    small_object_granules = region_granules;
  if (small_object_granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD)
    push_small(mut, region, small_object_granules, region_granules);
  else
    push_medium(mut, region, region_granules);
}

static void split_medium_object(struct mutator *mut,
                                struct gcobj_free_medium *medium,
                                size_t granules) {
  size_t medium_granules = medium->granules;
  ASSERT(medium_granules >= granules);
  ASSERT(granules >= MEDIUM_OBJECT_GRANULE_THRESHOLD);
  // Invariant: all words in MEDIUM are 0 except the two header words.
  // MEDIUM is off the freelist.  We return a block of cleared memory, so
  // clear those fields now.
  medium->next = NULL;
  medium->granules = 0;

  if (medium_granules == granules)
    return;
  
  char *tail = ((char*)medium) + granules * GRANULE_SIZE;
  reclaim(mut, 0, tail, medium_granules - granules);
}

static void unlink_medium_object(struct gcobj_free_medium **prev,
                                 struct gcobj_free_medium *medium) {
  *prev = medium->next;
}

static size_t mark_space_live_object_granules(uint8_t *metadata) {
  size_t n = 0;
  while ((metadata[n] & METADATA_BYTE_END) == 0)
    n++;
  return n + 1;
}  

static size_t sweep_and_check_live(uint8_t *loc, uint8_t live_mask) {
  uint8_t metadata = *loc;
  // If the metadata byte is nonzero, that means either a young, dead,
  // survived, or marked object.  If it's live (young, survived, or
  // marked), we found the next mark.  Otherwise it's dead and we clear
  // the byte.
  if (metadata) {
    if (metadata & live_mask)
      return 1;
    *loc = 0;
  }
  return 0;
}

static size_t next_mark(uint8_t *mark, size_t limit, uint8_t live_mask) {
  for (size_t n = 0; n < limit; n++)
    if (sweep_and_check_live(&mark[n], live_mask))
      return n;
  return limit;
}

static uintptr_t mark_space_next_block(struct mark_space *space) {
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

// Sweep some heap to reclaim free space.  Return 1 if there is more
// heap to sweep, or 0 if we reached the end.
static int sweep(struct mutator *mut,
                 size_t small_object_granules,
                 size_t medium_object_granules) {
  // Sweep until we have reclaimed memory corresponding to twice the
  // size of the smallest medium object, or we reach the end of the
  // block.
  ssize_t to_reclaim = 2 * MEDIUM_OBJECT_GRANULE_THRESHOLD;
  uintptr_t sweep = mut->sweep;
  uintptr_t limit = align_up(sweep, BLOCK_SIZE);
  uint8_t live_mask = heap_mark_space(mutator_heap(mut))->sweep_live_mask;

  if (sweep == limit) {
    sweep = mark_space_next_block(heap_mark_space(mutator_heap(mut)));
    if (sweep == 0) {
      mut->sweep = 0;
      return 0;
    }
    limit = sweep + BLOCK_SIZE;
  }

  while (to_reclaim > 0 && sweep < limit) {
    ASSERT((sweep & (GRANULE_SIZE - 1)) == 0);
    uint8_t* mark = object_metadata_byte((struct gcobj*)sweep);
    size_t limit_granules = (limit - sweep) >> GRANULE_SIZE_LOG_2;
    if (limit_granules > to_reclaim) {
      if (small_object_granules == 0) {
        if (medium_object_granules < limit_granules)
          limit_granules = medium_object_granules;
      } else {
        limit_granules = to_reclaim;
      }
    }
    size_t free_granules = next_mark(mark, limit_granules, live_mask);
    if (free_granules) {
      ASSERT(free_granules <= limit_granules);
      size_t free_bytes = free_granules * GRANULE_SIZE;
      clear_memory(sweep + sizeof(uintptr_t), free_bytes - sizeof(uintptr_t));
      reclaim(mut, small_object_granules, (void*)sweep, free_granules);
      sweep += free_bytes;
      to_reclaim -= free_granules;

      mark += free_granules;
      if (free_granules == limit_granules)
        break;
    }
    // Object survived collection; skip over it and continue sweeping.
    ASSERT((*mark) & live_mask);
    sweep += mark_space_live_object_granules(mark) * GRANULE_SIZE;
  }

  mut->sweep = sweep;
  return 1;
}

// Another thread is triggering GC.  Before we stop, finish clearing the
// dead mark bytes for the mutator's block, and release the block.
static void finish_sweeping(struct mutator *mut) {
  uintptr_t sweep = mut->sweep;
  uintptr_t limit = align_up(sweep, BLOCK_SIZE);
  uint8_t live_mask = heap_mark_space(mutator_heap(mut))->sweep_live_mask;
  if (sweep) {
    uint8_t* mark = object_metadata_byte((struct gcobj*)sweep);
    size_t limit_granules = (limit - sweep) >> GRANULE_SIZE_LOG_2;
    while (limit_granules) {
      size_t free_granules = next_mark(mark, limit_granules, live_mask);
      if (free_granules) {
        ASSERT(free_granules <= limit_granules);
        mark += free_granules;
        limit_granules -= free_granules;
        if (limit_granules == 0)
          break;
      }
      // Object survived collection; skip over it and continue sweeping.
      ASSERT((*mark) & live_mask);
      size_t live_granules = mark_space_live_object_granules(mark);
      limit_granules -= live_granules;
      mark += live_granules;
    }
  }
}

static void* allocate_large(struct mutator *mut, enum alloc_kind kind,
                            size_t granules) {
  struct heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);

  size_t size = granules * GRANULE_SIZE;
  size_t npages = large_object_space_npages(space, size);

  heap_lock(heap);

  if (!heap_steal_pages(heap, npages)) {
    collect(mut);
    if (!heap_steal_pages(heap, npages)) {
      fprintf(stderr, "ran out of space, heap size %zu\n", heap->size);
      abort();
    }
  }

  void *ret = large_object_space_alloc(space, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(space, npages);

  heap_unlock(heap);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    abort();
  }

  *(uintptr_t*)ret = kind;
  return ret;
}

static void* allocate_medium(struct mutator *mut, enum alloc_kind kind,
                             size_t granules) {
  maybe_pause_mutator_for_collection(mut);

  int swept_from_beginning = 0;
  while (1) {
    struct gcobj_free_medium *already_scanned = NULL;
    do {
      struct gcobj_free_medium **prev = &mut->medium_objects;
      for (struct gcobj_free_medium *medium = mut->medium_objects;
           medium != already_scanned;
           prev = &medium->next, medium = medium->next) {
        if (medium->granules >= granules) {
          unlink_medium_object(prev, medium);
          split_medium_object(mut, medium, granules);
          struct gcobj *obj = (struct gcobj *)medium;
          obj->tag = tag_live(kind);
          uint8_t *metadata = object_metadata_byte(obj);
          metadata[0] = METADATA_BYTE_YOUNG;
          metadata[granules - 1] = METADATA_BYTE_END;
          return medium;
        }
      }
      already_scanned = mut->medium_objects;
    } while (sweep(mut, 0, granules));

    struct heap *heap = mutator_heap(mut);
    if (swept_from_beginning) {
      fprintf(stderr, "ran out of space, heap size %zu\n", heap->size);
      abort();
    } else {
      heap_lock(heap);
      if (mutators_are_stopping(heap))
        pause_mutator_for_collection_with_lock(mut);
      else
        collect(mut);
      heap_unlock(heap);
      swept_from_beginning = 1;
    }
  }
}
  
static int fill_small_from_small(struct mutator *mut, size_t granules) {
  // Precondition: the freelist for KIND is already empty.
  ASSERT(!*get_small_object_freelist(mut, granules));
  // See if there are small objects already on the freelists
  // that can be split.
  for (size_t next_size = granules + 1;
       next_size <= MEDIUM_OBJECT_GRANULE_THRESHOLD;
       next_size++) {
    struct gcobj_free **loc = get_small_object_freelist(mut, next_size);
    if (*loc) {
      struct gcobj_free *ret = *loc;
      *loc = ret->next;
      push_small(mut, ret, granules, next_size);
      return 1;
    }
  }
  return 0;
}

static int fill_small_from_medium(struct mutator *mut, size_t granules) {
  // If there is a medium object, take and split it.
  struct gcobj_free_medium *medium = mut->medium_objects;
  if (!medium)
    return 0;

  unlink_medium_object(&mut->medium_objects, medium);
  ASSERT(medium->granules >= MEDIUM_OBJECT_GRANULE_THRESHOLD);
  split_medium_object(mut, medium, MEDIUM_OBJECT_GRANULE_THRESHOLD);
  push_small(mut, medium, granules, MEDIUM_OBJECT_GRANULE_THRESHOLD);
  return 1;
}

static void fill_small(struct mutator *mut, size_t granules) NEVER_INLINE;
static void fill_small(struct mutator *mut, size_t granules) {
  maybe_pause_mutator_for_collection(mut);

  int swept_from_beginning = 0;
  while (1) {
    if (fill_small_from_small(mut, granules))
      break;

    if (fill_small_from_medium(mut, granules))
      break;

    if (!sweep(mut, granules, 0)) {
      struct heap *heap = mutator_heap(mut);
      if (swept_from_beginning) {
        fprintf(stderr, "ran out of space, heap size %zu\n", heap->size);
        abort();
      } else {
        heap_lock(heap);
        if (mutators_are_stopping(heap))
          pause_mutator_for_collection_with_lock(mut);
        else
          collect(mut);
        heap_unlock(heap);
        swept_from_beginning = 1;
      }
    }

    if (*get_small_object_freelist(mut, granules))
      break;
  }
}

static inline void* allocate_small(struct mutator *mut, enum alloc_kind kind,
                                   size_t granules) {
  ASSERT(granules > 0); // allocating 0 granules would be silly
  struct gcobj_free **loc = get_small_object_freelist(mut, granules);
  if (!*loc)
    fill_small(mut, granules);
  struct gcobj_free *ret = *loc;
  uint8_t *metadata = object_metadata_byte(ret);
  if (granules == 1) {
    metadata[0] = METADATA_BYTE_YOUNG | METADATA_BYTE_END;
  } else {
    metadata[0] = METADATA_BYTE_YOUNG;
    metadata[granules - 1] = METADATA_BYTE_END;
  }
  *loc = ret->next;
  struct gcobj *obj = (struct gcobj *)ret;
  obj->tag = tag_live(kind);
  return obj;
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

static inline void init_field(void **addr, void *val) {
  *addr = val;
}
static inline void set_field(void **addr, void *val) {
  *addr = val;
}
static inline void* get_field(void **addr) {
  return *addr;
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
  space->sweep_live_mask = METADATA_BYTE_YOUNG | survived | marked;
  space->slabs = slabs;
  space->nslabs = nslabs;
  space->low_addr = (uintptr_t) slabs;
  space->extent = size;
  reset_sweeper(space);
  return 1;
}

static int initialize_gc(size_t size, struct heap **heap,
                         struct mutator **mut) {
  *heap = calloc(1, sizeof(struct heap));
  if (!*heap) abort();

  pthread_mutex_init(&(*heap)->lock, NULL);
  pthread_cond_init(&(*heap)->mutator_cond, NULL);
  pthread_cond_init(&(*heap)->collector_cond, NULL);
  (*heap)->size = size;

  if (!tracer_init(*heap))
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
  printf("Heap size with overhead is %zd\n", heap->size);
}
