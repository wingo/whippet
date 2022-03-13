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
#define LARGE_OBJECT_THRESHOLD 256
#define LARGE_OBJECT_GRANULE_THRESHOLD 32

STATIC_ASSERT_EQ(GRANULE_SIZE, 1 << GRANULE_SIZE_LOG_2);
STATIC_ASSERT_EQ(LARGE_OBJECT_THRESHOLD,
                 LARGE_OBJECT_GRANULE_THRESHOLD * GRANULE_SIZE);

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

static const enum small_object_size small_object_sizes_for_granules[LARGE_OBJECT_GRANULE_THRESHOLD + 2] = {
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
  ASSERT(granules <= LARGE_OBJECT_GRANULE_THRESHOLD);
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

// Objects larger than LARGE_OBJECT_GRANULE_THRESHOLD.
struct gcobj_free_large {
  struct gcobj_free_large *next;
  size_t granules;
};

struct gcobj {
  union {
    uintptr_t tag;
    struct gcobj_free free;
    struct gcobj_free_large free_large;
    uintptr_t words[0];
    void *pointers[0];
  };
};

struct context {
  // Segregated freelists of small objects.
  struct gcobj_free *small_objects[SMALL_OBJECT_SIZES];
  // Unordered list of large objects.
  struct gcobj_free_large *large_objects;
  uintptr_t base;
  uint8_t *mark_bytes;
  uintptr_t heap_base;
  size_t size;
  uintptr_t sweep;
  struct handle *roots;
  long count;
  struct marker marker;
};

static inline struct marker* context_marker(struct context *cx) {
  return &cx->marker;
}

static inline struct gcobj_free**
get_small_object_freelist(struct context *cx, enum small_object_size kind) {
  ASSERT(kind < SMALL_OBJECT_SIZES);
  return &cx->small_objects[kind];
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct context *cx) NEVER_INLINE;

static inline uint8_t* mark_byte(struct context *cx, struct gcobj *obj) {
  uintptr_t granule = (((uintptr_t) obj) - cx->heap_base)  / GRANULE_SIZE;
  ASSERT(granule < (cx->heap_base - cx->base));
  return &cx->mark_bytes[granule];
}

static inline int mark_object(struct context *cx, struct gcobj *obj) {
  uint8_t *byte = mark_byte(cx, obj);
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

static void clear_freelists(struct context *cx) {
  for (int i = 0; i < SMALL_OBJECT_SIZES; i++)
    cx->small_objects[i] = NULL;
  cx->large_objects = NULL;
}

static void collect(struct context *cx) {
  DEBUG("start collect #%ld:\n", cx->count);
  marker_prepare(cx);
  for (struct handle *h = cx->roots; h; h = h->next)
    marker_visit_root(&h->v, cx);
  marker_trace(cx);
  marker_release(cx);
  DEBUG("done marking\n");
  cx->sweep = cx->heap_base;
  clear_freelists(cx);
  cx->count++;
}

static void push_free(struct gcobj_free **loc, struct gcobj_free *obj) {
  obj->next = *loc;
  *loc = obj;
}

static void push_small(struct context *cx, void *region,
                       enum small_object_size kind, size_t region_granules) {
  uintptr_t addr = (uintptr_t) region;
  while (region_granules) {
    size_t granules = small_object_granule_sizes[kind];
    struct gcobj_free **loc = get_small_object_freelist(cx, kind);
    while (granules <= region_granules) {
      push_free(loc, (struct gcobj_free*) addr);
      region_granules -= granules;
      addr += granules * GRANULE_SIZE;
    }
    // Fit any remaining granules into smaller freelists.
    kind--;
  }
}

static void push_large(struct context *cx, void *region, size_t granules) {
  struct gcobj_free_large *large = region;
  large->next = cx->large_objects;
  large->granules = granules;
  cx->large_objects = large;
}

static void reclaim(struct context *cx, void *obj, size_t granules) {
  if (granules <= LARGE_OBJECT_GRANULE_THRESHOLD)
    push_small(cx, obj, SMALL_OBJECT_SIZES - 1, granules);
  else
    push_large(cx, obj, granules);
}

static void split_large_object(struct context *cx,
                                struct gcobj_free_large *large,
                                size_t granules) {
  size_t large_granules = large->granules;
  ASSERT(large_granules >= granules);
  ASSERT(granules >= LARGE_OBJECT_GRANULE_THRESHOLD);
  // Invariant: all words in LARGE are 0 except the two header words.
  // LARGE is off the freelist.  We return a block of cleared memory, so
  // clear those fields now.
  large->next = NULL;
  large->granules = 0;

  if (large_granules == granules)
    return;
  
  char *tail = ((char*)large) + granules * GRANULE_SIZE;
  reclaim(cx, tail, large_granules - granules);
}

static void unlink_large_object(struct gcobj_free_large **prev,
                                struct gcobj_free_large *large) {
  *prev = large->next;
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
  if (granules > LARGE_OBJECT_GRANULE_THRESHOLD)
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
static int sweep(struct context *cx) {
  // Sweep until we have reclaimed 128 granules (1024 kB), or we reach
  // the end of the heap.
  ssize_t to_reclaim = 128;
  uintptr_t sweep = cx->sweep;
  uintptr_t limit = cx->base + cx->size;

  while (to_reclaim > 0 && sweep < limit) {
    uint8_t* mark = mark_byte(cx, (struct gcobj*)sweep);
    size_t free_granules = next_mark(mark,
                                     (limit - sweep) >> GRANULE_SIZE_LOG_2);
    if (free_granules) {
      size_t free_bytes = free_granules * GRANULE_SIZE;
      clear_memory(sweep + GRANULE_SIZE, free_bytes - GRANULE_SIZE);
      reclaim(cx, (void*)sweep, free_granules);
      sweep += free_bytes;
      to_reclaim -= free_granules;

      mark += free_granules;
      if (sweep == limit)
        break;
    }
    // Object survived collection; clear mark and continue sweeping.
    ASSERT(*mark == 1);
    *mark = 0;
    sweep += live_object_granules((struct gcobj *)sweep) * GRANULE_SIZE;
  }

  cx->sweep = sweep;
  return to_reclaim < 128;
}

static void* allocate_large(struct context *cx, enum alloc_kind kind,
                            size_t granules) {
  int swept_from_beginning = 0;
  struct gcobj_free_large *already_scanned = NULL;
  while (1) {
    do {
      struct gcobj_free_large **prev = &cx->large_objects;
      for (struct gcobj_free_large *large = cx->large_objects;
           large != already_scanned;
           prev = &large->next, large = large->next) {
        if (large->granules >= granules) {
          unlink_large_object(prev, large);
          split_large_object(cx, large, granules);
          struct gcobj *obj = (struct gcobj *)large;
          obj->tag = tag_live(kind);
          return large;
        }
      }
      already_scanned = cx->large_objects;
    } while (sweep (cx));

    // No large object, and we swept across the whole heap.  Collect.
    if (swept_from_beginning) {
      fprintf(stderr, "ran out of space, heap size %zu\n", cx->size);
      abort();
    } else {
      collect(cx);
      swept_from_beginning = 1;
    }
  }
}
  
static void fill_small(struct context *cx, enum small_object_size kind) {
  int swept_from_beginning = 0;
  while (1) {
    // First see if there are small objects already on the freelists
    // that can be split.
    for (enum small_object_size next_kind = kind;
         next_kind < SMALL_OBJECT_SIZES;
         next_kind++) {
      struct gcobj_free **loc = get_small_object_freelist(cx, next_kind);
      if (*loc) {
        if (kind != next_kind) {
          struct gcobj_free *ret = *loc;
          *loc = ret->next;
          push_small(cx, ret, kind,
                     small_object_granule_sizes[next_kind]);
        }
        return;
      }
    }

    // Otherwise if there is a large object, take and split it.
    struct gcobj_free_large *large = cx->large_objects;
    if (large) {
      unlink_large_object(&cx->large_objects, large);
      split_large_object(cx, large, LARGE_OBJECT_GRANULE_THRESHOLD);
      push_small(cx, large, kind, LARGE_OBJECT_GRANULE_THRESHOLD);
      return;
    }

    if (!sweep(cx)) {
      if (swept_from_beginning) {
        fprintf(stderr, "ran out of space, heap size %zu\n", cx->size);
        abort();
      } else {
        collect(cx);
        swept_from_beginning = 1;
      }
    }
  }
}

static inline void* allocate_small(struct context *cx,
                                   enum alloc_kind alloc_kind,
                                   enum small_object_size small_kind) {
  struct gcobj_free **loc = get_small_object_freelist(cx, small_kind);
  if (!*loc)
    fill_small(cx, small_kind);
  struct gcobj_free *ret = *loc;
  *loc = ret->next;
  struct gcobj *obj = (struct gcobj *)ret;
  obj->tag = tag_live(alloc_kind);
  return obj;
}

static inline void* allocate(struct context *cx, enum alloc_kind kind,
                             size_t size) {
  size_t granules = size_to_granules(size);
  if (granules <= LARGE_OBJECT_GRANULE_THRESHOLD)
    return allocate_small(cx, kind, granules_to_small_object_size(granules));
  return allocate_large(cx, kind, granules);
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

static inline void initialize_gc(struct context *cx, size_t size) {
#define SMALL_OBJECT_GRANULE_SIZE(i) \
    ASSERT_EQ(SMALL_OBJECT_##i, small_object_sizes_for_granules[i]); \
    ASSERT_EQ(SMALL_OBJECT_##i + 1, small_object_sizes_for_granules[i+1]);
  FOR_EACH_SMALL_OBJECT_GRANULES(SMALL_OBJECT_GRANULE_SIZE);
#undef SMALL_OBJECT_GRANULE_SIZE

  ASSERT_EQ(SMALL_OBJECT_SIZES - 1,
            small_object_sizes_for_granules[LARGE_OBJECT_GRANULE_THRESHOLD]);

  size = align_up(size, getpagesize());

  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    abort();
  }
  clear_freelists(cx);
  cx->base = (uintptr_t) mem;
  cx->mark_bytes = mem;
  size_t heap_admin_size = align_up(size / GRANULE_SIZE, GRANULE_SIZE);
  cx->heap_base = cx->base + heap_admin_size;
  cx->size = size;
  cx->sweep = cx->base + cx->size;
  cx->roots = NULL;
  cx->count = 0;
  if (!marker_init(cx))
    abort();
  reclaim(cx, (void*)cx->heap_base, size_to_granules(size - heap_admin_size));
}

static inline void print_start_gc_stats(struct context *cx) {
}

static inline void print_end_gc_stats(struct context *cx) {
  printf("Completed %ld collections\n", cx->count);
  printf("Heap size is %zd\n", cx->size);
}
