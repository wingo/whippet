#ifndef PARALLEL_TRACER_H
#define PARALLEL_TRACER_H

#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "gc-inline.h"
#include "spin.h"

// The Chase-Lev work-stealing deque, as initially described in "Dynamic
// Circular Work-Stealing Deque" (Chase and Lev, SPAA'05)
// (https://www.dre.vanderbilt.edu/~schmidt/PDF/work-stealing-dequeue.pdf)
// and improved with C11 atomics in "Correct and Efficient Work-Stealing
// for Weak Memory Models" (Lê et al, PPoPP'13)
// (http://www.di.ens.fr/%7Ezappa/readings/ppopp13.pdf).

struct trace_buf {
  unsigned log_size;
  size_t size;
  uintptr_t *data;
};

// Min size: 8 kB on 64-bit systems, 4 kB on 32-bit.
#define trace_buf_min_log_size ((unsigned) 10)
// Max size: 2 GB on 64-bit systems, 1 GB on 32-bit.
#define trace_buf_max_log_size ((unsigned) 28)

static int
trace_buf_init(struct trace_buf *buf, unsigned log_size) {
  ASSERT(log_size >= trace_buf_min_log_size);
  ASSERT(log_size <= trace_buf_max_log_size);
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
trace_buf_size(struct trace_buf *buf) {
  return buf->size;
}

static inline size_t
trace_buf_byte_size(struct trace_buf *buf) {
  return trace_buf_size(buf) * sizeof(uintptr_t);
}

static void
trace_buf_release(struct trace_buf *buf) {
  if (buf->data)
    madvise(buf->data, trace_buf_byte_size(buf), MADV_DONTNEED);
}

static void
trace_buf_destroy(struct trace_buf *buf) {
  if (buf->data) {
    munmap(buf->data, trace_buf_byte_size(buf));
    buf->data = NULL;
    buf->log_size = 0;
    buf->size = 0;
  }
}

static inline struct gc_ref
trace_buf_get(struct trace_buf *buf, size_t i) {
  return gc_ref(atomic_load_explicit(&buf->data[i & (buf->size - 1)],
                                     memory_order_relaxed));
}

static inline void
trace_buf_put(struct trace_buf *buf, size_t i, struct gc_ref ref) {
  return atomic_store_explicit(&buf->data[i & (buf->size - 1)],
                               gc_ref_value(ref),
                               memory_order_relaxed);
}

static inline int
trace_buf_grow(struct trace_buf *from, struct trace_buf *to,
              size_t b, size_t t) {
  if (from->log_size == trace_buf_max_log_size)
    return 0;
  if (!trace_buf_init (to, from->log_size + 1))
    return 0;
  for (size_t i=t; i<b; i++)
    trace_buf_put(to, i, trace_buf_get(from, i));
  return 1;
}

// Chase-Lev work-stealing deque.  One thread pushes data into the deque
// at the bottom, and many threads compete to steal data from the top.
struct trace_deque {
  // Ensure bottom and top are on different cache lines.
  union {
    atomic_size_t bottom;
    char bottom_padding[64];
  };
  union {
    atomic_size_t top;
    char top_padding[64];
  };
  atomic_int active; // Which trace_buf is active.
  struct trace_buf bufs[(trace_buf_max_log_size - trace_buf_min_log_size) + 1];
};

#define LOAD_RELAXED(loc) atomic_load_explicit(loc, memory_order_relaxed)
#define STORE_RELAXED(loc, o) atomic_store_explicit(loc, o, memory_order_relaxed)

#define LOAD_ACQUIRE(loc) atomic_load_explicit(loc, memory_order_acquire)
#define STORE_RELEASE(loc, o) atomic_store_explicit(loc, o, memory_order_release)

#define LOAD_CONSUME(loc) atomic_load_explicit(loc, memory_order_consume)

static int
trace_deque_init(struct trace_deque *q) {
  memset(q, 0, sizeof (*q));
  int ret = trace_buf_init(&q->bufs[0], trace_buf_min_log_size);
  // Note, this fence isn't in the paper, I added it out of caution.
  atomic_thread_fence(memory_order_release);
  return ret;
}

static void
trace_deque_release(struct trace_deque *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    trace_buf_release(&q->bufs[i]);
}

static void
trace_deque_destroy(struct trace_deque *q) {
  for (int i = LOAD_RELAXED(&q->active); i >= 0; i--)
    trace_buf_destroy(&q->bufs[i]);
}

static int
trace_deque_grow(struct trace_deque *q, int cur, size_t b, size_t t) {
  if (!trace_buf_grow(&q->bufs[cur], &q->bufs[cur + 1], b, t)) {
    fprintf(stderr, "failed to grow deque!!\n");
    GC_CRASH();
  }

  cur++;
  STORE_RELAXED(&q->active, cur);
  return cur;
}

static void
trace_deque_push(struct trace_deque *q, struct gc_ref x) {
  size_t b = LOAD_RELAXED(&q->bottom);
  size_t t = LOAD_ACQUIRE(&q->top);
  int active = LOAD_RELAXED(&q->active);

  ssize_t size = b - t;
  if (size > trace_buf_size(&q->bufs[active]) - 1) /* Full queue. */
    active = trace_deque_grow(q, active, b, t);

  trace_buf_put(&q->bufs[active], b, x);
  atomic_thread_fence(memory_order_release);
  STORE_RELAXED(&q->bottom, b + 1);
}

static void
trace_deque_push_many(struct trace_deque *q, struct gc_ref *objv, size_t count) {
  size_t b = LOAD_RELAXED(&q->bottom);
  size_t t = LOAD_ACQUIRE(&q->top);
  int active = LOAD_RELAXED(&q->active);

  ssize_t size = b - t;
  while (size > trace_buf_size(&q->bufs[active]) - count) /* Full queue. */
    active = trace_deque_grow(q, active, b, t);

  for (size_t i = 0; i < count; i++)
    trace_buf_put(&q->bufs[active], b + i, objv[i]);
  atomic_thread_fence(memory_order_release);
  STORE_RELAXED(&q->bottom, b + count);
}

static struct gc_ref
trace_deque_try_pop(struct trace_deque *q) {
  size_t b = LOAD_RELAXED(&q->bottom);
  int active = LOAD_RELAXED(&q->active);
  STORE_RELAXED(&q->bottom, b - 1);
  atomic_thread_fence(memory_order_seq_cst);
  size_t t = LOAD_RELAXED(&q->top);
  struct gc_ref x;
  ssize_t size = b - t;
  if (size > 0) { // Non-empty queue.
    x = trace_buf_get(&q->bufs[active], b - 1);
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
trace_deque_steal(struct trace_deque *q) {
  while (1) {
    size_t t = LOAD_ACQUIRE(&q->top);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = LOAD_ACQUIRE(&q->bottom);
    ssize_t size = b - t;
    if (size <= 0)
      return gc_ref_null();
    int active = LOAD_CONSUME(&q->active);
    struct gc_ref ref = trace_buf_get(&q->bufs[active], t);
    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1,
                                                 memory_order_seq_cst,
                                                 memory_order_relaxed))
      // Failed race.
      continue;
    return ref;
  }
}

static int
trace_deque_can_steal(struct trace_deque *q) {
  size_t t = LOAD_ACQUIRE(&q->top);
  atomic_thread_fence(memory_order_seq_cst);
  size_t b = LOAD_ACQUIRE(&q->bottom);
  ssize_t size = b - t;
  return size > 0;
}

#undef LOAD_RELAXED
#undef STORE_RELAXED
#undef LOAD_ACQUIRE
#undef STORE_RELEASE
#undef LOAD_CONSUME

#define LOCAL_TRACE_QUEUE_SIZE 1024
#define LOCAL_TRACE_QUEUE_MASK (LOCAL_TRACE_QUEUE_SIZE - 1)
#define LOCAL_TRACE_QUEUE_SHARE_AMOUNT (LOCAL_TRACE_QUEUE_SIZE * 3 / 4)
struct local_trace_queue {
  size_t read;
  size_t write;
  struct gc_ref data[LOCAL_TRACE_QUEUE_SIZE];
};

static inline void
local_trace_queue_init(struct local_trace_queue *q) {
  q->read = q->write = 0;
}
static inline void
local_trace_queue_poison(struct local_trace_queue *q) {
  q->read = 0; q->write = LOCAL_TRACE_QUEUE_SIZE;
}
static inline size_t
local_trace_queue_size(struct local_trace_queue *q) {
  return q->write - q->read;
}
static inline int
local_trace_queue_empty(struct local_trace_queue *q) {
  return local_trace_queue_size(q) == 0;
}
static inline int
local_trace_queue_full(struct local_trace_queue *q) {
  return local_trace_queue_size(q) >= LOCAL_TRACE_QUEUE_SIZE;
}
static inline void
local_trace_queue_push(struct local_trace_queue *q, struct gc_ref v) {
  q->data[q->write++ & LOCAL_TRACE_QUEUE_MASK] = v;
}
static inline struct gc_ref
local_trace_queue_pop(struct local_trace_queue *q) {
  return q->data[q->read++ & LOCAL_TRACE_QUEUE_MASK];
}

enum trace_worker_state {
  TRACE_WORKER_STOPPED,
  TRACE_WORKER_IDLE,
  TRACE_WORKER_TRACING,
  TRACE_WORKER_STOPPING,
  TRACE_WORKER_DEAD
};

struct gc_heap;
struct trace_worker {
  struct gc_heap *heap;
  size_t id;
  size_t steal_id;
  pthread_t thread;
  enum trace_worker_state state;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct trace_deque deque;
};

#define TRACE_WORKERS_MAX_COUNT 8

struct tracer {
  atomic_size_t active_tracers;
  size_t worker_count;
  atomic_size_t running_tracers;
  long count;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct trace_worker workers[TRACE_WORKERS_MAX_COUNT];
};

struct local_tracer {
  struct trace_worker *worker;
  struct trace_deque *share_deque;
  struct local_trace_queue local;
};

struct context;
static inline struct tracer* heap_tracer(struct gc_heap *heap);

static int
trace_worker_init(struct trace_worker *worker, struct gc_heap *heap,
                 struct tracer *tracer, size_t id) {
  worker->heap = heap;
  worker->id = id;
  worker->steal_id = 0;
  worker->thread = 0;
  worker->state = TRACE_WORKER_STOPPED;
  pthread_mutex_init(&worker->lock, NULL);
  pthread_cond_init(&worker->cond, NULL);
  return trace_deque_init(&worker->deque);
}

static void trace_worker_trace(struct trace_worker *worker);

static void*
trace_worker_thread(void *data) {
  struct trace_worker *worker = data;

  pthread_mutex_lock(&worker->lock);
  while (1) {
    switch (worker->state) {
    case TRACE_WORKER_IDLE:
      pthread_cond_wait(&worker->cond, &worker->lock);
      break;
    case TRACE_WORKER_TRACING:
      trace_worker_trace(worker);
      worker->state = TRACE_WORKER_IDLE;
      break;
    case TRACE_WORKER_STOPPING:
      worker->state = TRACE_WORKER_DEAD;
      pthread_mutex_unlock(&worker->lock);
      return NULL;
    default:
      GC_CRASH();
    }
  }
}

static int
trace_worker_spawn(struct trace_worker *worker) {
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == TRACE_WORKER_STOPPED);
  worker->state = TRACE_WORKER_IDLE;
  pthread_mutex_unlock(&worker->lock);

  if (pthread_create(&worker->thread, NULL, trace_worker_thread, worker)) {
    perror("spawning tracer thread failed");
    worker->state = TRACE_WORKER_STOPPED;
    return 0;
  }

  return 1;
}

