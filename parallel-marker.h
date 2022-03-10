#ifndef SERIAL_TRACE_H
#define SERIAL_TRACE_H

#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"

// The Chase-Lev work-stealing deque, as initially described in "Dynamic
// Circular Work-Stealing Deque" (Chase and Lev, SPAA'05)
// (https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf)
// and improved with C11 atomics in "Correct and Efficient Work-Stealing
// for Weak Memory Models" (Lê et al, PPoPP'13)
// (http://www.di.ens.fr/%7Ezappa/readings/ppopp13.pdf).

struct mark_buf {
  unsigned log_size;
  size_t size;
  atomic_uintptr_t *data;
};

// Min size: 8 kB on 64-bit systems, 4 kB on 32-bit.
#define mark_buf_min_log_size ((unsigned) 10)
// Max size: 2 GB on 64-bit systems, 1 GB on 32-bit.
#define mark_buf_max_log_size ((unsigned) 28)

static int
mark_buf_init(struct mark_buf *buf, unsigned log_size) {
  ASSERT(log_size >= mark_buf_min_log_size);
  ASSERT(log_size <= mark_buf_max_log_size);
  size_t size = (1 << log_size) * sizeof(uintptr_t);
  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("Failed to grow work-stealing dequeue");
    DEBUG("Failed to allocate %zu bytes", size, );
    return 0;
  }
  buf->log_size = log_size;
  buf->size = 1 << log_size;
  buf->data = mem;
  return 1;
}
  
static inline size_t
mark_buf_size(struct mark_buf *buf) {
  return buf->size;
}

static inline size_t
mark_buf_byte_size(struct mark_buf *buf) {
  return mark_buf_size(buf) * sizeof(uintptr_t);
}

static void
mark_buf_release(struct mark_buf *buf) {
  if (buf->data)
    madvise(buf->data, mark_buf_byte_size(buf), MADV_DONTNEED);
}

static void
mark_buf_destroy(struct mark_buf *buf) {
  if (buf->data) {
    munmap(buf->data, mark_buf_byte_size(buf));
    buf->data = NULL;
    buf->log_size = 0;
    buf->size = 0;
  }
}

static inline uintptr_t
mark_buf_get(struct mark_buf *buf, size_t i) {
  return atomic_load_explicit(&buf->data[i & (buf->size - 1)],
                              memory_order_relaxed);
}

static inline void
mark_buf_put(struct mark_buf *buf, size_t i, uintptr_t o) {
  return atomic_store_explicit(&buf->data[i & (buf->size - 1)],
                               o,
                               memory_order_relaxed);
}

static inline int
mark_buf_grow(struct mark_buf *from, struct mark_buf *to,
              size_t b, size_t t) {
  if (from->log_size == mark_buf_max_log_size)
    return 0;
  if (!mark_buf_init (to, from->log_size + 1))
    return 0;
  for (size_t i=t; i<b; i++)
    mark_buf_put(to, i, mark_buf_get(from, i));
  return 1;
}

static const uintptr_t mark_deque_empty = 0;
static const uintptr_t mark_deque_abort = 1;

// Chase-Lev work-stealing deque.  One thread pushes data into the deque
// at the bottom, and many threads compete to steal data from the top.
struct mark_deque {
  // Ensure bottom and top are on different cache lines.
  union {
    atomic_size_t bottom;
    char bottom_padding[64];
  };
  union {
    atomic_size_t top;
    char top_padding[64];
  };
  atomic_int active; // Which mark_buf is active.
  struct mark_buf bufs[(mark_buf_max_log_size - mark_buf_min_log_size) + 1];
};

#define LOAD_RELAXED(loc) atomic_load_explicit(loc, memory_order_relaxed)
#define STORE_RELAXED(loc, o) atomic_store_explicit(loc, o, memory_order_relaxed)

#define LOAD_ACQUIRE(loc) atomic_load_explicit(loc, memory_order_acquire)
#define STORE_RELEASE(loc, o) atomic_store_explicit(loc, o, memory_order_release)

#define LOAD_CONSUME(loc) atomic_load_explicit(loc, memory_order_consume)

static int
mark_deque_init(struct mark_deque *q) {
  memset(q, 0, sizeof (*q));
  int ret = mark_buf_init(&q->bufs[0], mark_buf_min_log_size);
  // Note, this fence isn't in the paper, I added it out of caution.
  atomic_thread_fence(memory_order_release);
  return ret;
}

