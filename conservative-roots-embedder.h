#ifndef CONSERVATIVE_ROOTS_EMBEDDER_H
#define CONSERVATIVE_ROOTS_EMBEDDER_H

#include "gc-assert.h"
#include "conservative-roots-types.h"

static inline void gc_trace_mutator_roots(struct gc_mutator_roots *roots,
                                          void (*trace_edge)(struct gc_edge edge,
                                                             void *trace_data),
                                          void *trace_data) {
  GC_CRASH();
}

static inline void gc_trace_heap_roots(struct gc_heap_roots *roots,
                                       void (*trace_edge)(struct gc_edge edge,
                                                          void *trace_data),
                                       void *trace_data) {
  GC_CRASH();
}

#endif // CONSERVATIVE_ROOTS_EMBEDDER_H
