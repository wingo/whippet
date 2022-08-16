#ifndef PRECISE_ROOTS_EMBEDDER_H
#define PRECISE_ROOTS_EMBEDDER_H

#include "gc-edge.h"
#include "precise-roots-types.h"

static inline void visit_roots(struct handle *roots,
                               void (*trace_edge)(struct gc_edge edge,
                                                  void *trace_data),
                               void *trace_data) {
  for (struct handle *h = roots; h; h = h->next)
    trace_edge(gc_edge(&h->v), trace_data);
}

static inline void gc_trace_mutator_roots(struct gc_mutator_roots *roots,
                                          void (*trace_edge)(struct gc_edge edge,
                                                             void *trace_data),
                                          void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, trace_data);
}

static inline void gc_trace_heap_roots(struct gc_heap_roots *roots,
                                       void (*trace_edge)(struct gc_edge edge,
                                                          void *trace_data),
                                       void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, trace_data);
}

#endif // PRECISE_ROOTS_EMBEDDER_H
