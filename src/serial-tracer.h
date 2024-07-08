#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "simple-worklist.h"

struct tracer {
  struct simple_worklist worklist;
};

struct gc_heap;
static inline struct tracer* heap_tracer(struct gc_heap *heap);

static int
tracer_init(struct gc_heap *heap, size_t parallelism) {
  return simple_worklist_init(&heap_tracer(heap)->worklist);
}
static void tracer_prepare(struct gc_heap *heap) {}
static void tracer_release(struct gc_heap *heap) {
  simple_worklist_release(&heap_tracer(heap)->worklist);
}

static inline void tracer_visit(struct gc_edge edge, struct gc_heap *heap,
                                void *trace_data) GC_ALWAYS_INLINE;
static inline void tracer_enqueue(struct gc_ref ref, struct gc_heap *heap,
                                  void *trace_data) GC_ALWAYS_INLINE;
static inline void trace_one(struct gc_ref ref, struct gc_heap *heap,
                             void *trace_data) GC_ALWAYS_INLINE;
static inline int trace_edge(struct gc_heap *heap,
                             struct gc_edge edge) GC_ALWAYS_INLINE;

static inline void
tracer_enqueue_root(struct tracer *tracer, struct gc_ref obj) {
  simple_worklist_push(&tracer->worklist, obj);
}
static inline void
tracer_enqueue_roots(struct tracer *tracer, struct gc_ref *objs,
                     size_t count) {
  simple_worklist_push_many(&tracer->worklist, objs, count);
}
static inline void
tracer_enqueue(struct gc_ref ref, struct gc_heap *heap, void *trace_data) {
  tracer_enqueue_root(heap_tracer(heap), ref);
}
static inline void
tracer_visit(struct gc_edge edge, struct gc_heap *heap, void *trace_data) {
  if (trace_edge(heap, edge))
    tracer_enqueue(gc_edge_ref(edge), heap, trace_data);
}
static inline void
tracer_trace(struct gc_heap *heap) {
  do {
    struct gc_ref obj = simple_worklist_pop(&heap_tracer(heap)->worklist);
    if (!gc_ref_is_heap_object(obj))
      break;
    trace_one(obj, heap, NULL);
  } while (1);
}

#endif // SERIAL_TRACER_H
