#ifndef ROOT_H
#define ROOT_H

#include "gc-edge.h"
#include "extents.h"

struct gc_ephemeron;
struct gc_heap;
struct gc_mutator;

enum gc_root_kind {
  GC_ROOT_KIND_NONE,
  GC_ROOT_KIND_HEAP,
  GC_ROOT_KIND_MUTATOR,
  GC_ROOT_KIND_CONSERVATIVE_EDGES,
  GC_ROOT_KIND_CONSERVATIVE_POSSIBLY_INTERIOR_EDGES,
  GC_ROOT_KIND_RESOLVED_EPHEMERONS,
  GC_ROOT_KIND_EDGE,
  GC_ROOT_KIND_REMEMBERED_OBJECT,
  GC_ROOT_KIND_REMEMBERED_SLAB,
};

struct gc_root {
  enum gc_root_kind kind;
  union {
    struct gc_heap *heap;
    struct gc_mutator *mutator;
    struct gc_ephemeron *resolved_ephemerons;
    struct extent_range range;
    struct gc_edge edge;
    struct gc_ref ref;
    size_t idx;
  };
};

static inline struct gc_root
gc_root_heap(struct gc_heap* heap) {
  struct gc_root ret = { GC_ROOT_KIND_HEAP };
  ret.heap = heap;
  return ret;
}

static inline struct gc_root
gc_root_mutator(struct gc_mutator* mutator) {
  struct gc_root ret = { GC_ROOT_KIND_MUTATOR };
  ret.mutator = mutator;
  return ret;
}

static inline struct gc_root
gc_root_conservative_edges(uintptr_t lo_addr, uintptr_t hi_addr,
                           int possibly_interior) {
  enum gc_root_kind kind = possibly_interior
    ? GC_ROOT_KIND_CONSERVATIVE_POSSIBLY_INTERIOR_EDGES
    : GC_ROOT_KIND_CONSERVATIVE_EDGES;
  struct gc_root ret = { kind };
  ret.range = (struct extent_range) {lo_addr, hi_addr};
  return ret;
}

static inline struct gc_root
gc_root_resolved_ephemerons(struct gc_ephemeron* resolved) {
  struct gc_root ret = { GC_ROOT_KIND_RESOLVED_EPHEMERONS };
  ret.resolved_ephemerons = resolved;
  return ret;
}

static inline struct gc_root
gc_root_edge(struct gc_edge edge) {
  struct gc_root ret = { GC_ROOT_KIND_EDGE };
  ret.edge = edge;
  return ret;
}

static inline struct gc_root
gc_root_remembered_object(struct gc_ref ref) {
  struct gc_root ret = { GC_ROOT_KIND_REMEMBERED_OBJECT };
  ret.ref = ref;
  return ret;
}

static inline struct gc_root
gc_root_remembered_slab(size_t idx) {
  struct gc_root ret = { GC_ROOT_KIND_REMEMBERED_SLAB };
  ret.idx = idx;
  return ret;
}

#endif // ROOT_H
