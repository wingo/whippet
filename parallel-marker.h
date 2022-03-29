#ifndef PARALLEL_MARKER_H
#define PARALLEL_MARKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "inline.h"

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
    DEBUG("Failed to allocate %zu bytes", size);
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

static int
mark_deque_can_steal(struct mark_deque *q) {
  size_t t = LOAD_ACQUIRE(&q->top);
  atomic_thread_fence(memory_order_seq_cst);
  size_t b = LOAD_ACQUIRE(&q->bottom);
  return t < b;
}

#undef LOAD_RELAXED
#undef STORE_RELAXED
#undef LOAD_ACQUIRE
#undef STORE_RELEASE
#undef LOAD_CONSUME

#define LOCAL_MARK_QUEUE_SIZE 1024
#define LOCAL_MARK_QUEUE_MASK (LOCAL_MARK_QUEUE_SIZE - 1)
#define LOCAL_MARK_QUEUE_SHARE_AMOUNT (LOCAL_MARK_QUEUE_SIZE * 3 / 4)
struct local_mark_queue {
  size_t read;
  size_t write;
  uintptr_t data[LOCAL_MARK_QUEUE_SIZE];
};

static inline void
local_mark_queue_init(struct local_mark_queue *q) {
  q->read = q->write = 0;
}
static inline void
local_mark_queue_poison(struct local_mark_queue *q) {
  q->read = 0; q->write = LOCAL_MARK_QUEUE_SIZE;
}
static inline size_t
local_mark_queue_size(struct local_mark_queue *q) {
  return q->write - q->read;
}
static inline int
local_mark_queue_empty(struct local_mark_queue *q) {
  return local_mark_queue_size(q) == 0;
}
static inline int
local_mark_queue_full(struct local_mark_queue *q) {
  return local_mark_queue_size(q) >= LOCAL_MARK_QUEUE_SIZE;
}
static inline void
local_mark_queue_push(struct local_mark_queue *q, uintptr_t v) {
  q->data[q->write++ & LOCAL_MARK_QUEUE_MASK] = v;
}
static inline uintptr_t
local_mark_queue_pop(struct local_mark_queue *q) {
  return q->data[q->read++ & LOCAL_MARK_QUEUE_MASK];
}

enum mark_worker_state {
  MARK_WORKER_STOPPED,
  MARK_WORKER_IDLE,
  MARK_WORKER_MARKING,
  MARK_WORKER_STOPPING,
  MARK_WORKER_DEAD
};

struct mark_worker {
  struct mark_space *space;
  size_t id;
  size_t steal_id;
  pthread_t thread;
  enum mark_worker_state state;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct mark_deque deque;
};

#define MARK_WORKERS_MAX_COUNT 8

struct marker {
  atomic_size_t active_markers;
  size_t worker_count;
  atomic_size_t running_markers;
  long count;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct mark_worker workers[MARK_WORKERS_MAX_COUNT];
};

struct local_marker {
  struct mark_worker *worker;
  struct mark_deque *share_deque;
  struct mark_space *space;
  struct local_mark_queue local;
};

struct context;
static inline struct marker* mark_space_marker(struct mark_space *space);

static size_t number_of_current_processors(void) { return 1; }

static int
mark_worker_init(struct mark_worker *worker, struct mark_space *space,
                 struct marker *marker, size_t id) {
  worker->space = space;
  worker->id = id;
  worker->steal_id = 0;
  worker->thread = 0;
  worker->state = MARK_WORKER_STOPPED;
  pthread_mutex_init(&worker->lock, NULL);
  pthread_cond_init(&worker->cond, NULL);
  return mark_deque_init(&worker->deque);
}

static void mark_worker_mark(struct mark_worker *worker);

static void*
mark_worker_thread(void *data) {
  struct mark_worker *worker = data;

  pthread_mutex_lock(&worker->lock);
  while (1) {
    switch (worker->state) {
    case MARK_WORKER_IDLE:
      pthread_cond_wait(&worker->cond, &worker->lock);
      break;
    case MARK_WORKER_MARKING:
      mark_worker_mark(worker);
      worker->state = MARK_WORKER_IDLE;
      break;
    case MARK_WORKER_STOPPING:
      worker->state = MARK_WORKER_DEAD;
      pthread_mutex_unlock(&worker->lock);
      return NULL;
    default:
      abort();
    }
  }
}

static int
mark_worker_spawn(struct mark_worker *worker) {
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == MARK_WORKER_STOPPED);
  worker->state = MARK_WORKER_IDLE;
  pthread_mutex_unlock(&worker->lock);

  if (pthread_create(&worker->thread, NULL, mark_worker_thread, worker)) {
    perror("spawning marker thread failed");
    worker->state = MARK_WORKER_STOPPED;
    return 0;
  }

  return 1;
}

