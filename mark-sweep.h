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
#include "precise-roots.h"
#ifdef GC_PARALLEL_MARK
#include "parallel-marker.h"
#else
#include "serial-marker.h"
#endif

#define GRANULE_SIZE 8
#define GRANULE_SIZE_LOG_2 3
#define MEDIUM_OBJECT_THRESHOLD 256
#define MEDIUM_OBJECT_GRANULE_THRESHOLD 32

STATIC_ASSERT_EQ(GRANULE_SIZE, 1 << GRANULE_SIZE_LOG_2);
STATIC_ASSERT_EQ(MEDIUM_OBJECT_THRESHOLD,
                 MEDIUM_OBJECT_GRANULE_THRESHOLD * GRANULE_SIZE);

// There are small object pages for allocations of these sizes.
#define FOR_EACH_SMALL_OBJECT_GRANULES(M) \
  M(1) M(2) M(3) M(4) M(5) M(6) M(8) M(10) M(16) M(32)

enum small_object_size {
#define SMALL_OBJECT_GRANULE_SIZE(i) SMALL_OBJECT_##i,
  FOR_EACH_SMALL_OBJECT_GRANULES(SMALL_OBJECT_GRANULE_SIZE)
#undef SMALL_OBJECT_GRANULE_SIZE
  SMALL_OBJECT_SIZES,
  NOT_SMALL_OBJECT = SMALL_OBJECT_SIZES
};

static const uint8_t small_object_granule_sizes[] = 
{
#define SMALL_OBJECT_GRANULE_SIZE(i) i,
  FOR_EACH_SMALL_OBJECT_GRANULES(SMALL_OBJECT_GRANULE_SIZE)
#undef SMALL_OBJECT_GRANULE_SIZE
};

static const enum small_object_size small_object_sizes_for_granules[MEDIUM_OBJECT_GRANULE_THRESHOLD + 2] = {
  SMALL_OBJECT_1,   SMALL_OBJECT_1, SMALL_OBJECT_2,  SMALL_OBJECT_3,
  SMALL_OBJECT_4,   SMALL_OBJECT_5,   SMALL_OBJECT_6,  SMALL_OBJECT_8,
  SMALL_OBJECT_8,   SMALL_OBJECT_10,  SMALL_OBJECT_10, SMALL_OBJECT_16,
  SMALL_OBJECT_16,  SMALL_OBJECT_16,  SMALL_OBJECT_16, SMALL_OBJECT_16,
  SMALL_OBJECT_16,  SMALL_OBJECT_32,  SMALL_OBJECT_32, SMALL_OBJECT_32,
  SMALL_OBJECT_32,  SMALL_OBJECT_32,  SMALL_OBJECT_32, SMALL_OBJECT_32,
  SMALL_OBJECT_32,  SMALL_OBJECT_32,  SMALL_OBJECT_32, SMALL_OBJECT_32,
  SMALL_OBJECT_32,  SMALL_OBJECT_32,  SMALL_OBJECT_32, SMALL_OBJECT_32,
  SMALL_OBJECT_32,  NOT_SMALL_OBJECT
};

