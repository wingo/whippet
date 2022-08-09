#ifndef GC_API_H_
#define GC_API_H_

#include <stdint.h>

#ifndef GC_DEBUG
#define GC_DEBUG 0
#endif

#define GC_UNLIKELY(e) __builtin_expect(e, 0)
#define GC_LIKELY(e) __builtin_expect(e, 1)

#if GC_DEBUG
#define GC_ASSERT(x) do { if (GC_UNLIKELY(!(x))) __builtin_trap(); } while (0)
#else
#define GC_ASSERT(x) do { } while (0)
#endif

struct gc_ref {
  uintptr_t value;
};

static inline struct gc_ref gc_ref(uintptr_t value) {
  return (struct gc_ref){value};
}
static inline uintptr_t gc_ref_value(struct gc_ref ref) {
  return ref.value;
}

static inline struct gc_ref gc_ref_null(void) {
  return gc_ref(0);
}
static inline int gc_ref_is_heap_object(struct gc_ref ref) {
  return ref.value != 0;
}
static inline struct gc_ref gc_ref_from_heap_object_or_null(void *obj) {
  return gc_ref((uintptr_t) obj);
}
static inline struct gc_ref gc_ref_from_heap_object(void *obj) {
  GC_ASSERT(obj);
  return gc_ref_from_heap_object_or_null(obj);
}
static inline void* gc_ref_heap_object(struct gc_ref ref) {
  GC_ASSERT(gc_ref_is_heap_object(ref));
  return (void *) gc_ref_value(ref);
}

struct gc_edge {
  struct gc_ref *dst;
};

static inline struct gc_edge gc_edge(void* addr) {
  return (struct gc_edge){addr};
}
static struct gc_ref gc_edge_ref(struct gc_edge edge) {
  return *edge.dst;
}
static inline void gc_edge_update(struct gc_edge edge, struct gc_ref ref) {
  *edge.dst = ref;
}

// FIXME: prefix with gc_
struct heap;
struct mutator;

enum {
  GC_OPTION_FIXED_HEAP_SIZE,
  GC_OPTION_PARALLELISM
};

struct gc_option {
  int option;
  double value;
};

// FIXME: Conflict with bdw-gc GC_API.  Switch prefix?
#ifndef GC_API_
#define GC_API_ static
#endif

GC_API_ int gc_option_from_string(const char *str);
GC_API_ int gc_init(int argc, struct gc_option argv[],
                    struct heap **heap, struct mutator **mutator);

#endif // GC_API_H_
