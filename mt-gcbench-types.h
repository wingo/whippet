#ifndef GCBENCH_TYPES_H
#define GCBENCH_TYPES_H

#define FOR_EACH_HEAP_OBJECT_KIND(M) \
  M(node, Node, NODE) \
  M(double_array, DoubleArray, DOUBLE_ARRAY) \
  M(hole, Hole, HOLE)

#include "heap-objects.h"

#endif // GCBENCH_TYPES_H