static void
trace_worker_request_trace(struct trace_worker *worker) {
  struct tracer *tracer = heap_tracer(worker->heap);
    
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == TRACE_WORKER_IDLE);
  worker->state = TRACE_WORKER_TRACING;
  pthread_cond_signal(&worker->cond);
  pthread_mutex_unlock(&worker->lock);
}  

static void
trace_worker_finished_tracing(struct trace_worker *worker) {
  // Signal controller that we are done with tracing.
  struct tracer *tracer = heap_tracer(worker->heap);
    
  if (atomic_fetch_sub(&tracer->running_tracers, 1) == 1) {
    pthread_mutex_lock(&tracer->lock);
    tracer->count++;
    pthread_cond_signal(&tracer->cond);
    pthread_mutex_unlock(&tracer->lock);
  }
}

static void
trace_worker_request_stop(struct trace_worker *worker) {
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == TRACE_WORKER_IDLE);
  worker->state = TRACE_WORKER_STOPPING;
  pthread_cond_signal(&worker->cond);
  pthread_mutex_unlock(&worker->lock);
}  

static int
tracer_init(struct gc_heap *heap, size_t parallelism) {
  struct tracer *tracer = heap_tracer(heap);
  atomic_init(&tracer->active_tracers, 0);
  atomic_init(&tracer->running_tracers, 0);
  tracer->count = 0;
  pthread_mutex_init(&tracer->lock, NULL);
  pthread_cond_init(&tracer->cond, NULL);
  size_t desired_worker_count = parallelism;
  ASSERT(desired_worker_count);
  if (desired_worker_count > TRACE_WORKERS_MAX_COUNT)
    desired_worker_count = TRACE_WORKERS_MAX_COUNT;
  for (size_t i = 0; i < desired_worker_count; i++) {
    if (!trace_worker_init(&tracer->workers[i], heap, tracer, i))
      break;
    if (trace_worker_spawn(&tracer->workers[i]))
      tracer->worker_count++;
    else
      break;
  }
  return tracer->worker_count > 0;
}

