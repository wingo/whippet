#ifndef CONSERVATIVE_ROOTS_EMBEDDER_H
#define CONSERVATIVE_ROOTS_EMBEDDER_H

#include "gc-assert.h"
#include "conservative-roots-types.h"

static inline int gc_has_conservative_roots(void) {
  return 1;
}
static inline int gc_has_conservative_intraheap_edges(void) {
  // FIXME: Implement both ways.
  return 0;
}

static inline void gc_trace_conservative_mutator_roots(struct gc_mutator_roots *roots,
                                                       void (*trace_ref)(struct gc_ref edge,
                                                                         void *trace_data),
                                                       void *trace_data) {
}

static inline void gc_trace_precise_mutator_roots(struct gc_mutator_roots *roots,
                                                  void (*trace_edge)(struct gc_edge edge,
                                                                     void *trace_data),
                                                  void *trace_data) {
}

static inline void gc_trace_conservative_heap_roots(struct gc_heap_roots *roots,
                                                    void (*trace_ref)(struct gc_ref ref,
                                                                      void *trace_data),
                                                    void *trace_data) {
}

static inline void gc_trace_precise_heap_roots(struct gc_heap_roots *roots,
                                               void (*trace_edge)(struct gc_edge edge,
                                                                  void *trace_data),
                                               void *trace_data) {
}

#endif // CONSERVATIVE_ROOTS_EMBEDDER_H