static enum small_object_size granules_to_small_object_size(unsigned granules) {
  ASSERT(granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD);
  return small_object_sizes_for_granules[granules];
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

struct gcobj_freelists {
  struct gcobj_free *by_size[SMALL_OBJECT_SIZES];
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
  pthread_mutex_t lock;
  pthread_cond_t collector_cond;
  pthread_cond_t mutator_cond;
  int collecting;
  int multithreaded;
  size_t active_mutator_count;
  size_t mutator_count;
  struct gcobj_freelists small_objects;
  // Unordered list of medium objects.
  struct gcobj_free_medium *medium_objects;
  uintptr_t base;
  uint8_t *mark_bytes;
  uintptr_t heap_base;
  size_t heap_size;
  uintptr_t sweep;
  struct handle *global_roots;
  struct mutator_mark_buf *mutator_roots;
  void *mem;
  size_t mem_size;
  long count;
  struct marker marker;
  struct mutator *deactivated_mutators;
};

struct heap {
  struct mark_space mark_space;
};

struct mutator_mark_buf {
  struct mutator_mark_buf *next;
  size_t size;
  size_t capacity;
  struct gcobj **objects;
};

struct mutator {
  // Segregated freelists of small objects.
  struct gcobj_freelists small_objects;
  struct heap *heap;
  struct handle *roots;
  struct mutator_mark_buf mark_buf;
  struct mutator *next;
};

static inline struct marker* mark_space_marker(struct mark_space *space) {
  return &space->marker;
}
static inline struct mark_space* heap_mark_space(struct heap *heap) {
  return &heap->mark_space;
}
static inline struct heap* mutator_heap(struct mutator *mutator) {
  return mutator->heap;
}
static inline struct mark_space* mutator_mark_space(struct mutator *mutator) {
  return heap_mark_space(mutator_heap(mutator));
}

static inline struct gcobj_free**
get_small_object_freelist(struct gcobj_freelists *freelists,
                          enum small_object_size kind) {
  ASSERT(kind < SMALL_OBJECT_SIZES);
  return &freelists->by_size[kind];
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct mark_space *space, struct mutator *mut) NEVER_INLINE;

static inline uint8_t* mark_byte(struct mark_space *space, struct gcobj *obj) {
  ASSERT(space->heap_base <= (uintptr_t) obj);
  ASSERT((uintptr_t) obj < space->heap_base + space->heap_size);
  uintptr_t granule = (((uintptr_t) obj) - space->heap_base)  / GRANULE_SIZE;
  return &space->mark_bytes[granule];
}

static inline int mark_object(struct mark_space *space, struct gcobj *obj) {
  uint8_t *byte = mark_byte(space, obj);
  if (*byte)
    return 0;
  *byte = 1;
  return 1;
}

static inline void trace_one(struct gcobj *obj, void *mark_data) {
  switch (tag_live_alloc_kind(obj->tag)) {
#define SCAN_OBJECT(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      visit_##name##_fields((Name*)obj, marker_visit, mark_data); \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    abort ();
  }
}

static void clear_small_freelists(struct gcobj_freelists *small) {
  for (int i = 0; i < SMALL_OBJECT_SIZES; i++)
    small->by_size[i] = NULL;
}
static void clear_mutator_freelists(struct mutator *mut) {
  clear_small_freelists(&mut->small_objects);
}
static void clear_global_freelists(struct mark_space *space) {
  clear_small_freelists(&space->small_objects);
  space->medium_objects = NULL;
}

static int space_has_multiple_mutators(struct mark_space *space) {
  return atomic_load_explicit(&space->multithreaded, memory_order_relaxed);
}

static int mutators_are_stopping(struct mark_space *space) {
  return atomic_load_explicit(&space->collecting, memory_order_relaxed);
}

static inline void mark_space_lock(struct mark_space *space) {
  pthread_mutex_lock(&space->lock);
}
static inline void mark_space_unlock(struct mark_space *space) {
  pthread_mutex_unlock(&space->lock);
}

static void add_mutator(struct heap *heap, struct mutator *mut) {
  mut->heap = heap;
  struct mark_space *space = heap_mark_space(heap);
  mark_space_lock(space);
  // We have no roots.  If there is a GC currently in progress, we have
  // nothing to add.  Just wait until it's done.
  while (mutators_are_stopping(space))
    pthread_cond_wait(&space->mutator_cond, &space->lock);
  if (space->mutator_count == 1)
    space->multithreaded = 1;
  space->active_mutator_count++;
  space->mutator_count++;
  mark_space_unlock(space);
}

static void remove_mutator(struct heap *heap, struct mutator *mut) {
  mut->heap = NULL;
  struct mark_space *space = heap_mark_space(heap);
  mark_space_lock(space);
  space->active_mutator_count--;
  space->mutator_count--;
  // We have no roots.  If there is a GC stop currently in progress,
  // maybe tell the controller it can continue.
  if (mutators_are_stopping(space) && space->active_mutator_count == 0)
    pthread_cond_signal(&space->collector_cond);
  mark_space_unlock(space);
}

static void request_mutators_to_stop(struct mark_space *space) {
  ASSERT(!mutators_are_stopping(space));
  atomic_store_explicit(&space->collecting, 1, memory_order_relaxed);
}

static void allow_mutators_to_continue(struct mark_space *space) {
  ASSERT(mutators_are_stopping(space));
  ASSERT(space->active_mutator_count == 0);
  space->active_mutator_count++;
  atomic_store_explicit(&space->collecting, 0, memory_order_relaxed);
  ASSERT(!mutators_are_stopping(space));
  pthread_cond_broadcast(&space->mutator_cond);
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
  struct mark_space *space = mutator_mark_space(mut);
  struct mutator_mark_buf *local_roots = &mut->mark_buf;
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gcobj *root = h->v;
    if (root && mark_object(space, root))
      mutator_mark_buf_push(local_roots, root);
  }

