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

struct context {};

enum alloc_kind { NODE, DOUBLE_ARRAY };

typedef void (*field_visitor)(struct context *, void **ref);

#define GC_HEADER /**/

static inline void* allocate(struct context *cx, enum alloc_kind kind,
                             size_t size) {
  // memset to 0 by the collector.
  switch (kind) {
  case NODE:
    return GC_malloc(size);
  case DOUBLE_ARRAY:
    return GC_malloc_atomic(size);
  }
  abort();
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

static inline void initialize_gc(struct context* cx, size_t heap_size) {
  // GC_full_freq = 30;
  // GC_free_space_divisor = 16;
  // GC_enable_incremental();
  GC_INIT();
  size_t current_heap_size = GC_get_heap_size();
  if (heap_size > current_heap_size) {
    GC_set_max_heap_size (heap_size);
    GC_expand_hp(heap_size - current_heap_size);
  }
}

static inline void print_start_gc_stats(struct context *cx) {
}

static inline void print_end_gc_stats(struct context *cx) {
  printf("Completed %ld collections\n", (long)GC_get_gc_no());
  printf("Heap size is %ld\n", (long)GC_get_heap_size());
}
