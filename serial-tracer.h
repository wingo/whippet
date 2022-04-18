#ifndef SERIAL_TRACER_H
#define SERIAL_TRACER_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"

struct gcobj;

struct trace_queue {
  size_t size;
  size_t read;
  size_t write;
  struct gcobj **buf;
};

static const size_t trace_queue_max_size =
  (1ULL << (sizeof(struct gcobj *) * 8 - 1)) / sizeof(struct gcobj *);
static const size_t trace_queue_release_byte_threshold = 1 * 1024 * 1024;

static struct gcobj **
trace_queue_alloc(size_t size) {
  void *mem = mmap(NULL, size * sizeof(struct gcobj *), PROT_READ|PROT_WRITE,
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
  q->size = getpagesize() / sizeof(struct gcobj *);
  q->read = 0;
  q->write = 0;
  q->buf = trace_queue_alloc(q->size);
  return !!q->buf;
}
  
static inline struct gcobj *
trace_queue_get(struct trace_queue *q, size_t idx) {
  return q->buf[idx & (q->size - 1)];
}

static inline void
trace_queue_put(struct trace_queue *q, size_t idx, struct gcobj *x) {
  q->buf[idx & (q->size - 1)] = x;
}

static int trace_queue_grow(struct trace_queue *q) NEVER_INLINE;

static int
trace_queue_grow(struct trace_queue *q) {
  size_t old_size = q->size;
  struct gcobj **old_buf = q->buf;
  if (old_size >= trace_queue_max_size) {
    DEBUG("trace queue already at max size of %zu bytes", old_size);
    return 0;
  }

  size_t new_size = old_size * 2;
  struct gcobj **new_buf = trace_queue_alloc(new_size);
  if (!new_buf)
    return 0;

  size_t old_mask = old_size - 1;
  size_t new_mask = new_size - 1;

  for (size_t i = q->read; i < q->write; i++)
    new_buf[i & new_mask] = old_buf[i & old_mask];

  munmap(old_buf, old_size * sizeof(struct gcobj *));

  q->size = new_size;
  q->buf = new_buf;
  return 1;
}
  
static inline void
trace_queue_push(struct trace_queue *q, struct gcobj *p) {
  if (UNLIKELY(q->write - q->read == q->size)) {
    if (!trace_queue_grow(q))
      abort();
  }
  trace_queue_put(q, q->write++, p);
}

static inline void
trace_queue_push_many(struct trace_queue *q, struct gcobj **pv, size_t count) {
  while (q->size - (q->write - q->read) < count) {
    if (!trace_queue_grow(q))
      abort();
  }
  for (size_t i = 0; i < count; i++)
    trace_queue_put(q, q->write++, pv[i]);
}

static inline struct gcobj*
trace_queue_pop(struct trace_queue *q) {
  if (UNLIKELY(q->read == q->write))
    return NULL;
  return trace_queue_get(q, q->read++);
}

static void
trace_queue_release(struct trace_queue *q) {
  size_t byte_size = q->size * sizeof(struct gcobj *);
  if (byte_size >= trace_queue_release_byte_threshold)
    madvise(q->buf, byte_size, MADV_DONTNEED);
  q->read = q->write = 0;
}

static void
trace_queue_destroy(struct trace_queue *q) {
  size_t byte_size = q->size * sizeof(struct gcobj *);
  munmap(q->buf, byte_size);
}

struct tracer {
  struct trace_queue queue;
};

struct heap;
static inline struct tracer* heap_tracer(struct heap *heap);

static int
tracer_init(struct heap *heap) {
  return trace_queue_init(&heap_tracer(heap)->queue);
}
static void tracer_prepare(struct heap *heap) {}
static void tracer_release(struct heap *heap) {
  trace_queue_release(&heap_tracer(heap)->queue);
}

struct gcobj;
static inline void tracer_visit(void **loc, void *trace_data) ALWAYS_INLINE;
static inline void trace_one(struct gcobj *obj, void *trace_data) ALWAYS_INLINE;
static inline int trace_object(struct heap *heap,
                               struct gcobj *obj) ALWAYS_INLINE;

static inline void
tracer_enqueue_root(struct tracer *tracer, struct gcobj *obj) {
  trace_queue_push(&tracer->queue, obj);
}
static inline void
tracer_enqueue_roots(struct tracer *tracer, struct gcobj **objs,
                     size_t count) {
  trace_queue_push_many(&tracer->queue, objs, count);
}
static inline void
tracer_visit(void **loc, void *trace_data) {
  struct heap *heap = trace_data;
  struct gcobj *obj = *loc;
  if (obj && trace_object(heap, obj))
    tracer_enqueue_root(heap_tracer(heap), obj);
}
static inline void
tracer_trace(struct heap *heap) {
  struct gcobj *obj;
  while ((obj = trace_queue_pop(&heap_tracer(heap)->queue)))
    trace_one(obj, heap);
}

#endif // SERIAL_TRACER_H
