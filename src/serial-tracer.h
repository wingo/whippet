#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "simple-worklist.h"
#include "tracer.h"

struct gc_tracer {
  struct simple_worklist worklist;
};

static int
gc_tracer_init(struct gc_heap *heap, size_t parallelism) {
  return simple_worklist_init(&heap_tracer(heap)->worklist);
}
static void gc_tracer_prepare(struct gc_heap *heap) {}
static void gc_tracer_release(struct gc_heap *heap) {
  simple_worklist_release(&heap_tracer(heap)->worklist);
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
gc_tracer_enqueue(struct gc_ref ref, struct gc_heap *heap, void *trace_data) {
  gc_tracer_enqueue_root(heap_tracer(heap), ref);
}
static inline void
gc_tracer_trace(struct gc_heap *heap) {
  do {
    struct gc_ref obj = simple_worklist_pop(&heap_tracer(heap)->worklist);
    if (!gc_ref_is_heap_object(obj))
      break;
    trace_one(obj, heap, NULL);
  } while (1);
}

#endif // SERIAL_TRACER_H
