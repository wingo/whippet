#ifndef MT_GCBENCH_EMBEDDER_H
#define MT_GCBENCH_EMBEDDER_H

#include "gc-config.h"
#include "mt-gcbench-types.h"

struct gc_heap;

#define DEFINE_METHODS(name, Name, NAME) \
  static inline size_t name##_size(Name *obj) GC_ALWAYS_INLINE; \
  static inline void visit_##name##_fields(Name *obj,\
                                           void (*visit)(struct gc_edge edge, \
                                                         struct gc_heap *heap, \
                                                         void *visit_data), \
                                           struct gc_heap *heap,        \
                                           void *visit_data) GC_ALWAYS_INLINE;
FOR_EACH_HEAP_OBJECT_KIND(DEFINE_METHODS)
#undef DEFINE_METHODS

static inline size_t node_size(Node *obj) {
  return sizeof(Node);
}
static inline size_t double_array_size(DoubleArray *array) {
  return sizeof(*array) + array->length * sizeof(double);
}
static inline size_t hole_size(Hole *hole) {
  return sizeof(*hole) + hole->length * sizeof(uintptr_t);
}
static inline void
visit_node_fields(Node *node,
                  void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                void *visit_data),
                  struct gc_heap *heap, void *visit_data) {
  visit(gc_edge(&node->left), heap, visit_data);
  visit(gc_edge(&node->right), heap, visit_data);
}
static inline void
visit_double_array_fields(DoubleArray *obj,
                          void (*visit)(struct gc_edge edge,
                                        struct gc_heap *heap, void *visit_data),
                          struct gc_heap *heap, void *visit_data) {
}
static inline void
visit_hole_fields(Hole *obj,
                  void (*visit)(struct gc_edge edge,
                                struct gc_heap *heap, void *visit_data),
                  struct gc_heap *heap, void *visit_data) {
  if (GC_PRECISE_ROOTS)
    GC_CRASH();
}

#include "simple-gc-embedder.h"

#endif // MT_GCBENCH_EMBEDDER_H
