#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "simple-worklist.h"
#include "root-worklist.h"
#include "tracer.h"

struct gc_tracer {
  struct gc_heap *heap;
  int trace_roots_only;
  struct root_worklist roots;
  struct simple_worklist worklist;
};

struct gc_trace_worker {
  struct gc_tracer *tracer;
  struct gc_trace_worker_data *data;
};

static inline struct gc_trace_worker_data*
gc_trace_worker_data(struct gc_trace_worker *worker) {
  return worker->data;
}

static int
gc_tracer_init(struct gc_tracer *tracer, struct gc_heap *heap,
               size_t parallelism) {
  tracer->heap = heap;
  tracer->trace_roots_only = 0;
  root_worklist_init(&tracer->roots);
  return simple_worklist_init(&tracer->worklist);
}
static void gc_tracer_prepare(struct gc_tracer *tracer) {}
static void gc_tracer_release(struct gc_tracer *tracer) {
  simple_worklist_release(&tracer->worklist);
}

static inline void
gc_tracer_add_root(struct gc_tracer *tracer, struct gc_root root) {
  root_worklist_push(&tracer->roots, root);
}

static inline void
gc_trace_worker_enqueue(struct gc_trace_worker *worker, struct gc_ref ref) {
  simple_worklist_push(&worker->tracer->worklist, ref);
}

static inline void
tracer_trace_with_data(struct gc_tracer *tracer, struct gc_heap *heap,
                       struct gc_trace_worker *worker,
                       struct gc_trace_worker_data *data) {
  worker->data = data;
  do {
    struct gc_root root = root_worklist_pop(&tracer->roots);
    if (root.kind == GC_ROOT_KIND_NONE)
      break;
    trace_root(root, heap, worker);
  } while (1);
  root_worklist_reset(&tracer->roots);
  if (!tracer->trace_roots_only) {
    do {
      struct gc_ref obj = simple_worklist_pop(&tracer->worklist);
      if (gc_ref_is_null(obj))
        break;
      trace_one(obj, heap, worker);
    } while (1);
  }
}
static inline void
gc_tracer_trace(struct gc_tracer *tracer) {
  struct gc_trace_worker worker = { tracer };
  gc_trace_worker_call_with_data(tracer_trace_with_data, tracer, tracer->heap,
                                 &worker);
}

static inline void
gc_tracer_trace_roots(struct gc_tracer *tracer) {
  tracer->trace_roots_only = 1;
  gc_tracer_trace(tracer);
  tracer->trace_roots_only = 0;
}

#endif // SERIAL_TRACER_H
