#ifndef GC_EPHEMERON_H_
#define GC_EPHEMERON_H_

#include "gc-edge.h"
#include "gc-ref.h"
#include "gc-visibility.h"

// Ephemerons establish an association between a "key" object and a
// "value" object.  If the ephemeron and the key are live, then the
// value is live, and can be retrieved from the ephemeron.  Ephemerons
// can be chained together, which allows them to function as links in a
// buckets-and-chains hash table.
//
// This file defines the user-facing API for ephemerons.

struct gc_heap;
struct gc_mutator;
struct gc_ephemeron;

GC_API_ size_t gc_ephemeron_size(void);
GC_API_ struct gc_ephemeron* gc_allocate_ephemeron(struct gc_mutator *mut);
GC_API_ void gc_ephemeron_init(struct gc_mutator *mut,
                               struct gc_ephemeron *ephemeron,
                               struct gc_ref key, struct gc_ref value);

GC_API_ struct gc_ref gc_ephemeron_key(struct gc_ephemeron *ephemeron);
GC_API_ struct gc_ref gc_ephemeron_value(struct gc_ephemeron *ephemeron);

GC_API_ struct gc_ephemeron* gc_ephemeron_chain_head(struct gc_ephemeron **loc);
GC_API_ void gc_ephemeron_chain_push(struct gc_ephemeron **loc,
                                     struct gc_ephemeron *ephemeron);
GC_API_ struct gc_ephemeron* gc_ephemeron_chain_next(struct gc_ephemeron *ephemeron);
GC_API_ void gc_ephemeron_mark_dead(struct gc_ephemeron *ephemeron);

GC_API_ void gc_trace_ephemeron(struct gc_ephemeron *ephemeron,
                                void (*visit)(struct gc_edge edge,
                                              struct gc_heap *heap,
                                              void *visit_data),
                                struct gc_heap *heap,
                                void *trace_data);

#endif // GC_EPHEMERON_H_