  // Post to global linked-list of thread roots.
  struct mutator_mark_buf *next =
    atomic_load_explicit(&space->mutator_roots, memory_order_acquire);
  do {
    local_roots->next = next;
  } while (!atomic_compare_exchange_weak(&space->mutator_roots,
                                         &next, local_roots));
}

// Mark the roots of the mutator that causes GC.
static void mark_controlling_mutator_roots(struct mutator *mut) {
  struct mark_space *space = mutator_mark_space(mut);
  for (struct handle *h = mut->roots; h; h = h->next) {
    struct gcobj *root = h->v;
    if (root && mark_object(space, root))
      marker_enqueue_root(&space->marker, root);
  }
}

static void release_stopping_mutator_roots(struct mutator *mut) {
  mutator_mark_buf_release(&mut->mark_buf);
}

static void wait_for_mutators_to_stop(struct mark_space *space) {
  space->active_mutator_count--;
  while (space->active_mutator_count)
    pthread_cond_wait(&space->collector_cond, &space->lock);
}

static void mark_inactive_mutators(struct mark_space *space) {
  for (struct mutator *mut = space->deactivated_mutators; mut; mut = mut->next)
    mark_controlling_mutator_roots(mut);
}

static void mark_global_roots(struct mark_space *space) {
  for (struct handle *h = space->global_roots; h; h = h->next) {
    struct gcobj *obj = h->v;
    if (obj && mark_object(space, obj))
      marker_enqueue_root(&space->marker, obj);
  }

  struct mutator_mark_buf *roots = atomic_load(&space->mutator_roots);
  for (; roots; roots = roots->next)
    marker_enqueue_roots(&space->marker, roots->objects, roots->size);
  atomic_store(&space->mutator_roots, NULL);
}

static void pause_mutator_for_collection(struct mark_space *space) NEVER_INLINE;
static void pause_mutator_for_collection(struct mark_space *space) {
  ASSERT(mutators_are_stopping(space));
  ASSERT(space->active_mutator_count);
  space->active_mutator_count--;
  if (space->active_mutator_count == 0)
    pthread_cond_signal(&space->collector_cond);

  // Go to sleep and wake up when the collector is done.  Note,
  // however, that it may be that some other mutator manages to
  // trigger collection before we wake up.  In that case we need to
  // mark roots, not just sleep again.  To detect a wakeup on this
  // collection vs a future collection, we use the global GC count.
  // This is safe because the count is protected by the space lock,
  // which we hold.
  long epoch = space->count;
  do
    pthread_cond_wait(&space->mutator_cond, &space->lock);
  while (mutators_are_stopping(space) && space->count == epoch);

  space->active_mutator_count++;
}

static void pause_mutator_for_collection_with_lock(struct mutator *mut) NEVER_INLINE;
static void pause_mutator_for_collection_with_lock(struct mutator *mut) {
  struct mark_space *space = mutator_mark_space(mut);
  ASSERT(mutators_are_stopping(space));
  mark_controlling_mutator_roots(mut);
  pause_mutator_for_collection(space);
  clear_mutator_freelists(mut);
}

