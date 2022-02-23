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

static Node* allocate_node(void) {
  // memset to 0 by the collector.
  return GC_malloc (sizeof (Node));
}

static double* allocate_double_array(size_t size) {
  // note, not memset to 0 by the collector.
  return GC_malloc_atomic (sizeof (double) * size);
}

struct handle {
  void *v;
};

#define HANDLE_TO(T) union { T* v; struct handle handle; }
#define HANDLE_REF(h) h.v
#define HANDLE_SET(h,val) do { h.v = val; } while (0)
#define PUSH_HANDLE(h) push_handle(&h.handle)
#define POP_HANDLE(h) pop_handle(&h.handle)

typedef HANDLE_TO(Node) NodeHandle;

static inline void push_handle(struct handle *handle) {
}

static inline void pop_handle(struct handle *handle) {
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

static inline void initialize_gc(void) {
  // GC_full_freq = 30;
  // GC_free_space_divisor = 16;
  // GC_enable_incremental();
}

static inline void print_start_gc_stats(void) {
}

static inline void print_end_gc_stats(void) {
  printf("Completed %ld collections\n", (long)GC_get_gc_no());
  printf("Heap size is %ld\n", (long)GC_get_heap_size());
}
