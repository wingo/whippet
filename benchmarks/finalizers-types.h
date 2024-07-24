#ifndef FINALIZERS_TYPES_H
#define FINALIZERS_TYPES_H

#define FOR_EACH_HEAP_OBJECT_KIND(M) \
  M(pair, Pair, PAIR) \
  M(finalizer, Finalizer, FINALIZER) \
  M(small_object, SmallObject, SMALL_OBJECT)

#include "heap-objects.h"
#include "simple-tagging-scheme.h"

struct SmallObject {
  struct gc_header header;
};

struct Pair {
  struct gc_header header;
  void *car;
  void *cdr;
};

#endif // FINALIZERS_TYPES_H
