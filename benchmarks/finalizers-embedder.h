#ifndef FINALIZERS_EMBEDDER_H
#define FINALIZERS_EMBEDDER_H

#include <stddef.h>

#include "finalizers-types.h"
#include "gc-finalizer.h"

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

static inline size_t small_object_size(SmallObject *obj) { return sizeof(*obj); }
static inline size_t finalizer_size(Finalizer *obj) { return gc_finalizer_size(); }
static inline size_t pair_size(Pair *obj) { return sizeof(*obj); }

static inline void
visit_small_object_fields(SmallObject *obj,
                          void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                        void *visit_data),
                          struct gc_heap *heap,
                          void *visit_data) {}

static inline void
visit_finalizer_fields(Finalizer *finalizer,
                       void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                     void *visit_data),

                       struct gc_heap *heap,
                       void *visit_data) {
  gc_trace_finalizer((struct gc_finalizer*)finalizer, visit, heap, visit_data);
}

static inline void
visit_pair_fields(Pair *pair,
                  void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                void *visit_data),
                  struct gc_heap *heap,
                  void *visit_data) {
  visit(gc_edge(&pair->car), heap, visit_data);
  visit(gc_edge(&pair->cdr), heap, visit_data);
}

#include "simple-gc-embedder.h"

#endif // FINALIZERS_EMBEDDER_H
