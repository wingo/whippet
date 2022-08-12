#include <stdint.h>
#include <string.h>

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
                                 enum gc_inline_kind kind) GC_NEVER_INLINE;
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

static inline void* gc_allocate(struct mutator *mut, size_t size) {
  size_t idx = gc_inline_bytes_to_freelist_index(size);

  if (UNLIKELY(idx >= GC_INLINE_FREELIST_COUNT))
    return GC_malloc(size);

  return allocate_small(&mut->freelists[idx], idx, GC_INLINE_KIND_NORMAL);
}

static inline void* gc_allocate_pointerless(struct mutator *mut,
                                            size_t size) {
  // Because the BDW API requires us to implement a custom marker so
  // that the pointerless freelist gets traced, even though it's in a
  // pointerless region, we punt on thread-local pointerless freelists.
  return GC_malloc_atomic(size);
}

static inline void collect(struct mutator *mut) {
  GC_gcollect();
}

static inline void init_field(void *obj, void **addr, void *val) {
  *addr = val;
}
static inline void set_field(void *obj, void **addr, void *val) {
  *addr = val;
}

static inline struct mutator *add_mutator(struct heap *heap) {
  struct mutator *ret = GC_malloc(sizeof(struct mutator));
  ret->heap = heap;
  return ret;
}

static inline struct heap *mutator_heap(struct mutator *mutator) {
  return mutator->heap;
}

#define FOR_EACH_GC_OPTION(M) \
  M(GC_OPTION_FIXED_HEAP_SIZE, "fixed-heap-size") \
  M(GC_OPTION_PARALLELISM, "parallelism")

static void dump_available_gc_options(void) {
  fprintf(stderr, "available gc options:");
#define PRINT_OPTION(option, name) fprintf(stderr, " %s", name);
  FOR_EACH_GC_OPTION(PRINT_OPTION)
#undef PRINT_OPTION
  fprintf(stderr, "\n");
}

static int gc_option_from_string(const char *str) {
#define PARSE_OPTION(option, name) if (strcmp(str, name) == 0) return option;
  FOR_EACH_GC_OPTION(PARSE_OPTION)
#undef PARSE_OPTION
  if (strcmp(str, "fixed-heap-size") == 0)
    return GC_OPTION_FIXED_HEAP_SIZE;
  if (strcmp(str, "parallelism") == 0)
    return GC_OPTION_PARALLELISM;
  fprintf(stderr, "bad gc option: '%s'\n", str);
  dump_available_gc_options();
  return -1;
}

struct options {
  size_t fixed_heap_size;
  size_t parallelism;
};

static size_t parse_size_t(double value) {
  ASSERT(value >= 0);
  ASSERT(value <= (size_t) -1);
  return value;
}

static size_t number_of_current_processors(void) { return 1; }

static int parse_options(int argc, struct gc_option argv[],
                         struct options *options) {
  for (int i = 0; i < argc; i++) {
    switch (argv[i].option) {
    case GC_OPTION_FIXED_HEAP_SIZE:
      options->fixed_heap_size = parse_size_t(argv[i].value);
      break;
    case GC_OPTION_PARALLELISM:
      options->parallelism = parse_size_t(argv[i].value);
      break;
    default:
      abort();
    }
  }

  if (!options->fixed_heap_size) {
    fprintf(stderr, "fixed heap size is currently required\n");
    return 0;
  }
  if (!options->parallelism)
    options->parallelism = number_of_current_processors();

  return 1;
}

static int gc_init(int argc, struct gc_option argv[],
                   struct heap **heap, struct mutator **mutator) {
  struct options options = { 0, };
  if (!parse_options(argc, argv, &options))
    return 0;

  // GC_full_freq = 30;
  // GC_free_space_divisor = 16;
  // GC_enable_incremental();
  
  GC_set_max_heap_size(options.fixed_heap_size);
  // Not part of 7.3, sigh.  Have to set an env var.
  // GC_set_markers_count(options.parallelism);
  char markers[21] = {0,}; // 21 bytes enough for 2**64 in decimal + NUL.
  snprintf(markers, sizeof(markers), "%zu", options.parallelism);
  setenv("GC_MARKERS", markers, 1);
  GC_init();
  size_t current_heap_size = GC_get_heap_size();
  if (options.fixed_heap_size > current_heap_size)
    GC_expand_hp(options.fixed_heap_size - current_heap_size);
  GC_allow_register_threads();
  *heap = GC_malloc(sizeof(struct heap));
  pthread_mutex_init(&(*heap)->lock, NULL);
  *mutator = add_mutator(*heap);
  return 1;
}

static struct mutator* gc_init_for_thread(uintptr_t *stack_base,
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
static void gc_finish_for_thread(struct mutator *mut) {
  GC_unregister_my_thread();
}

static void* gc_call_without_gc(struct mutator *mut,
                                void* (*f)(void*),
                                void *data) {
  return GC_do_blocking(f, data);
}

static inline void print_start_gc_stats(struct heap *heap) {
}
static inline void print_end_gc_stats(struct heap *heap) {
  printf("Completed %ld collections\n", (long)GC_get_gc_no());
  printf("Heap size is %ld\n", (long)GC_get_heap_size());
}
