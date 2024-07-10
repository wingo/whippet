#ifndef ROOT_H
#define ROOT_H

struct gc_ephemeron;
struct gc_heap;
struct gc_mutator;

enum gc_root_kind {
  GC_ROOT_KIND_NONE,
  GC_ROOT_KIND_HEAP,
  GC_ROOT_KIND_MUTATOR,
  GC_ROOT_KIND_RESOLVED_EPHEMERONS
};

struct gc_root {
  enum gc_root_kind kind;
  union {
    struct gc_heap *heap;
    struct gc_mutator *mutator;
    struct gc_ephemeron *resolved_ephemerons;
  };
};

static inline struct gc_root gc_root_heap(struct gc_heap* heap) {
  struct gc_root ret = { GC_ROOT_KIND_HEAP };
  ret.heap = heap;
  return ret;
}

static inline struct gc_root gc_root_mutator(struct gc_mutator* mutator) {
  struct gc_root ret = { GC_ROOT_KIND_MUTATOR };
  ret.mutator = mutator;
  return ret;
}

static inline struct gc_root
gc_root_resolved_ephemerons(struct gc_ephemeron* resolved) {
  struct gc_root ret = { GC_ROOT_KIND_RESOLVED_EPHEMERONS };
  ret.resolved_ephemerons = resolved;
  return ret;
}

#endif // ROOT_H
