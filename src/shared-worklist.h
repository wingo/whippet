#ifndef SHARED_WORKLIST_H
#define SHARED_WORKLIST_H

#include <stdatomic.h>

#include "assert.h"
#include "debug.h"
#include "gc-align.h"
#include "gc-inline.h"
#include "gc-platform.h"
#include "spin.h"

// The Chase-Lev work-stealing deque, as initially described in "Dynamic
// Circular Work-Stealing Deque" (Chase and Lev, SPAA'05)
// (https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf)
// and improved with C11 atomics in "Correct and Efficient Work-Stealing
// for Weak Memory Models" (Lê et al, PPoPP'13)
// (http://www.di.ens.fr/%7Ezappa/readings/ppopp13.pdf).

struct shared_worklist_buf {
  unsigned log_size;
  size_t size;
  uintptr_t *data;
};

// Min size: 8 kB on 64-bit systems, 4 kB on 32-bit.
#define shared_worklist_buf_min_log_size ((unsigned) 10)
// Max size: 2 GB on 64-bit systems, 1 GB on 32-bit.
#define shared_worklist_buf_max_log_size ((unsigned) 28)

static const size_t shared_worklist_release_byte_threshold = 256 * 1024;

static int
shared_worklist_buf_init(struct shared_worklist_buf *buf, unsigned log_size) {
  ASSERT(log_size >= shared_worklist_buf_min_log_size);
  ASSERT(log_size <= shared_worklist_buf_max_log_size);
  size_t size = (1 << log_size) * sizeof(uintptr_t);
  void *mem = gc_platform_acquire_memory(size, 0);
  if (!mem) {
    perror("Failed to grow work-stealing dequeue");
    DEBUG("Failed to allocate %zu bytes", size);
    return 0;
  }
  buf->log_size = log_size;
  buf->size = 1 << log_size;
  buf->data = mem;
  return 1;
}
  
static inline size_t
shared_worklist_buf_size(struct shared_worklist_buf *buf) {
  return buf->size;
}

static inline size_t
shared_worklist_buf_byte_size(struct shared_worklist_buf *buf) {
  return shared_worklist_buf_size(buf) * sizeof(uintptr_t);
}

static void
shared_worklist_buf_release(struct shared_worklist_buf *buf) {
  size_t byte_size = shared_worklist_buf_byte_size(buf);
  if (buf->data && byte_size >= shared_worklist_release_byte_threshold)
    gc_platform_discard_memory(buf->data, byte_size);
}

static void
shared_worklist_buf_destroy(struct shared_worklist_buf *buf) {
  if (buf->data) {
    gc_platform_release_memory(buf->data, shared_worklist_buf_byte_size(buf));
    buf->data = NULL;
    buf->log_size = 0;
    buf->size = 0;
  }
}

static inline struct gc_ref
shared_worklist_buf_get(struct shared_worklist_buf *buf, size_t i) {
  return gc_ref(atomic_load_explicit(&buf->data[i & (buf->size - 1)],
                                     memory_order_relaxed));
}

static inline void
shared_worklist_buf_put(struct shared_worklist_buf *buf, size_t i,
                        struct gc_ref ref) {
  return atomic_store_explicit(&buf->data[i & (buf->size - 1)],
                               gc_ref_value(ref),
                               memory_order_relaxed);
}

static inline int
shared_worklist_buf_grow(struct shared_worklist_buf *from,
                         struct shared_worklist_buf *to, size_t b, size_t t) {
  if (from->log_size == shared_worklist_buf_max_log_size)
    return 0;
  if (!shared_worklist_buf_init (to, from->log_size + 1))
    return 0;
  for (size_t i=t; i<b; i++)
    shared_worklist_buf_put(to, i, shared_worklist_buf_get(from, i));
  return 1;
}

// Chase-Lev work-stealing deque.  One thread pushes data into the deque
// at the bottom, and many threads compete to steal data from the top.
struct shared_worklist {
  // Ensure bottom and top are on different cache lines.
  union {
    atomic_size_t bottom;
    char bottom_padding[AVOID_FALSE_SHARING];
  };
  union {
    atomic_size_t top;
    char top_padding[AVOID_FALSE_SHARING];
  };
  atomic_int active; // Which shared_worklist_buf is active.
  struct shared_worklist_buf bufs[(shared_worklist_buf_max_log_size -
                                   shared_worklist_buf_min_log_size) + 1];
};

#define LOAD_RELAXED(loc) atomic_load_explicit(loc, memory_order_relaxed)
#define STORE_RELAXED(loc, o) atomic_store_explicit(loc, o, memory_order_relaxed)

