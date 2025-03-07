#ifndef GC_TRACE_H
#define GC_TRACE_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-conservative-ref.h"
#include "gc-embedder-api.h"

static inline int gc_has_mutator_conservative_roots(void) {
  return GC_CONSERVATIVE_ROOTS;
}
static inline int gc_mutator_conservative_roots_may_be_interior(void) {
  return 1;
}
static inline int gc_has_global_conservative_roots(void) {
  return GC_CONSERVATIVE_ROOTS;
}
static inline int gc_has_conservative_intraheap_edges(void) {
  return GC_CONSERVATIVE_TRACE;
}

static inline int gc_has_conservative_roots(void) {
  return gc_has_mutator_conservative_roots() ||
    gc_has_global_conservative_roots();
}

enum gc_trace_kind {
  GC_TRACE_PRECISELY,
  GC_TRACE_NONE,
  GC_TRACE_CONSERVATIVELY,
  GC_TRACE_EPHEMERON,
};

struct gc_trace_plan {
  enum gc_trace_kind kind;
  size_t size; // For conservative tracing.
};

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

#endif // GC_TRACE_H
