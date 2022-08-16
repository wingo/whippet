#ifndef GCBENCH_TYPES_H
#define GCBENCH_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define FOR_EACH_HEAP_OBJECT_KIND(M) \
  M(node, Node, NODE) \
  M(double_array, DoubleArray, DOUBLE_ARRAY) \
  M(hole, Hole, HOLE)

#include "heap-objects.h"
#include "simple-tagging-scheme.h"

struct Node {
  struct gc_header header;
  struct Node *left;
  struct Node *right;
  int i, j;
};

struct DoubleArray {
  struct gc_header header;
  size_t length;
  double values[0];
};

struct Hole {
  struct gc_header header;
  size_t length;
  uintptr_t values[0];
};

#endif // GCBENCH_TYPES_H