static void
mark_deque_release(struct mark_deque *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    mark_buf_release(&q->bufs[i]);
}

static void
mark_deque_destroy(struct mark_deque *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    mark_buf_destroy(&q->bufs[i]);
}

static int
mark_deque_grow(struct mark_deque *q, int cur, size_t b, size_t t) {
  if (!mark_buf_grow(&q->bufs[cur], &q->bufs[cur + 1], b, t)) {
    fprintf(stderr, "failed to grow deque!!\n");
    abort();
  }

  cur++;
  STORE_RELAXED(&q->active, cur);
  return cur;
}

static void
mark_deque_push(struct mark_deque *q, uintptr_t x) {
  size_t b = LOAD_RELAXED(&q->bottom);
  size_t t = LOAD_ACQUIRE(&q->top);
  int active = LOAD_RELAXED(&q->active);

  if (b - t > mark_buf_size(&q->bufs[active]) - 1) /* Full queue. */
    active = mark_deque_grow(q, active, b, t);

  mark_buf_put(&q->bufs[active], b, x);
  atomic_thread_fence(memory_order_release);
  STORE_RELAXED(&q->bottom, b + 1);
}

static uintptr_t
mark_deque_try_pop(struct mark_deque *q) {
  size_t b = LOAD_RELAXED(&q->bottom);
  b = b - 1;
  int active = LOAD_RELAXED(&q->active);
  STORE_RELAXED(&q->bottom, b);
  atomic_thread_fence(memory_order_seq_cst);
  size_t t = LOAD_RELAXED(&q->top);
  uintptr_t x;
  if (t <= b) { // Non-empty queue.
    x = mark_buf_get(&q->bufs[active], b);
    if (t == b) { // Single last element in queue.
      if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1,
                                                   memory_order_seq_cst,
                                                   memory_order_relaxed))
        // Failed race.
        x = mark_deque_empty;
      STORE_RELAXED(&q->bottom, b + 1);
    }
  } else { // Empty queue.
    x = mark_deque_empty;
    STORE_RELAXED(&q->bottom, b + 1);
  }
  return x;
}

static uintptr_t
mark_deque_steal(struct mark_deque *q) {
  size_t t = LOAD_ACQUIRE(&q->top);
  atomic_thread_fence(memory_order_seq_cst);
  size_t b = LOAD_ACQUIRE(&q->bottom);
  uintptr_t x = mark_deque_empty;
  if (t < b) { // Non-empty queue.
    int active = LOAD_CONSUME(&q->active);
    x = mark_buf_get(&q->bufs[active], t);
    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed))
      // Failed race.
      return mark_deque_abort;
  }
  return x;
}

#undef LOAD_RELAXED
#undef STORE_RELAXED
#undef LOAD_ACQUIRE
#undef STORE_RELEASE
#undef LOAD_CONSUME

struct marker {
  struct mark_deque deque;
};

struct context;
static inline struct marker* context_marker(struct context *cx);

static int
marker_init(struct context *cx) {
  return mark_deque_init(&context_marker(cx)->deque);
}
static void marker_prepare(struct context *cx) {}
static void marker_release(struct context *cx) {
  mark_deque_release(&context_marker(cx)->deque);
}

struct gcobj;
static inline void marker_visit(struct context *cx, void **loc) __attribute__((always_inline));
static inline void marker_trace(struct context *cx,
                                void (*)(struct context *, struct gcobj *))
  __attribute__((always_inline));
static inline int mark_object(struct context *cx,
                              struct gcobj *obj) __attribute__((always_inline));

static inline void
marker_visit(struct context *cx, void **loc) {
  struct gcobj *obj = *loc;
  if (obj && mark_object(cx, obj))
    mark_deque_push(&context_marker(cx)->deque, (uintptr_t)obj);
}
static inline void
marker_visit_root(struct context *cx, void **loc) {
  marker_visit(cx, loc);
}
static inline void
marker_trace(struct context *cx,
             void (*process)(struct context *, struct gcobj *)) {
  while (1) {
    uintptr_t addr = mark_deque_steal(&context_marker(cx)->deque);
    if (addr == mark_deque_empty)
      return;
    if (addr == mark_deque_abort)
      continue;
    process(cx, (struct gcobj*)addr);
  }
}

#endif // SERIAL_MARK_H
