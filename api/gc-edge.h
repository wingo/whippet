#ifndef GC_EDGE_H
#define GC_EDGE_H

#include "gc-ref.h"

struct gc_edge {
  struct gc_ref *dst;
};

static inline struct gc_edge gc_edge(void* addr) {
  return (struct gc_edge){addr};
}
static inline struct gc_ref gc_edge_ref(struct gc_edge edge) {
  return *edge.dst;
}
static inline struct gc_ref* gc_edge_loc(struct gc_edge edge) {
  return edge.dst;
}
static inline uintptr_t gc_edge_address(struct gc_edge edge) {
  return (uintptr_t)gc_edge_loc(edge);
}
static inline void gc_edge_update(struct gc_edge edge, struct gc_ref ref) {
  *edge.dst = ref;
}

#endif // GC_EDGE_H
