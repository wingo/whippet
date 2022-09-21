#ifndef PRECISE_ROOTS_EMBEDDER_H
#define PRECISE_ROOTS_EMBEDDER_H

#include "gc-edge.h"
#include "gc-embedder-api.h"
#include "precise-roots-types.h"

static inline int gc_has_mutator_conservative_roots(void) {
  return 0;
}
static inline int gc_mutator_conservative_roots_may_be_interior(void) {
  return 0;
}
static inline int gc_has_global_conservative_roots(void) {
  return 0;
}
static inline int gc_has_conservative_intraheap_edges(void) {
  return 0;
}

static inline void visit_roots(struct handle *roots,
                               void (*trace_edge)(struct gc_edge edge,
                                                  void *trace_data),
                               void *trace_data) {
  for (struct handle *h = roots; h; h = h->next)
    trace_edge(gc_edge(&h->v), trace_data);
}

static inline void gc_trace_precise_mutator_roots(struct gc_mutator_roots *roots,
                                                  void (*trace_edge)(struct gc_edge edge,
                                                                     void *trace_data),
                                                  void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, trace_data);
}

static inline void gc_trace_precise_heap_roots(struct gc_heap_roots *roots,
                                               void (*trace_edge)(struct gc_edge edge,
                                                                  void *trace_data),
                                               void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, trace_data);
}

#endif // PRECISE_ROOTS_EMBEDDER_H
