#ifndef TRACER_H
#define TRACER_H

#include "gc-ref.h"
#include "gc-edge.h"

struct gc_heap;

////////////////////////////////////////////////////////////////////////
/// To be implemented by collector.
////////////////////////////////////////////////////////////////////////

// Initialize the tracer when the heap is created.
static inline struct gc_tracer* heap_tracer(struct gc_heap *heap);

// Visit all fields in an object.
static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             void *trace_data) GC_ALWAYS_INLINE;

// Visit one edge.  Return nonzero if this call shaded the object grey.
static inline int trace_edge(struct gc_heap *heap,
                             struct gc_edge edge) GC_ALWAYS_INLINE;

////////////////////////////////////////////////////////////////////////
/// To be implemented by tracer.
////////////////////////////////////////////////////////////////////////

// The tracer struct itself should be defined by the tracer
// implementation.
struct gc_tracer;

// Initialize the tracer when the heap is created.
static int gc_tracer_init(struct gc_heap *heap, size_t parallelism);

// Initialize the tracer for a new GC cycle.
static void gc_tracer_prepare(struct gc_heap *heap);

// Release any resources allocated during the trace.
static void gc_tracer_release(struct gc_heap *heap);

// Add root objects to the trace.  Call before tracer_trace.
static inline void gc_tracer_enqueue_root(struct gc_tracer *tracer,
                                          struct gc_ref obj);
static inline void gc_tracer_enqueue_roots(struct gc_tracer *tracer,
                                           struct gc_ref *objs,
                                           size_t count);

// Given that an object has been shaded grey, enqueue for tracing.
static inline void gc_tracer_enqueue(struct gc_ref ref, struct gc_heap *heap,
                                     void *trace_data) GC_ALWAYS_INLINE;

// Run the full trace.
static inline void gc_tracer_trace(struct gc_heap *heap);

////////////////////////////////////////////////////////////////////////
/// Procedures that work with any tracer.
////////////////////////////////////////////////////////////////////////

// Visit one edge.  If we shade the edge target grey, enqueue it for
// tracing.
static inline void tracer_visit(struct gc_edge edge, struct gc_heap *heap,
                                void *trace_data) GC_ALWAYS_INLINE;
static inline void
tracer_visit(struct gc_edge edge, struct gc_heap *heap, void *trace_data) {
  if (trace_edge(heap, edge))
    gc_tracer_enqueue(gc_edge_ref(edge), heap, trace_data);
}

#endif // TRACER_H
