#ifndef GC_TYPES_H_
#define GC_TYPES_H_

struct gc_edge {
  union {
    void *addr;
    void **loc;
  };
};

static inline struct gc_edge gc_edge(void* addr) {
  struct gc_edge edge;
  edge.addr = addr;
  return edge;
}
static inline struct gc_edge object_field(void* addr) {
  return gc_edge(addr);
}
static inline void* dereference_edge(struct gc_edge edge) {
  return *edge.loc;
}
static inline void update_edge(struct gc_edge edge, void *value) {
  *edge.loc = value;
}

#endif // GC_TYPES_H_
