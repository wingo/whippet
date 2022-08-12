#ifndef HEAP_OBJECTS_H
#define HEAP_OBJECTS_H

#include "gc-inline.h"
#include "gc-edge.h"

#define DECLARE_NODE_TYPE(name, Name, NAME) \
  struct Name;                              \
  typedef struct Name Name;
FOR_EACH_HEAP_OBJECT_KIND(DECLARE_NODE_TYPE)
#undef DECLARE_NODE_TYPE

#define DEFINE_ENUM(name, Name, NAME) ALLOC_KIND_##NAME,
enum alloc_kind {
  FOR_EACH_HEAP_OBJECT_KIND(DEFINE_ENUM)
};
#undef DEFINE_ENUM

#define DEFINE_METHODS(name, Name, NAME) \
  static inline size_t name##_size(Name *obj) GC_ALWAYS_INLINE; \
  static inline void visit_##name##_fields(Name *obj,\
                                           void (*visit)(struct gc_edge edge, void *visit_data), \
                                           void *visit_data) GC_ALWAYS_INLINE;
FOR_EACH_HEAP_OBJECT_KIND(DEFINE_METHODS)
#undef DEFINE_METHODS

#endif // HEAP_OBJECTS_H
