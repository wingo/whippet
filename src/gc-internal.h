#ifndef GC_INTERNAL_H
#define GC_INTERNAL_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "embedder-api-impl.h"
#include "gc-ephemeron-internal.h"
#include "gc-finalizer-internal.h"
#include "gc-options-internal.h"

uint64_t gc_heap_total_bytes_allocated(struct gc_heap *heap);
void gc_mutator_adjust_heap_size(struct gc_mutator *mut, uint64_t new_size);


#endif // GC_INTERNAL_H
