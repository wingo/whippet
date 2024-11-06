#ifndef SIMPLE_WORKLIST_H
#define SIMPLE_WORKLIST_H

#include "assert.h"
#include "debug.h"
#include "gc-inline.h"
#include "gc-ref.h"
#include "gc-platform.h"

struct simple_worklist {
  size_t size;
  size_t read;
  size_t write;
  struct gc_ref *buf;
};

static const size_t simple_worklist_max_size =
  (1ULL << (sizeof(struct gc_ref) * 8 - 1)) / sizeof(struct gc_ref);
static const size_t simple_worklist_release_byte_threshold = 1 * 1024 * 1024;

static struct gc_ref *
simple_worklist_alloc(size_t size) {
  void *mem = gc_platform_acquire_memory(size * sizeof(struct gc_ref), 0);
  if (!mem) {
    perror("Failed to grow trace queue");
    DEBUG("Failed to allocate %zu bytes", size);
    return NULL;
  }
  return mem;
}

static int
simple_worklist_init(struct simple_worklist *q) {
  q->size = gc_platform_page_size() / sizeof(struct gc_ref);
  q->read = 0;
  q->write = 0;
  q->buf = simple_worklist_alloc(q->size);
  return !!q->buf;
}
  
static inline struct gc_ref
simple_worklist_get(struct simple_worklist *q, size_t idx) {
  return q->buf[idx & (q->size - 1)];
}

static inline void
simple_worklist_put(struct simple_worklist *q, size_t idx, struct gc_ref x) {
  q->buf[idx & (q->size - 1)] = x;
}

static int simple_worklist_grow(struct simple_worklist *q) GC_NEVER_INLINE;

static int
simple_worklist_grow(struct simple_worklist *q) {
  size_t old_size = q->size;
  struct gc_ref *old_buf = q->buf;
  if (old_size >= simple_worklist_max_size) {
    DEBUG("trace queue already at max size of %zu bytes", old_size);
    return 0;
  }

  size_t new_size = old_size * 2;
  struct gc_ref *new_buf = simple_worklist_alloc(new_size);
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
simple_worklist_push(struct simple_worklist *q, struct gc_ref p) {
  if (UNLIKELY(q->write - q->read == q->size)) {
    if (!simple_worklist_grow(q))
      GC_CRASH();
  }
  simple_worklist_put(q, q->write++, p);
}

static inline void
simple_worklist_push_many(struct simple_worklist *q, struct gc_ref *pv,
                          size_t count) {
  while (q->size - (q->write - q->read) < count) {
    if (!simple_worklist_grow(q))
      GC_CRASH();
  }
  for (size_t i = 0; i < count; i++)
    simple_worklist_put(q, q->write++, pv[i]);
}

static inline struct gc_ref
simple_worklist_pop(struct simple_worklist *q) {
  if (UNLIKELY(q->read == q->write))
    return gc_ref_null();
  return simple_worklist_get(q, q->read++);
}

static void
simple_worklist_release(struct simple_worklist *q) {
  size_t byte_size = q->size * sizeof(struct gc_ref);
  if (byte_size >= simple_worklist_release_byte_threshold)
    madvise(q->buf, byte_size, MADV_DONTNEED);
  q->read = q->write = 0;
}

static void
simple_worklist_destroy(struct simple_worklist *q) {
  size_t byte_size = q->size * sizeof(struct gc_ref);
  munmap(q->buf, byte_size);
}

#endif // SIMPLE_WORKLIST_H
