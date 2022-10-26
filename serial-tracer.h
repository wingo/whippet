#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "gc-api.h"

struct trace_queue {
  size_t size;
  size_t read;
  size_t write;
  struct gc_ref *buf;
};

static const size_t trace_queue_max_size =
  (1ULL << (sizeof(struct gc_ref) * 8 - 1)) / sizeof(struct gc_ref);
static const size_t trace_queue_release_byte_threshold = 1 * 1024 * 1024;

static struct gc_ref *
trace_queue_alloc(size_t size) {
  void *mem = mmap(NULL, size * sizeof(struct gc_ref), PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("Failed to grow trace queue");
    DEBUG("Failed to allocate %zu bytes", size);
    return NULL;
  }
  return mem;
}

static int
trace_queue_init(struct trace_queue *q) {
  q->size = getpagesize() / sizeof(struct gc_ref);
  q->read = 0;
  q->write = 0;
  q->buf = trace_queue_alloc(q->size);
  return !!q->buf;
}
  
static inline struct gc_ref
trace_queue_get(struct trace_queue *q, size_t idx) {
  return q->buf[idx & (q->size - 1)];
}

static inline void
trace_queue_put(struct trace_queue *q, size_t idx, struct gc_ref x) {
  q->buf[idx & (q->size - 1)] = x;
}

static int trace_queue_grow(struct trace_queue *q) GC_NEVER_INLINE;

static int
trace_queue_grow(struct trace_queue *q) {
  size_t old_size = q->size;
  struct gc_ref *old_buf = q->buf;
  if (old_size >= trace_queue_max_size) {
    DEBUG("trace queue already at max size of %zu bytes", old_size);
    return 0;
  }

  size_t new_size = old_size * 2;
  struct gc_ref *new_buf = trace_queue_alloc(new_size);
  if (!new_buf)
    return 0;

  size_t old_mask = old_size - 1;
  size_t new_mask = new_size - 1;

  for (size_t i = q->read; i < q->write; i++)
    new_buf[i & new_mask] = old_buf[i & old_mask];

  munmap(old_buf, old_size * sizeof(struct gc_ref));

  q->size = new_size;
  q->buf = new_buf;
  return 1;
}
  
static inline void
trace_queue_push(struct trace_queue *q, struct gc_ref p) {
  if (UNLIKELY(q->write - q->read == q->size)) {
    if (!trace_queue_grow(q))
      GC_CRASH();
  }
  trace_queue_put(q, q->write++, p);
}

static inline void
trace_queue_push_many(struct trace_queue *q, struct gc_ref *pv, size_t count) {
  while (q->size - (q->write - q->read) < count) {
    if (!trace_queue_grow(q))
      GC_CRASH();
  }
  for (size_t i = 0; i < count; i++)
    trace_queue_put(q, q->write++, pv[i]);
}

static inline struct gc_ref
trace_queue_pop(struct trace_queue *q) {
  if (UNLIKELY(q->read == q->write))
    return gc_ref_null();
  return trace_queue_get(q, q->read++);
}

static void
trace_queue_release(struct trace_queue *q) {
  size_t byte_size = q->size * sizeof(struct gc_ref);
  if (byte_size >= trace_queue_release_byte_threshold)
    madvise(q->buf, byte_size, MADV_DONTNEED);
  q->read = q->write = 0;
}

static void
trace_queue_destroy(struct trace_queue *q) {
  size_t byte_size = q->size * sizeof(struct gc_ref);
  munmap(q->buf, byte_size);
}

struct tracer {
  struct trace_queue queue;
};

struct gc_heap;
static inline struct tracer* heap_tracer(struct gc_heap *heap);

static int
tracer_init(struct gc_heap *heap, size_t parallelism) {
  return trace_queue_init(&heap_tracer(heap)->queue);
}
static void tracer_prepare(struct gc_heap *heap) {}
static void tracer_release(struct gc_heap *heap) {
  trace_queue_release(&heap_tracer(heap)->queue);
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
  trace_queue_push(&tracer->queue, obj);
}
static inline void
tracer_enqueue_roots(struct tracer *tracer, struct gc_ref *objs,
                     size_t count) {
  trace_queue_push_many(&tracer->queue, objs, count);
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
    struct gc_ref obj = trace_queue_pop(&heap_tracer(heap)->queue);
    if (!gc_ref_is_heap_object(obj))
      break;
    trace_one(obj, heap, NULL);
  } while (1);
}

#endif // SERIAL_TRACER_H
