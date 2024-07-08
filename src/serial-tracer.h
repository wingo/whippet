#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "simple-worklist.h"
#include "tracer.h"

struct gc_tracer {
  struct gc_heap *heap;
  struct simple_worklist worklist;
};

struct gc_trace_worker {
  struct gc_tracer *tracer;
  struct gc_trace_worker_data *data;
};

static int
gc_tracer_init(struct gc_tracer *tracer, struct gc_heap *heap,
               size_t parallelism) {
  tracer->heap = heap;
  return simple_worklist_init(&tracer->worklist);
}
static void gc_tracer_prepare(struct gc_tracer *tracer) {}
static void gc_tracer_release(struct gc_tracer *tracer) {
  simple_worklist_release(&tracer->worklist);
}

static inline void
gc_tracer_enqueue_root(struct gc_tracer *tracer, struct gc_ref obj) {
  simple_worklist_push(&tracer->worklist, obj);
}
static inline void
gc_tracer_enqueue_roots(struct gc_tracer *tracer, struct gc_ref *objs,
                        size_t count) {
  simple_worklist_push_many(&tracer->worklist, objs, count);
}
static inline void
gc_trace_worker_enqueue(struct gc_trace_worker *worker, struct gc_ref ref) {
  gc_tracer_enqueue_root(worker->tracer, ref);
}
static inline void
tracer_trace_with_data(struct gc_tracer *tracer, struct gc_heap *heap,
                       struct gc_trace_worker *worker,
                       struct gc_trace_worker_data *data) {
  worker->data = data;
  do {
    struct gc_ref obj = simple_worklist_pop(&tracer->worklist);
    if (!gc_ref_is_heap_object(obj))
      break;
    trace_one(obj, heap, worker);
  } while (1);
}
static inline void
gc_tracer_trace(struct gc_tracer *tracer) {
  struct gc_trace_worker worker = { tracer };
  gc_trace_worker_call_with_data(tracer_trace_with_data, tracer, tracer->heap,
                                 &worker);
}

#endif // SERIAL_TRACER_H
