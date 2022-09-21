#ifndef CONSERVATIVE_ROOTS_EMBEDDER_H
#define CONSERVATIVE_ROOTS_EMBEDDER_H

#include "gc-embedder-api.h"

static inline int gc_has_mutator_conservative_roots(void) {
  return 1;
}
static inline int gc_mutator_conservative_roots_may_be_interior(void) {
  return 1;
}
static inline int gc_has_global_conservative_roots(void) {
  return 1;
}
static inline int gc_has_conservative_intraheap_edges(void) {
  return 0;
}

static inline void gc_trace_precise_mutator_roots(struct gc_mutator_roots *roots,
                                                  void (*trace_edge)(struct gc_edge edge,
                                                                     void *trace_data),
                                                  void *trace_data) {
}

static inline void gc_trace_precise_heap_roots(struct gc_heap_roots *roots,
                                               void (*trace_edge)(struct gc_edge edge,
                                                                  void *trace_data),
                                               void *trace_data) {
}

#endif // CONSERVATIVE_ROOTS_EMBEDDER_H
