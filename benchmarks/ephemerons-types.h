#ifndef EPHEMERONS_TYPES_H
#define EPHEMERONS_TYPES_H

#define FOR_EACH_HEAP_OBJECT_KIND(M) \
  M(box, Box, BOX) \
  M(ephemeron, Ephemeron, EPHEMERON) \
  M(small_object, SmallObject, SMALL_OBJECT)

#include "heap-objects.h"
#include "simple-tagging-scheme.h"

struct SmallObject {
  struct gc_header header;
};

struct Box {
  struct gc_header header;
  void *obj;
};

#endif // EPHEMERONS_TYPES_H
