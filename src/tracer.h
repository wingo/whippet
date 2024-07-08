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

////////////////////////////////////////////////////////////////////////
/// To be implemented by tracer.
////////////////////////////////////////////////////////////////////////

// The tracer struct itself should be defined by the tracer
// implementation.
struct gc_tracer;

// Initialize the tracer when the heap is created.
static int gc_tracer_init(struct gc_tracer *tracer, struct gc_heap *heap,
                          size_t parallelism);

// Initialize the tracer for a new GC cycle.
static void gc_tracer_prepare(struct gc_tracer *tracer);

// Release any resources allocated during the trace.
static void gc_tracer_release(struct gc_tracer *tracer);

// Add root objects to the trace.  Call before tracer_trace.
static inline void gc_tracer_enqueue_root(struct gc_tracer *tracer,
                                          struct gc_ref obj);
static inline void gc_tracer_enqueue_roots(struct gc_tracer *tracer,
                                           struct gc_ref *objs,
                                           size_t count);

// Given that an object has been shaded grey, enqueue for tracing.
static inline void gc_tracer_enqueue(struct gc_tracer *tracer,
                                     struct gc_ref ref,
                                     void *trace_data) GC_ALWAYS_INLINE;

// Run the full trace.
static inline void gc_tracer_trace(struct gc_tracer *tracer);

#endif // TRACER_H
