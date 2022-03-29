#ifndef SERIAL_TRACE_H
#define SERIAL_TRACE_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"

struct gcobj;

struct mark_queue {
  size_t size;
  size_t read;
  size_t write;
  struct gcobj **buf;
};

static const size_t mark_queue_max_size =
  (1ULL << (sizeof(struct gcobj *) * 8 - 1)) / sizeof(struct gcobj *);
static const size_t mark_queue_release_byte_threshold = 1 * 1024 * 1024;

static struct gcobj **
mark_queue_alloc(size_t size) {
  void *mem = mmap(NULL, size * sizeof(struct gcobj *), PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("Failed to grow mark queue");
    DEBUG("Failed to allocate %zu bytes", size);
    return NULL;
  }
  return mem;
}

static int
mark_queue_init(struct mark_queue *q) {
  q->size = getpagesize() / sizeof(struct gcobj *);
  q->read = 0;
  q->write = 0;
  q->buf = mark_queue_alloc(q->size);
  return !!q->buf;
}
  
static inline struct gcobj *
mark_queue_get(struct mark_queue *q, size_t idx) {
  return q->buf[idx & (q->size - 1)];
}

static inline void
mark_queue_put(struct mark_queue *q, size_t idx, struct gcobj *x) {
  q->buf[idx & (q->size - 1)] = x;
}

static int mark_queue_grow(struct mark_queue *q) NEVER_INLINE;

static int
mark_queue_grow(struct mark_queue *q) {
  size_t old_size = q->size;
  struct gcobj **old_buf = q->buf;
  if (old_size >= mark_queue_max_size) {
    DEBUG("mark queue already at max size of %zu bytes", old_size);
    return 0;
  }

  size_t new_size = old_size * 2;
  struct gcobj **new_buf = mark_queue_alloc(new_size);
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
mark_queue_push(struct mark_queue *q, struct gcobj *p) {
  if (UNLIKELY(q->write - q->read == q->size)) {
    if (!mark_queue_grow(q))
      abort();
  }
  mark_queue_put(q, q->write++, p);
}

static inline void
mark_queue_push_many(struct mark_queue *q, struct gcobj **pv, size_t count) {
  while (q->size - (q->write - q->read) < count) {
    if (!mark_queue_grow(q))
      abort();
  }
  for (size_t i = 0; i < count; i++)
    mark_queue_put(q, q->write++, pv[i]);
}

static inline struct gcobj*
mark_queue_pop(struct mark_queue *q) {
  if (UNLIKELY(q->read == q->write))
    return NULL;
  return mark_queue_get(q, q->read++);
}

static void
mark_queue_release(struct mark_queue *q) {
  size_t byte_size = q->size * sizeof(struct gcobj *);
  if (byte_size >= mark_queue_release_byte_threshold)
    madvise(q->buf, byte_size, MADV_DONTNEED);
  q->read = q->write = 0;
}

static void
mark_queue_destroy(struct mark_queue *q) {
  size_t byte_size = q->size * sizeof(struct gcobj *);
  munmap(q->buf, byte_size);
}

struct marker {
  struct mark_queue queue;
};

struct mark_space;
static inline struct marker* mark_space_marker(struct mark_space *space);

static int
marker_init(struct mark_space *space) {
  return mark_queue_init(&mark_space_marker(space)->queue);
}
static void marker_prepare(struct mark_space *space) {}
static void marker_release(struct mark_space *space) {
  mark_queue_release(&mark_space_marker(space)->queue);
}

struct gcobj;
static inline void marker_visit(void **loc, void *mark_data) ALWAYS_INLINE;
static inline void trace_one(struct gcobj *obj, void *mark_data) ALWAYS_INLINE;
static inline int mark_object(struct mark_space *space,
                              struct gcobj *obj) ALWAYS_INLINE;

static inline void
marker_enqueue_root(struct marker *marker, struct gcobj *obj) {
  mark_queue_push(&marker->queue, obj);
}
static inline void
marker_enqueue_roots(struct marker *marker, struct gcobj **objs,
                     size_t count) {
  mark_queue_push_many(&marker->queue, objs, count);
}
static inline void
marker_visit(void **loc, void *mark_data) {
  struct mark_space *space = mark_data;
  struct gcobj *obj = *loc;
  if (obj && mark_object(space, obj))
    marker_enqueue_root(mark_space_marker(space), obj);
}
static inline void
marker_trace(struct mark_space *space) {
  struct gcobj *obj;
  while ((obj = mark_queue_pop(&mark_space_marker(space)->queue)))
    trace_one(obj, space);
}

#endif // SERIAL_MARK_H
