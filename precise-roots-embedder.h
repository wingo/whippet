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

static inline int
gc_is_valid_conservative_ref_displacement(uintptr_t displacement) {
  GC_CRASH();
}
static inline int
gc_conservative_ref_might_be_a_heap_object(struct gc_conservative_ref ref,
                                           int possibly_interior) {
  GC_CRASH();
}

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