static void
mark_worker_request_mark(struct mark_worker *worker) {
  struct marker *marker = mark_space_marker(worker->space);
    
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == MARK_WORKER_IDLE);
  worker->state = MARK_WORKER_MARKING;
  pthread_cond_signal(&worker->cond);
  pthread_mutex_unlock(&worker->lock);
}  

static void
mark_worker_finished_marking(struct mark_worker *worker) {
  // Signal controller that we are done with marking.
  struct marker *marker = mark_space_marker(worker->space);
    
  if (atomic_fetch_sub(&marker->running_markers, 1) == 1) {
    pthread_mutex_lock(&marker->lock);
    marker->count++;
    pthread_cond_signal(&marker->cond);
    pthread_mutex_unlock(&marker->lock);
  }
}

static void
mark_worker_request_stop(struct mark_worker *worker) {
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == MARK_WORKER_IDLE);
  worker->state = MARK_WORKER_STOPPING;
  pthread_cond_signal(&worker->cond);
  pthread_mutex_unlock(&worker->lock);
}  

static int
marker_init(struct mark_space *space) {
  struct marker *marker = mark_space_marker(space);
  atomic_init(&marker->active_markers, 0);
  atomic_init(&marker->running_markers, 0);
  marker->count = 0;
  pthread_mutex_init(&marker->lock, NULL);
  pthread_cond_init(&marker->cond, NULL);
  size_t desired_worker_count = 0;
  if (getenv("GC_MARKERS"))
    desired_worker_count = atoi(getenv("GC_MARKERS"));
  if (desired_worker_count == 0)
    desired_worker_count = number_of_current_processors();
  if (desired_worker_count > MARK_WORKERS_MAX_COUNT)
    desired_worker_count = MARK_WORKERS_MAX_COUNT;
  for (size_t i = 0; i < desired_worker_count; i++) {
    if (!mark_worker_init(&marker->workers[i], space, marker, i))
      break;
    if (mark_worker_spawn(&marker->workers[i]))
      marker->worker_count++;
    else
      break;
  }
  return marker->worker_count > 0;
}

static void marker_prepare(struct mark_space *space) {
  struct marker *marker = mark_space_marker(space);
  for (size_t i = 0; i < marker->worker_count; i++)
    marker->workers[i].steal_id = 0;
}
static void marker_release(struct mark_space *space) {
  struct marker *marker = mark_space_marker(space);
  for (size_t i = 0; i < marker->worker_count; i++)
    mark_deque_release(&marker->workers[i].deque);
}

struct gcobj;
static inline void marker_visit(void **loc, void *mark_data) ALWAYS_INLINE;
static inline void trace_one(struct gcobj *obj, void *mark_data) ALWAYS_INLINE;
static inline int mark_object(struct mark_space *space,
                              struct gcobj *obj) ALWAYS_INLINE;

static inline void
marker_share(struct local_marker *mark) {
  DEBUG("marker #%zu: sharing\n", mark->worker->id);
  for (size_t i = 0; i < LOCAL_MARK_QUEUE_SHARE_AMOUNT; i++)
    mark_deque_push(mark->share_deque, local_mark_queue_pop(&mark->local));
}

static inline void
marker_visit(void **loc, void *mark_data) {
  struct local_marker *mark = mark_data;
  struct gcobj *obj = *loc;
  if (obj && mark_object(mark->space, obj)) {
    if (local_mark_queue_full(&mark->local))
      marker_share(mark);
    local_mark_queue_push(&mark->local, (uintptr_t)obj);
  }
}

static uintptr_t
marker_steal_from_worker(struct marker *marker, size_t id) {
  ASSERT(id < marker->worker_count);
  while (1) {
    uintptr_t res = mark_deque_steal(&marker->workers[id].deque);
    if (res == mark_deque_empty)
      return 0;
    if (res == mark_deque_abort)
      continue;
    return res;
  }
}

static uintptr_t
marker_can_steal_from_worker(struct marker *marker, size_t id) {
  ASSERT(id < marker->worker_count);
  return mark_deque_can_steal(&marker->workers[id].deque);
}

static uintptr_t
mark_worker_steal_from_any(struct mark_worker *worker, struct marker *marker) {
  size_t steal_id = worker->steal_id;
  for (size_t i = 0; i < marker->worker_count; i++) {
    steal_id = (steal_id + 1) % marker->worker_count;
    DEBUG("marker #%zu: stealing from #%zu\n", worker->id, steal_id);
    uintptr_t addr = marker_steal_from_worker(marker, steal_id);
    if (addr) {
      DEBUG("marker #%zu: stealing got 0x%zx\n", worker->id, addr);
      worker->steal_id = steal_id;
      return addr;
    }
  }
  DEBUG("marker #%zu: failed to steal\n", worker->id);
  return 0;
}