static void tracer_prepare(struct gc_heap *heap) {
  struct tracer *tracer = heap_tracer(heap);
  for (size_t i = 0; i < tracer->worker_count; i++)
    tracer->workers[i].steal_id = 0;
}
static void tracer_release(struct gc_heap *heap) {
  struct tracer *tracer = heap_tracer(heap);
  for (size_t i = 0; i < tracer->worker_count; i++)
    trace_deque_release(&tracer->workers[i].deque);
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
tracer_share(struct local_tracer *trace) {
  DEBUG("tracer #%zu: sharing\n", trace->worker->id);
  for (size_t i = 0; i < LOCAL_TRACE_QUEUE_SHARE_AMOUNT; i++)
    trace_deque_push(trace->share_deque, local_trace_queue_pop(&trace->local));
}

static inline void
tracer_enqueue(struct gc_ref ref, struct gc_heap *heap, void *trace_data) {
  struct local_tracer *trace = trace_data;
  if (local_trace_queue_full(&trace->local))
    tracer_share(trace);
  local_trace_queue_push(&trace->local, ref);
}

static inline void
tracer_visit(struct gc_edge edge, struct gc_heap *heap, void *trace_data) {
  if (trace_edge(heap, edge))
    tracer_enqueue(gc_edge_ref(edge), heap, trace_data);
}

static inline void
tracer_visit_(struct gc_edge edge, struct gc_heap *heap, void *trace_data) {
  if (trace_edge(heap, edge)) {
    struct local_tracer *trace = trace_data;
    if (local_trace_queue_full(&trace->local))
      tracer_share(trace);
    local_trace_queue_push(&trace->local, gc_edge_ref(edge));
  }
}

static struct gc_ref
tracer_steal_from_worker(struct tracer *tracer, size_t id) {
  ASSERT(id < tracer->worker_count);
  return trace_deque_steal(&tracer->workers[id].deque);
}

static int
tracer_can_steal_from_worker(struct tracer *tracer, size_t id) {
  ASSERT(id < tracer->worker_count);
  return trace_deque_can_steal(&tracer->workers[id].deque);
}

static struct gc_ref
trace_worker_steal_from_any(struct trace_worker *worker, struct tracer *tracer) {
  for (size_t i = 0; i < tracer->worker_count; i++) {
    DEBUG("tracer #%zu: stealing from #%zu\n", worker->id, worker->steal_id);
    struct gc_ref obj = tracer_steal_from_worker(tracer, worker->steal_id);
    if (gc_ref_is_heap_object(obj)) {
      DEBUG("tracer #%zu: stealing got %p\n", worker->id,
            gc_ref_heap_object(obj));
      return obj;
    }
    worker->steal_id = (worker->steal_id + 1) % tracer->worker_count;
  }
  DEBUG("tracer #%zu: failed to steal\n", worker->id);
  return gc_ref_null();
}

static int
trace_worker_can_steal_from_any(struct trace_worker *worker, struct tracer *tracer) {
  DEBUG("tracer #%zu: checking if any worker has tasks\n", worker->id);
  for (size_t i = 0; i < tracer->worker_count; i++) {
    int res = tracer_can_steal_from_worker(tracer, worker->steal_id);
    if (res) {
      DEBUG("tracer #%zu: worker #%zu has tasks!\n", worker->id,
            worker->steal_id);
      return 1;
    }
    worker->steal_id = (worker->steal_id + 1) % tracer->worker_count;
  }
  DEBUG("tracer #%zu: nothing to steal\n", worker->id);
  return 0;
}

static int
trace_worker_check_termination(struct trace_worker *worker,
                              struct tracer *tracer) {
  // We went around all workers and nothing.  Enter termination phase.
  if (atomic_fetch_sub_explicit(&tracer->active_tracers, 1,
                                memory_order_relaxed) == 1) {
    DEBUG("  ->> tracer #%zu: DONE (no spinning) <<-\n", worker->id);
    return 1;
  }

  for (size_t spin_count = 0;; spin_count++) {
    if (trace_worker_can_steal_from_any(worker, tracer)) {
      atomic_fetch_add_explicit(&tracer->active_tracers, 1,
                                memory_order_relaxed);
      return 0;
    }
    if (atomic_load_explicit(&tracer->active_tracers,
                             memory_order_relaxed) == 0) {
      DEBUG("  ->> tracer #%zu: DONE <<-\n", worker->id);
      return 1;
    }
    // spin
    DEBUG("tracer #%zu: spinning #%zu\n", worker->id, spin_count);
    yield_for_spin(spin_count);
  }
}

static struct gc_ref
trace_worker_steal(struct local_tracer *trace) {
  struct trace_worker *worker = trace->worker;
  struct tracer *tracer = heap_tracer(worker->heap);

  // It could be that the worker's local trace queue has simply
  // overflowed.  In that case avoid contention by trying to pop
  // something from the worker's own queue.
  {
    DEBUG("tracer #%zu: trying to pop worker's own deque\n", worker->id);
    struct gc_ref obj = trace_deque_try_pop(&worker->deque);
    if (gc_ref_is_heap_object(obj))
      return obj;
  }

  while (1) {
    DEBUG("tracer #%zu: trying to steal\n", worker->id);
    struct gc_ref obj = trace_worker_steal_from_any(worker, tracer);
    if (gc_ref_is_heap_object(obj))
      return obj;

    if (trace_worker_check_termination(worker, tracer))
      return gc_ref_null();
  }
}

static void
trace_worker_trace(struct trace_worker *worker) {
  struct local_tracer trace;
  trace.worker = worker;
  trace.share_deque = &worker->deque;
  struct gc_heap *heap = worker->heap;
  local_trace_queue_init(&trace.local);

  size_t n = 0;
  DEBUG("tracer #%zu: running trace loop\n", worker->id);
  while (1) {
    struct gc_ref ref;
    if (!local_trace_queue_empty(&trace.local)) {
      ref = local_trace_queue_pop(&trace.local);
    } else {
      ref = trace_worker_steal(&trace);
      if (!gc_ref_is_heap_object(ref))
        break;
    }
    trace_one(ref, heap, &trace);
    n++;
  }
  DEBUG("tracer #%zu: done tracing, %zu objects traced\n", worker->id, n);

  trace_worker_finished_tracing(worker);
}

static inline void
tracer_enqueue_root(struct tracer *tracer, struct gc_ref ref) {
  struct trace_deque *worker0_deque = &tracer->workers[0].deque;
  trace_deque_push(worker0_deque, ref);
}

static inline void
tracer_enqueue_roots(struct tracer *tracer, struct gc_ref *objv,
                     size_t count) {
  struct trace_deque *worker0_deque = &tracer->workers[0].deque;
  trace_deque_push_many(worker0_deque, objv, count);
}

static inline void
tracer_trace(struct gc_heap *heap) {
  struct tracer *tracer = heap_tracer(heap);

  pthread_mutex_lock(&tracer->lock);
  long trace_count = tracer->count;
  pthread_mutex_unlock(&tracer->lock);

  DEBUG("starting trace; %zu workers\n", tracer->worker_count);
  DEBUG("waking workers\n");
  atomic_store_explicit(&tracer->active_tracers, tracer->worker_count,
                        memory_order_release);
  atomic_store_explicit(&tracer->running_tracers, tracer->worker_count,
                        memory_order_release);
  for (size_t i = 0; i < tracer->worker_count; i++)
    trace_worker_request_trace(&tracer->workers[i]);

  DEBUG("waiting on tracers\n");

  pthread_mutex_lock(&tracer->lock);
  while (tracer->count <= trace_count)
    pthread_cond_wait(&tracer->cond, &tracer->lock);
  pthread_mutex_unlock(&tracer->lock);

  DEBUG("trace finished\n");
}

#endif // PARALLEL_TRACER_H
