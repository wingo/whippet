#ifndef PRECISE_ROOTS_TYPES_H
#define PRECISE_ROOTS_TYPES_H

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

#endif // PRECISE_ROOTS_TYPES_H