static int
mark_worker_can_steal_from_any(struct mark_worker *worker, struct marker *marker) {
  size_t steal_id = worker->steal_id;
  DEBUG("marker #%zu: checking if any worker has tasks\n", worker->id);
  for (size_t i = 0; i < marker->worker_count; i++) {
    steal_id = (steal_id + 1) % marker->worker_count;
    int res = marker_can_steal_from_worker(marker, steal_id);
    if (res) {
      DEBUG("marker #%zu: worker #%zu has tasks!\n", worker->id, steal_id);
      worker->steal_id = steal_id;
      return 1;
    }
  }
  DEBUG("marker #%zu: nothing to steal\n", worker->id);
  return 0;
}

static int
mark_worker_check_termination(struct mark_worker *worker,
                              struct marker *marker) {
  // We went around all workers and nothing.  Enter termination phase.
  if (atomic_fetch_sub_explicit(&marker->active_markers, 1,
                                memory_order_relaxed) == 1) {
    DEBUG("  ->> marker #%zu: DONE (no spinning) <<-\n", worker->id);
    return 1;
  }

  size_t spin_count = 0;
  while (1) {
    if (mark_worker_can_steal_from_any(worker, marker)) {
      atomic_fetch_add_explicit(&marker->active_markers, 1,
                                memory_order_relaxed);
      return 0;
    }
    if (atomic_load_explicit(&marker->active_markers,
                             memory_order_relaxed) == 0) {
      DEBUG("  ->> marker #%zu: DONE <<-\n", worker->id);
      return 1;
    }
    // spin
    DEBUG("marker #%zu: spinning #%zu\n", worker->id, spin_count);
    if (spin_count < 10)
      __builtin_ia32_pause();
    else if (spin_count < 20)
      sched_yield();
    else if (spin_count < 40)
      usleep(0);
    else
      usleep(1);
    spin_count++;
  }
}

static uintptr_t
mark_worker_steal(struct local_marker *mark) {
  struct marker *marker = mark_space_marker(mark->space);
  struct mark_worker *worker = mark->worker;

  while (1) {
    DEBUG("marker #%zu: trying to steal\n", worker->id);
    uintptr_t addr = mark_worker_steal_from_any(worker, marker);
    if (addr)
      return addr;

    if (mark_worker_check_termination(worker, marker))
      return 0;
  }
}

static void
mark_worker_mark(struct mark_worker *worker) {
  struct local_marker mark;
  mark.worker = worker;
  mark.share_deque = &worker->deque;
  mark.space = worker->space;
  local_mark_queue_init(&mark.local);

  size_t n = 0;
  DEBUG("marker #%zu: running mark loop\n", worker->id);
  while (1) {
    uintptr_t addr;
    if (!local_mark_queue_empty(&mark.local)) {
      addr = local_mark_queue_pop(&mark.local);
    } else {
      addr = mark_worker_steal(&mark);
      if (!addr)
        break;
    }
    trace_one((struct gcobj*)addr, &mark);
    n++;
  }
  DEBUG("marker #%zu: done marking, %zu objects traced\n", worker->id, n);

  mark_worker_finished_marking(worker);
}

static inline void
marker_enqueue_root(struct marker *marker, struct gcobj *obj) {
  struct mark_deque *worker0_deque = &marker->workers[0].deque;
  mark_deque_push(worker0_deque, (uintptr_t)obj);
}

static inline void
marker_trace(struct mark_space *space) {
  struct marker *marker = mark_space_marker(space);

  pthread_mutex_lock(&marker->lock);
  long mark_count = marker->count;
  pthread_mutex_unlock(&marker->lock);

  DEBUG("starting trace; %zu workers\n", marker->worker_count);
  DEBUG("waking workers\n");
  atomic_store_explicit(&marker->active_markers, marker->worker_count,
                        memory_order_release);
  atomic_store_explicit(&marker->running_markers, marker->worker_count,
                        memory_order_release);
  for (size_t i = 0; i < marker->worker_count; i++)
    mark_worker_request_mark(&marker->workers[i]);

  DEBUG("waiting on markers\n");

  pthread_mutex_lock(&marker->lock);
  while (marker->count <= mark_count)
    pthread_cond_wait(&marker->cond, &marker->lock);
  pthread_mutex_unlock(&marker->lock);

  DEBUG("trace finished\n");
}

#endif // PARALLEL_MARKER_H
