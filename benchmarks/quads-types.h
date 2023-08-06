#ifndef QUADS_TYPES_H
#define QUADS_TYPES_H

#define FOR_EACH_HEAP_OBJECT_KIND(M) \
  M(quad, Quad, QUAD)

#include "heap-objects.h"
#include "simple-tagging-scheme.h"

struct Quad {
  struct gc_header header;
  struct Quad *kids[4];
};

#endif // QUADS_TYPES_H
