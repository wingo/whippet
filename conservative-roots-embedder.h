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

static inline int
gc_is_valid_conservative_ref_displacement(uintptr_t displacement) {
  // Here is where you would allow tagged heap object references.
  return displacement == 0;
}
static inline int
gc_conservative_ref_might_be_a_heap_object(struct gc_conservative_ref ref,
                                           int possibly_interior) {
  // Assume that the minimum page size is 4096, and that the first page
  // will contain no heap objects.
  if (gc_conservative_ref_value(ref) < 4096)
    return 0;
  if (possibly_interior)
    return 1;
  return gc_is_valid_conservative_ref_displacement
    (gc_conservative_ref_value(ref) & (sizeof(uintptr_t) - 1));
}

static inline void gc_trace_mutator_roots(struct gc_mutator_roots *roots,
                                          void (*trace_edge)(struct gc_edge edge,
                                                             struct gc_heap *heap,
                                                             void *trace_data),
                                          struct gc_heap *heap,
                                          void *trace_data) {
}

static inline void gc_trace_heap_roots(struct gc_heap_roots *roots,
                                       void (*trace_edge)(struct gc_edge edge,
                                                          struct gc_heap *heap,
                                                          void *trace_data),
                                       struct gc_heap *heap,
                                       void *trace_data) {
}

#endif // CONSERVATIVE_ROOTS_EMBEDDER_H