#define LOAD_ACQUIRE(loc) atomic_load_explicit(loc, memory_order_acquire)
#define STORE_RELEASE(loc, o) atomic_store_explicit(loc, o, memory_order_release)

#define LOAD_CONSUME(loc) atomic_load_explicit(loc, memory_order_consume)

static int
shared_worklist_init(struct shared_worklist *q) {
  memset(q, 0, sizeof (*q));
  int ret = shared_worklist_buf_init(&q->bufs[0],
                                     shared_worklist_buf_min_log_size);
  // Note, this fence isn't in the paper, I added it out of caution.
  atomic_thread_fence(memory_order_release);
  return ret;
}

static void
shared_worklist_release(struct shared_worklist *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    shared_worklist_buf_release(&q->bufs[i]);
}

static void
shared_worklist_destroy(struct shared_worklist *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    shared_worklist_buf_destroy(&q->bufs[i]);
}

static int
shared_worklist_grow(struct shared_worklist *q, int cur, size_t b, size_t t) {
  if (!shared_worklist_buf_grow(&q->bufs[cur], &q->bufs[cur + 1], b, t)) {
    fprintf(stderr, "failed to grow deque!!\n");
    GC_CRASH();
  }

  cur++;
  STORE_RELAXED(&q->active, cur);
  return cur;
}

static void
shared_worklist_push(struct shared_worklist *q, struct gc_ref x) {
  size_t b = LOAD_RELAXED(&q->bottom);
  size_t t = LOAD_ACQUIRE(&q->top);
  int active = LOAD_RELAXED(&q->active);

  ssize_t size = b - t;
  if (size > shared_worklist_buf_size(&q->bufs[active]) - 1)
    active = shared_worklist_grow(q, active, b, t); /* Full queue; grow. */

  shared_worklist_buf_put(&q->bufs[active], b, x);
  atomic_thread_fence(memory_order_release);
  STORE_RELAXED(&q->bottom, b + 1);
}

static void
shared_worklist_push_many(struct shared_worklist *q, struct gc_ref *objv,
                          size_t count) {
  size_t b = LOAD_RELAXED(&q->bottom);
  size_t t = LOAD_ACQUIRE(&q->top);
  int active = LOAD_RELAXED(&q->active);

  ssize_t size = b - t;
  while (size > shared_worklist_buf_size(&q->bufs[active]) - count)
    active = shared_worklist_grow(q, active, b, t); /* Full queue; grow. */

  for (size_t i = 0; i < count; i++)
    shared_worklist_buf_put(&q->bufs[active], b + i, objv[i]);
  atomic_thread_fence(memory_order_release);
  STORE_RELAXED(&q->bottom, b + count);
}

static struct gc_ref
shared_worklist_try_pop(struct shared_worklist *q) {
  size_t b = LOAD_RELAXED(&q->bottom);
  int active = LOAD_RELAXED(&q->active);
  STORE_RELAXED(&q->bottom, b - 1);
  atomic_thread_fence(memory_order_seq_cst);
  size_t t = LOAD_RELAXED(&q->top);
  struct gc_ref x;
  ssize_t size = b - t;
  if (size > 0) { // Non-empty queue.
    x = shared_worklist_buf_get(&q->bufs[active], b - 1);
    if (size == 1) { // Single last element in queue.
      if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1,
                                                   memory_order_seq_cst,
                                                   memory_order_relaxed))
        // Failed race.
        x = gc_ref_null();
      STORE_RELAXED(&q->bottom, b);
    }
  } else { // Empty queue.
    x = gc_ref_null();
    STORE_RELAXED(&q->bottom, b);
  }
  return x;
}

static struct gc_ref
shared_worklist_steal(struct shared_worklist *q) {
  while (1) {
    size_t t = LOAD_ACQUIRE(&q->top);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = LOAD_ACQUIRE(&q->bottom);
    ssize_t size = b - t;
    if (size <= 0)
      return gc_ref_null();
    int active = LOAD_CONSUME(&q->active);
    struct gc_ref ref = shared_worklist_buf_get(&q->bufs[active], t);
    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed))
      // Failed race.
      continue;
    return ref;
  }
}

static ssize_t
shared_worklist_size(struct shared_worklist *q) {
  size_t t = LOAD_ACQUIRE(&q->top);
  atomic_thread_fence(memory_order_seq_cst);
  size_t b = LOAD_ACQUIRE(&q->bottom);
  ssize_t size = b - t;
  return size;
}

static int
shared_worklist_can_steal(struct shared_worklist *q) {
  return shared_worklist_size(q) > 0;
}

#undef LOAD_RELAXED
#undef STORE_RELAXED
#undef LOAD_ACQUIRE
#undef STORE_RELEASE
#undef LOAD_CONSUME

#endif // SHARED_WORKLIST_H
