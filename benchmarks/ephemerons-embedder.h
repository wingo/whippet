#ifndef EPHEMERONS_EMBEDDER_H
#define EPHEMERONS_EMBEDDER_H

#include <stddef.h>

#include "ephemerons-types.h"
#include "gc-ephemeron.h"

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
static inline size_t ephemeron_size(Ephemeron *obj) { return gc_ephemeron_size(); }
static inline size_t box_size(Box *obj) { return sizeof(*obj); }

static inline void
visit_small_object_fields(SmallObject *obj,
                          void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                        void *visit_data),
                          struct gc_heap *heap,
                          void *visit_data) {}

static inline void
visit_ephemeron_fields(Ephemeron *ephemeron,
                       void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                                     void *visit_data),

                       struct gc_heap *heap,
                       void *visit_data) {
  gc_trace_ephemeron((struct gc_ephemeron*)ephemeron, visit, heap, visit_data);
}

static inline void
visit_box_fields(Box *box,
                 void (*visit)(struct gc_edge edge, struct gc_heap *heap,
                               void *visit_data),
                 struct gc_heap *heap,
                 void *visit_data) {
  visit(gc_edge(&box->obj), heap, visit_data);
}

#include "simple-gc-embedder.h"

#endif // EPHEMERONS_EMBEDDER_H