static void pause_mutator_for_collection_without_lock(struct mutator *mut) NEVER_INLINE;
static void pause_mutator_for_collection_without_lock(struct mutator *mut) {
  struct mark_space *space = mutator_mark_space(mut);
  ASSERT(mutators_are_stopping(space));
  mark_stopping_mutator_roots(mut);
  mark_space_lock(space);
  pause_mutator_for_collection(space);
  mark_space_unlock(space);
  release_stopping_mutator_roots(mut);
  clear_mutator_freelists(mut);
}

static inline void maybe_pause_mutator_for_collection(struct mutator *mut) {
  while (mutators_are_stopping(mutator_mark_space(mut)))
    pause_mutator_for_collection_without_lock(mut);
}

static void reset_sweeper(struct mark_space *space) {
  space->sweep = space->heap_base;
}

static void collect(struct mark_space *space, struct mutator *mut) {
  DEBUG("start collect #%ld:\n", space->count);
  marker_prepare(space);
  request_mutators_to_stop(space);
  mark_controlling_mutator_roots(mut);
  wait_for_mutators_to_stop(space);
  mark_inactive_mutators(space);
  mark_global_roots(space);
  marker_trace(space);
  marker_release(space);
  clear_global_freelists(space);
  reset_sweeper(space);
  space->count++;
  allow_mutators_to_continue(space);
  clear_mutator_freelists(mut);
  DEBUG("collect done\n");
}

static void push_free(struct gcobj_free **loc, struct gcobj_free *obj) {
  obj->next = *loc;
  *loc = obj;
}

static void push_small(struct gcobj_freelists *small_objects, void *region,
                       enum small_object_size kind, size_t region_granules) {
  uintptr_t addr = (uintptr_t) region;
  while (region_granules) {
    size_t granules = small_object_granule_sizes[kind];
    struct gcobj_free **loc = get_small_object_freelist(small_objects, kind);
    while (granules <= region_granules) {
      push_free(loc, (struct gcobj_free*) addr);
      region_granules -= granules;
      addr += granules * GRANULE_SIZE;
    }
    // Fit any remaining granules into smaller freelists.
    kind--;
  }
}

static void push_medium(struct mark_space *space, void *region, size_t granules) {
  struct gcobj_free_medium *medium = region;
  medium->next = space->medium_objects;
  medium->granules = granules;
  space->medium_objects = medium;
}

static void reclaim_small(struct gcobj_freelists *small_objects,
                          enum small_object_size kind,
                          void *region, size_t region_granules) {
  ASSERT(kind != NOT_SMALL_OBJECT);
  struct gcobj_free **loc = get_small_object_freelist(small_objects, kind);
  uintptr_t addr = (uintptr_t) region;
  size_t object_granules = small_object_granule_sizes[kind];
  while (region_granules >= object_granules) {
    push_free(loc, (struct gcobj_free*) addr);
    region_granules -= object_granules;
    addr += object_granules * GRANULE_SIZE;
  }
  // Any leftover granules are wasted!
}

static void reclaim(struct mark_space *space,
                    struct gcobj_freelists *small_objects,
                    enum small_object_size kind,
                    void *region,
                    size_t region_granules) {
  if (kind != NOT_SMALL_OBJECT)
    reclaim_small(small_objects, kind, region, region_granules);
  else if (region_granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD)
    push_small(small_objects, region, SMALL_OBJECT_SIZES - 1, region_granules);
  else
    push_medium(space, region, region_granules);
}

static void split_medium_object(struct mark_space *space,
                               struct gcobj_freelists *small_objects,
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
  reclaim(space, small_objects, NOT_SMALL_OBJECT, tail,
          medium_granules - granules);
}

static void unlink_medium_object(struct gcobj_free_medium **prev,
                                struct gcobj_free_medium *medium) {
  *prev = medium->next;
}

static size_t live_object_granules(struct gcobj *obj) {
  size_t bytes;
  switch (tag_live_alloc_kind (obj->tag)) {
#define COMPUTE_SIZE(name, Name, NAME) \
    case ALLOC_KIND_##NAME:            \
      bytes = name##_size((Name*)obj); \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(COMPUTE_SIZE)
#undef COMPUTE_SIZE
  default:
    abort ();
  }
  size_t granules = size_to_granules(bytes);
  if (granules > MEDIUM_OBJECT_GRANULE_THRESHOLD)
    return granules;
  return small_object_granule_sizes[granules_to_small_object_size(granules)];
}  

