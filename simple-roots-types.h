#ifndef SIMPLE_ROOTS_TYPES_H
#define SIMPLE_ROOTS_TYPES_H

struct handle {
  void *v;
  struct handle *next;
};

struct gc_heap_roots {
  struct handle *roots;
};

struct gc_mutator_roots {
  struct handle *roots;
};

#endif // SIMPLE_ROOTS_TYPES_H
