#ifndef GC_EPHEMERON_INTERNAL_H
#define GC_EPHEMERON_INTERNAL_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "gc-ephemeron.h"

struct gc_pending_ephemerons;

// API implemented by collector, for use by ephemerons:
GC_INTERNAL int gc_visit_ephemeron_key(struct gc_edge edge,
                                       struct gc_heap *heap);
GC_INTERNAL struct gc_pending_ephemerons*
gc_heap_pending_ephemerons(struct gc_heap *heap);
GC_INTERNAL unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap);

// API implemented by ephemerons, for use by collector:
GC_INTERNAL struct gc_edge gc_ephemeron_key_edge(struct gc_ephemeron *eph);
GC_INTERNAL struct gc_edge gc_ephemeron_value_edge(struct gc_ephemeron *eph);

GC_INTERNAL struct gc_pending_ephemerons*
gc_prepare_pending_ephemerons(struct gc_pending_ephemerons *state,
                              size_t target_size, double slop);

GC_INTERNAL void
gc_resolve_pending_ephemerons(struct gc_ref obj, struct gc_heap *heap);

GC_INTERNAL void
gc_scan_pending_ephemerons(struct gc_pending_ephemerons *state,
                           struct gc_heap *heap, size_t shard,
                           size_t nshards);

GC_INTERNAL struct gc_ephemeron*
gc_pop_resolved_ephemerons(struct gc_heap *heap);

GC_INTERNAL void
gc_trace_resolved_ephemerons(struct gc_ephemeron *resolved,
                             void (*visit)(struct gc_edge edge,
                                           struct gc_heap *heap,
                                           void *visit_data),
                             struct gc_heap *heap,
                             void *trace_data);

GC_INTERNAL void
gc_sweep_pending_ephemerons(struct gc_pending_ephemerons *state,
                            size_t shard, size_t nshards);

GC_INTERNAL void gc_ephemeron_init_internal(struct gc_heap *heap,
                                            struct gc_ephemeron *ephemeron,
                                            struct gc_ref key,
                                            struct gc_ref value);

#endif // GC_EPHEMERON_INTERNAL_H