static size_t next_mark(const uint8_t *mark, size_t limit) {
  size_t n = 0;
  for (; (((uintptr_t)mark) & 7) && n < limit; n++)
    if (mark[n])
      return n;
  uintptr_t *word_mark = (uintptr_t *)(mark + n);
  for (;
       n + sizeof(uintptr_t) * 4 <= limit;
       n += sizeof(uintptr_t) * 4, word_mark += 4)
    if (word_mark[0] | word_mark[1] | word_mark[2] | word_mark[3])
      break;
  for (;
       n + sizeof(uintptr_t) <= limit;
       n += sizeof(uintptr_t), word_mark += 1)
    if (word_mark[0])
      break;
  for (; n < limit; n++)
    if (mark[n])
      return n;
  return limit;
}

// Sweep some heap to reclaim free space.  Return 1 if there is more
// heap to sweep, or 0 if we reached the end.
static int sweep(struct mark_space *space,
                 struct gcobj_freelists *small_objects,
                 enum small_object_size kind,
                 size_t medium_object_granules) {
  // Sweep until we have reclaimed 32 kB of free memory, or we reach the
  // end of the heap.
  ssize_t to_reclaim = 32 * 1024 / GRANULE_SIZE;
  uintptr_t sweep = space->sweep;
  uintptr_t limit = space->heap_base + space->heap_size;

  if (sweep == limit)
    return 0;

  while (to_reclaim > 0 && sweep < limit) {
    uint8_t* mark = mark_byte(space, (struct gcobj*)sweep);
    size_t limit_granules = (limit - sweep) >> GRANULE_SIZE_LOG_2;
    if (limit_granules > to_reclaim) {
      if (kind == NOT_SMALL_OBJECT) {
        if (medium_object_granules < limit_granules)
          limit_granules = medium_object_granules;
      } else {
        limit_granules = to_reclaim;
      }
    }
    size_t free_granules = next_mark(mark, limit_granules);
    if (free_granules) {
      size_t free_bytes = free_granules * GRANULE_SIZE;
      clear_memory(sweep + GRANULE_SIZE, free_bytes - GRANULE_SIZE);
      reclaim(space, small_objects, kind, (void*)sweep, free_granules);
      sweep += free_bytes;
      to_reclaim -= free_granules;

      mark += free_granules;
      if (free_granules == limit_granules)
        break;
    }
    // Object survived collection; clear mark and continue sweeping.
    ASSERT(*mark == 1);
    *mark = 0;
    sweep += live_object_granules((struct gcobj *)sweep) * GRANULE_SIZE;
  }

  space->sweep = sweep;
  return 1;
}

static void* allocate_medium(struct mutator *mut, enum alloc_kind kind,
                            size_t granules) {
  struct mark_space *space = mutator_mark_space(mut);
  struct gcobj_freelists *small_objects = space_has_multiple_mutators(space) ?
    &space->small_objects : &mut->small_objects;

  maybe_pause_mutator_for_collection(mut);

  mark_space_lock(space);

  while (mutators_are_stopping(space))
    pause_mutator_for_collection_with_lock(mut);

  int swept_from_beginning = 0;
  while (1) {
    struct gcobj_free_medium *already_scanned = NULL;
    do {
      struct gcobj_free_medium **prev = &space->medium_objects;
      for (struct gcobj_free_medium *medium = space->medium_objects;
           medium != already_scanned;
           prev = &medium->next, medium = medium->next) {
        if (medium->granules >= granules) {
          unlink_medium_object(prev, medium);
          split_medium_object(space, small_objects, medium, granules);
          mark_space_unlock(space);
          struct gcobj *obj = (struct gcobj *)medium;
          obj->tag = tag_live(kind);
          return medium;
        }
      }
      already_scanned = space->medium_objects;
    } while (sweep(space, small_objects, NOT_SMALL_OBJECT, granules));

    // No medium object, and we swept across the whole heap.  Collect.
    if (swept_from_beginning) {
      fprintf(stderr, "ran out of space, heap size %zu\n", space->heap_size);
      abort();
    } else {
      collect(space, mut);
      swept_from_beginning = 1;
    }
  }
}
  
