#ifndef TRACER_H
#define TRACER_H

#include "gc-ref.h"
#include "gc-edge.h"
#include "root.h"

struct gc_heap;

// Data types to be implemented by tracer.
struct gc_tracer;
struct gc_trace_worker;
// Data types to be implemented by collector.
struct gc_trace_worker_data;

////////////////////////////////////////////////////////////////////////
/// To be implemented by collector.
////////////////////////////////////////////////////////////////////////

// Visit all fields in an object.
static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             struct gc_trace_worker *worker) GC_ALWAYS_INLINE;
static inline void trace_root(struct gc_root root, struct gc_heap *heap,
                              struct gc_trace_worker *worker) GC_ALWAYS_INLINE;

static void
gc_trace_worker_call_with_data(void (*f)(struct gc_tracer *tracer,
                                         struct gc_heap *heap,
                                         struct gc_trace_worker *worker,
                                         struct gc_trace_worker_data *data),
                               struct gc_tracer *tracer,
                               struct gc_heap *heap,
                               struct gc_trace_worker *worker);

////////////////////////////////////////////////////////////////////////
/// To be implemented by tracer.
////////////////////////////////////////////////////////////////////////

// Initialize the tracer when the heap is created.
static int gc_tracer_init(struct gc_tracer *tracer, struct gc_heap *heap,
                          size_t parallelism);

// Initialize the tracer for a new GC cycle.
static void gc_tracer_prepare(struct gc_tracer *tracer);

// Release any resources allocated during the trace.
static void gc_tracer_release(struct gc_tracer *tracer);

// Add root objects to the trace.  Call before tracer_trace.
static inline void gc_tracer_add_root(struct gc_tracer *tracer,
                                      struct gc_root root);

// Given that an object has been shaded grey, enqueue for tracing.
static inline void gc_trace_worker_enqueue(struct gc_trace_worker *worker,
                                           struct gc_ref ref) GC_ALWAYS_INLINE;
static inline struct gc_trace_worker_data*
gc_trace_worker_data(struct gc_trace_worker *worker) GC_ALWAYS_INLINE;

// Just trace roots.
static inline void gc_tracer_trace_roots(struct gc_tracer *tracer);

// Run the full trace, including roots.
static inline void gc_tracer_trace(struct gc_tracer *tracer);

#endif // TRACER_H
