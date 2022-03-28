#include <stdint.h>

#include "conservative-roots.h"

// When pthreads are used, let `libgc' know about it and redirect
// allocation calls such as `GC_MALLOC ()' to (contention-free, faster)
// thread-local allocation.

#define GC_THREADS 1
#define GC_REDIRECT_TO_LOCAL 1

// Don't #define pthread routines to their GC_pthread counterparts.
// Instead we will be careful inside the benchmarks to use API to
// register threads with libgc.
#define GC_NO_THREAD_REDIRECTS 1

#include <gc/gc.h>
#include <gc/gc_inline.h> /* GC_generic_malloc_many */

#define GC_INLINE_GRANULE_WORDS 2
#define GC_INLINE_GRANULE_BYTES (sizeof(void *) * GC_INLINE_GRANULE_WORDS)

/* A freelist set contains GC_INLINE_FREELIST_COUNT pointers to singly
   linked lists of objects of different sizes, the ith one containing
   objects i + 1 granules in size.  This setting of
   GC_INLINE_FREELIST_COUNT will hold freelists for allocations of
   up to 256 bytes.  */
#define GC_INLINE_FREELIST_COUNT (256U / GC_INLINE_GRANULE_BYTES)

struct heap {
  pthread_mutex_t lock;
  int multithreaded;
};

struct mutator {
  void *freelists[GC_INLINE_FREELIST_COUNT];
  void *pointerless_freelists[GC_INLINE_FREELIST_COUNT];
  struct heap *heap;
};

static inline size_t gc_inline_bytes_to_freelist_index(size_t bytes) {
  return (bytes - 1U) / GC_INLINE_GRANULE_BYTES;
}
static inline size_t gc_inline_freelist_object_size(size_t idx) {
  return (idx + 1U) * GC_INLINE_GRANULE_BYTES;
}

// The values of these must match the internal POINTERLESS and NORMAL
// definitions in libgc, for which unfortunately there are no external
// definitions.  Alack.
enum gc_inline_kind {
  GC_INLINE_KIND_POINTERLESS,
  GC_INLINE_KIND_NORMAL
};

static void* allocate_small_slow(void **freelist, size_t idx,
                                 enum gc_inline_kind kind) NEVER_INLINE;
static void* allocate_small_slow(void **freelist, size_t idx,
                                 enum gc_inline_kind kind) {
  size_t bytes = gc_inline_freelist_object_size(idx);
  GC_generic_malloc_many(bytes, kind, freelist);
  void *head = *freelist;
  if (UNLIKELY (!head)) {
    fprintf(stderr, "ran out of space, heap size %zu\n",
            GC_get_heap_size());
    abort();
  }
  *freelist = *(void **)(head);
  return head;
}

static inline void *
allocate_small(void **freelist, size_t idx, enum gc_inline_kind kind) {
  void *head = *freelist;

  if (UNLIKELY (!head))
    return allocate_small_slow(freelist, idx, kind);

  *freelist = *(void **)(head);
  return head;
}

#define GC_HEADER /**/

static inline void* allocate(struct mutator *mut, enum alloc_kind kind,
                             size_t size) {
  size_t idx = gc_inline_bytes_to_freelist_index(size);

  if (UNLIKELY(idx >= GC_INLINE_FREELIST_COUNT))
    return GC_malloc(size);

  return allocate_small(&mut->freelists[idx], idx, GC_INLINE_KIND_NORMAL);
}

static inline void* allocate_pointerless(struct mutator *mut,
                                         enum alloc_kind kind, size_t size) {
  size_t idx = gc_inline_bytes_to_freelist_index(size);

  if (UNLIKELY (idx >= GC_INLINE_FREELIST_COUNT))
    return GC_malloc_atomic(size);

  return allocate_small(&mut->pointerless_freelists[idx], idx,
                        GC_INLINE_KIND_POINTERLESS);
}

static inline void collect(struct mutator *mut) {
  GC_gcollect();
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

static inline struct mutator *add_mutator(struct heap *heap) {
  struct mutator *ret = GC_malloc(sizeof(struct mutator));
  ret->heap = heap;
  return ret;
}

static inline struct heap *mutator_heap(struct mutator *mutator) {
  return mutator->heap;
}

static int initialize_gc(size_t heap_size, struct heap **heap,
                         struct mutator **mutator) {
  // GC_full_freq = 30;
  // GC_free_space_divisor = 16;
  // GC_enable_incremental();
  GC_INIT();
  size_t current_heap_size = GC_get_heap_size();
  if (heap_size > current_heap_size) {
    GC_set_max_heap_size (heap_size);
    GC_expand_hp(heap_size - current_heap_size);
  }
  *heap = GC_malloc(sizeof(struct heap));
  pthread_mutex_init(&(*heap)->lock, NULL);
  *mutator = add_mutator(*heap);
  return 1;
}

static struct mutator* initialize_gc_for_thread(uintptr_t *stack_base,
                                                struct heap *heap) {
  pthread_mutex_lock(&heap->lock);
  if (!heap->multithreaded) {
    GC_allow_register_threads();
    heap->multithreaded = 1;
  }
  pthread_mutex_unlock(&heap->lock);

  struct GC_stack_base base = { stack_base };
  GC_register_my_thread(&base);
  return add_mutator(heap);
}
static void finish_gc_for_thread(struct mutator *mut) {
  GC_unregister_my_thread();
}

static inline void print_start_gc_stats(struct heap *heap) {
}
static inline void print_end_gc_stats(struct heap *heap) {
  printf("Completed %ld collections\n", (long)GC_get_gc_no());
  printf("Heap size is %ld\n", (long)GC_get_heap_size());
}