static int fill_small_from_local(struct gcobj_freelists *small_objects,
                                 enum small_object_size kind) {
  // Precondition: the freelist for KIND is already empty.
  ASSERT(!*get_small_object_freelist(small_objects, kind));
  // See if there are small objects already on the freelists
  // that can be split.
  for (enum small_object_size next_kind = kind + 1;
       next_kind < SMALL_OBJECT_SIZES;
       next_kind++) {
    struct gcobj_free **loc = get_small_object_freelist(small_objects,
                                                        next_kind);
    if (*loc) {
      struct gcobj_free *ret = *loc;
      *loc = ret->next;
      push_small(small_objects, ret, kind,
                 small_object_granule_sizes[next_kind]);
      return 1;
    }
  }
  return 0;
}

// with space lock
static int fill_small_from_medium(struct mark_space *space,
                                 struct gcobj_freelists *small_objects,
                                 enum small_object_size kind) {
  // If there is a medium object, take and split it.
  struct gcobj_free_medium *medium = space->medium_objects;
  if (!medium)
    return 0;

  unlink_medium_object(&space->medium_objects, medium);
  ASSERT(medium->granules >= MEDIUM_OBJECT_GRANULE_THRESHOLD);
  split_medium_object(space, small_objects, medium,
                     MEDIUM_OBJECT_GRANULE_THRESHOLD);
  push_small(small_objects, medium, kind, MEDIUM_OBJECT_GRANULE_THRESHOLD);
  return 1;
}

static int fill_small_from_global_small(struct mark_space *space,
                                        struct gcobj_freelists *small_objects,
                                        enum small_object_size kind) {
  struct gcobj_free **src =
    get_small_object_freelist(&space->small_objects, kind);
  if (*src) {
    struct gcobj_free **dst = get_small_object_freelist(small_objects, kind);
    ASSERT(!*dst);
    *dst = *src;
    *src = NULL;
    return 1;
  }
  return 0;
}

static void fill_small_from_global(struct mutator *mut,
                                   enum small_object_size kind) NEVER_INLINE;
static void fill_small_from_global(struct mutator *mut,
                                   enum small_object_size kind) {
  struct gcobj_freelists *small_objects = &mut->small_objects;
  struct mark_space *space = mutator_mark_space(mut);

  maybe_pause_mutator_for_collection(mut);

  mark_space_lock(space);

  while (mutators_are_stopping(space))
    pause_mutator_for_collection_with_lock(mut);

  int swept_from_beginning = 0;
  while (1) {
    if (fill_small_from_global_small(space, small_objects, kind))
      break;

    if (fill_small_from_medium(space, small_objects, kind))
      break;

    // By default, pull in 16 kB of data at a time.
    if (!sweep(space, small_objects, kind, 0)) {
      if (swept_from_beginning) {
        fprintf(stderr, "ran out of space, heap size %zu\n", space->heap_size);
        abort();
      } else {
        collect(space, mut);
        swept_from_beginning = 1;
      }
    }

    if (*get_small_object_freelist(small_objects, kind))
      break;
  }
  mark_space_unlock(space);
}

static void fill_small(struct mutator *mut, enum small_object_size kind) {
  // See if there are small objects already on the local freelists that
  // can be split.
  if (fill_small_from_local(&mut->small_objects, kind))
    return;

  fill_small_from_global(mut, kind);
}

static inline void* allocate_small(struct mutator *mut,
                                   enum alloc_kind alloc_kind,
                                   enum small_object_size small_kind) {
  struct gcobj_free **loc =
    get_small_object_freelist(&mut->small_objects, small_kind);
  if (!*loc)
    fill_small(mut, small_kind);
  struct gcobj_free *ret = *loc;
  *loc = ret->next;
  struct gcobj *obj = (struct gcobj *)ret;
  obj->tag = tag_live(alloc_kind);
  return obj;
}

static inline void* allocate(struct mutator *mut, enum alloc_kind kind,
                             size_t size) {
  size_t granules = size_to_granules(size);
  if (granules <= MEDIUM_OBJECT_GRANULE_THRESHOLD)
    return allocate_small(mut, kind, granules_to_small_object_size(granules));
  return allocate_medium(mut, kind, granules);
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

static int initialize_gc(size_t size, struct heap **heap,
                         struct mutator **mut) {
#define SMALL_OBJECT_GRANULE_SIZE(i) \
    ASSERT_EQ(SMALL_OBJECT_##i, small_object_sizes_for_granules[i]); \
    ASSERT_EQ(SMALL_OBJECT_##i + 1, small_object_sizes_for_granules[i+1]);
  FOR_EACH_SMALL_OBJECT_GRANULES(SMALL_OBJECT_GRANULE_SIZE);
#undef SMALL_OBJECT_GRANULE_SIZE

  ASSERT_EQ(SMALL_OBJECT_SIZES - 1,
            small_object_sizes_for_granules[MEDIUM_OBJECT_GRANULE_THRESHOLD]);

  size = align_up(size, getpagesize());

  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return 0;
  }

  *heap = calloc(1, sizeof(struct heap));
  if (!*heap) abort();
  struct mark_space *space = heap_mark_space(*heap);
  space->mem = mem;
  space->mem_size = size;
  // If there is 1 mark byte per granule, and SIZE bytes available for
  // HEAP_SIZE + MARK_BYTES, then:
  //
  //   size = (granule_size + 1) / granule_size * heap_size
  //   mark_bytes = 1/granule_size * heap_size
  //   mark_bytes = ceil(heap_size / (granule_size + 1))
  space->mark_bytes = (uint8_t *) mem;
  size_t mark_bytes_size = (size + GRANULE_SIZE) / (GRANULE_SIZE + 1);
  size_t overhead = align_up(mark_bytes_size, GRANULE_SIZE);

  pthread_mutex_init(&space->lock, NULL);
  pthread_cond_init(&space->mutator_cond, NULL);
  pthread_cond_init(&space->collector_cond, NULL);

  space->heap_base = ((uintptr_t) mem) + overhead;
  space->heap_size = size - overhead;
  space->sweep = space->heap_base + space->heap_size;
  if (!marker_init(space))
    abort();
  reclaim(space, NULL, NOT_SMALL_OBJECT, (void*)space->heap_base,
          size_to_granules(space->heap_size));

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

static void deactivate_mutator(struct mark_space *space, struct mutator *mut) {
  ASSERT(mut->next == NULL);
  mark_space_lock(space);
  mut->next = space->deactivated_mutators;
  space->deactivated_mutators = mut;
  space->active_mutator_count--;
  if (!space->active_mutator_count && mutators_are_stopping(space))
    pthread_cond_signal(&space->collector_cond);
  mark_space_unlock(space);
}

static void reactivate_mutator(struct mark_space *space, struct mutator *mut) {
  mark_space_lock(space);
  while (mutators_are_stopping(space))
    pthread_cond_wait(&space->mutator_cond, &space->lock);
  struct mutator **prev = &space->deactivated_mutators;
  while (*prev != mut)
    prev = &(*prev)->next;
  *prev = mut->next;
  mut->next = NULL;
  space->active_mutator_count++;
  mark_space_unlock(space);
}

static void* call_without_gc(struct mutator *mut, void* (*f)(void*),
                             void *data) NEVER_INLINE;
static void* call_without_gc(struct mutator *mut,
                             void* (*f)(void*),
                             void *data) {
  struct mark_space *space = mutator_mark_space(mut);
  deactivate_mutator(space, mut);
  void *ret = f(data);
  reactivate_mutator(space, mut);
  return ret;
}

static inline void print_start_gc_stats(struct heap *heap) {
}

static inline void print_end_gc_stats(struct heap *heap) {
  printf("Completed %ld collections\n", heap_mark_space(heap)->count);
  printf("Heap size with overhead is %zd\n", heap_mark_space(heap)->mem_size);
}
