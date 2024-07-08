#ifndef PARALLEL_TRACER_H
#define PARALLEL_TRACER_H

#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "gc-inline.h"
#include "local-worklist.h"
#include "shared-worklist.h"
#include "spin.h"
#include "tracer.h"

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
  struct gc_tracer *tracer;
  size_t id;
  size_t steal_id;
  pthread_t thread;
  enum trace_worker_state state;
  pthread_mutex_t lock;
  struct shared_worklist deque;
  struct gc_trace_worker_data *data;
};

#define TRACE_WORKERS_MAX_COUNT 8

struct gc_tracer {
  struct gc_heap *heap;
  atomic_size_t active_tracers;
  size_t worker_count;
  long epoch;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct trace_worker workers[TRACE_WORKERS_MAX_COUNT];
};

struct local_tracer {
  struct trace_worker *worker;
  struct shared_worklist *share_deque;
  struct local_worklist local;
};

static int
trace_worker_init(struct trace_worker *worker, struct gc_heap *heap,
                  struct gc_tracer *tracer, size_t id) {
  worker->heap = heap;
  worker->tracer = tracer;
  worker->id = id;
  worker->steal_id = 0;
  worker->thread = 0;
  worker->state = TRACE_WORKER_STOPPED;
  pthread_mutex_init(&worker->lock, NULL);
  worker->data = NULL;
  return shared_worklist_init(&worker->deque);
}

static void trace_worker_trace(struct trace_worker *worker);

static void*
trace_worker_thread(void *data) {
  struct trace_worker *worker = data;
  struct gc_tracer *tracer = worker->tracer;
  long trace_epoch = 0;

  pthread_mutex_lock(&worker->lock);
  while (1) {
    long epoch = atomic_load_explicit(&tracer->epoch, memory_order_acquire);
    if (trace_epoch != epoch) {
      trace_epoch = epoch;
      trace_worker_trace(worker);
    }
    pthread_cond_wait(&tracer->cond, &worker->lock);
  }
  return NULL;
}

static int
trace_worker_spawn(struct trace_worker *worker) {
  if (pthread_create(&worker->thread, NULL, trace_worker_thread, worker)) {
    perror("spawning tracer thread failed");
    return 0;
  }

  return 1;
}

static int
gc_tracer_init(struct gc_tracer *tracer, struct gc_heap *heap,
               size_t parallelism) {
  tracer->heap = heap;
  atomic_init(&tracer->active_tracers, 0);
  tracer->epoch = 0;
  pthread_mutex_init(&tracer->lock, NULL);
  pthread_cond_init(&tracer->cond, NULL);
  size_t desired_worker_count = parallelism;
  ASSERT(desired_worker_count);
  if (desired_worker_count > TRACE_WORKERS_MAX_COUNT)
    desired_worker_count = TRACE_WORKERS_MAX_COUNT;
  if (!trace_worker_init(&tracer->workers[0], heap, tracer, 0))
    return 0;
  tracer->worker_count++;
  for (size_t i = 1; i < desired_worker_count; i++) {
    if (!trace_worker_init(&tracer->workers[i], heap, tracer, i))
      break;
    if (trace_worker_spawn(&tracer->workers[i]))
      tracer->worker_count++;
    else
      break;
  }
  return 1;
}

static void gc_tracer_prepare(struct gc_tracer *tracer) {
  for (size_t i = 0; i < tracer->worker_count; i++)
    tracer->workers[i].steal_id = 0;
}
static void gc_tracer_release(struct gc_tracer *tracer) {
  for (size_t i = 0; i < tracer->worker_count; i++)
    shared_worklist_release(&tracer->workers[i].deque);
}

static inline void
tracer_unpark_all_workers(struct gc_tracer *tracer) {
  long old_epoch =
    atomic_fetch_add_explicit(&tracer->epoch, 1, memory_order_acq_rel);
  long epoch = old_epoch + 1;
  DEBUG("starting trace; %zu workers; epoch=%ld\n", tracer->worker_count,
        epoch);
  pthread_cond_broadcast(&tracer->cond);
}

static inline void
tracer_maybe_unpark_workers(struct gc_tracer *tracer) {
  size_t active =
    atomic_load_explicit(&tracer->active_tracers, memory_order_acquire);
  if (active < tracer->worker_count)
    tracer_unpark_all_workers(tracer);
}

static inline void
tracer_share(struct local_tracer *trace) {
  DEBUG("tracer #%zu: sharing\n", trace->worker->id);
  size_t to_share = LOCAL_WORKLIST_SHARE_AMOUNT;
  while (to_share) {
    struct gc_ref *objv;
    size_t count = local_worklist_pop_many(&trace->local, &objv, to_share);
    shared_worklist_push_many(trace->share_deque, objv, count);
    to_share -= count;
  }
  tracer_maybe_unpark_workers(trace->worker->tracer);
}

static inline void
gc_tracer_enqueue(struct gc_tracer *tracer, struct gc_ref ref,
                  void *trace_data) {
  struct local_tracer *trace = trace_data;
  if (local_worklist_full(&trace->local))
    tracer_share(trace);
  local_worklist_push(&trace->local, ref);
}

static struct gc_ref
tracer_steal_from_worker(struct gc_tracer *tracer, size_t id) {
  ASSERT(id < tracer->worker_count);
  return shared_worklist_steal(&tracer->workers[id].deque);
}

static int
tracer_can_steal_from_worker(struct gc_tracer *tracer, size_t id) {
  ASSERT(id < tracer->worker_count);
  return shared_worklist_can_steal(&tracer->workers[id].deque);
}

static struct gc_ref
trace_worker_steal_from_any(struct trace_worker *worker, struct gc_tracer *tracer) {
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
trace_worker_can_steal_from_any(struct trace_worker *worker,
                                struct gc_tracer *tracer) {
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
trace_worker_should_continue(struct trace_worker *worker) {
  // Helper workers should park themselves immediately if they have no work.
  if (worker->id != 0)
    return 0;

  struct gc_tracer *tracer = worker->tracer;

  for (size_t spin_count = 0;; spin_count++) {
    if (atomic_load_explicit(&tracer->active_tracers,
                             memory_order_acquire) == 1) {
      // All trace workers have exited except us, the main worker.  We are
      // probably done, but we need to synchronize to be sure that there is no
      // work pending, for example if a worker had a spurious wakeup.  Skip
      // worker 0 (the main worker).
      size_t locked = 1;
      while (locked < tracer->worker_count) {
        if (pthread_mutex_trylock(&tracer->workers[locked].lock) == 0)
          locked++;
        else
          break;
      }
      int done = (locked == tracer->worker_count) &&
        !trace_worker_can_steal_from_any(worker, tracer);
      while (locked > 1)
        pthread_mutex_unlock(&tracer->workers[--locked].lock);
      return !done;
    }
    // spin
    DEBUG("checking for termination: spinning #%zu\n", spin_count);
    yield_for_spin(spin_count);
  }
}

static struct gc_ref
trace_worker_steal(struct local_tracer *trace) {
  struct trace_worker *worker = trace->worker;
  struct gc_tracer *tracer = worker->tracer;

  // It could be that the worker's local trace queue has simply
  // overflowed.  In that case avoid contention by trying to pop
  // something from the worker's own queue.
  {
    DEBUG("tracer #%zu: trying to pop worker's own deque\n", worker->id);
    struct gc_ref obj = shared_worklist_try_pop(&worker->deque);
    if (gc_ref_is_heap_object(obj))
      return obj;
  }

  DEBUG("tracer #%zu: trying to steal\n", worker->id);
  struct gc_ref obj = trace_worker_steal_from_any(worker, tracer);
  if (gc_ref_is_heap_object(obj))
    return obj;

  return gc_ref_null();
}

static void
trace_with_data(struct gc_tracer *tracer,
                struct gc_heap *heap,
                struct gc_trace_worker_data *worker_data,
                void *data) {
  struct trace_worker *worker = data;
  atomic_fetch_add_explicit(&tracer->active_tracers, 1, memory_order_acq_rel);
  worker->data = worker_data;

  struct local_tracer trace;
  trace.worker = worker;
  trace.share_deque = &worker->deque;
  local_worklist_init(&trace.local);

  size_t n = 0;
  DEBUG("tracer #%zu: running trace loop\n", worker->id);

  do {
    while (1) {
      struct gc_ref ref;
      if (!local_worklist_empty(&trace.local)) {
        ref = local_worklist_pop(&trace.local);
      } else {
        ref = trace_worker_steal(&trace);
        if (!gc_ref_is_heap_object(ref))
          break;
      }
      trace_one(ref, heap, &trace);
      n++;
    }
  } while (trace_worker_should_continue(worker));

  DEBUG("tracer #%zu: done tracing, %zu objects traced\n", worker->id, n);

  worker->data = NULL;
  atomic_fetch_sub_explicit(&tracer->active_tracers, 1, memory_order_acq_rel);
}

static void
trace_worker_trace(struct trace_worker *worker) {
  gc_trace_worker_call_with_data(trace_with_data, worker->tracer,
                                 worker->heap, worker);
}

static inline void
gc_tracer_enqueue_root(struct gc_tracer *tracer, struct gc_ref ref) {
  struct shared_worklist *worker0_deque = &tracer->workers[0].deque;
  shared_worklist_push(worker0_deque, ref);
}

static inline void
gc_tracer_enqueue_roots(struct gc_tracer *tracer, struct gc_ref *objv,
                        size_t count) {
  struct shared_worklist *worker0_deque = &tracer->workers[0].deque;
  shared_worklist_push_many(worker0_deque, objv, count);
}

static inline void
gc_tracer_trace(struct gc_tracer *tracer) {
  DEBUG("starting trace; %zu workers\n", tracer->worker_count);

  ssize_t parallel_threshold =
    LOCAL_WORKLIST_SIZE - LOCAL_WORKLIST_SHARE_AMOUNT;
  if (shared_worklist_size(&tracer->workers[0].deque) >= parallel_threshold) {
    DEBUG("waking workers\n");
    tracer_unpark_all_workers(tracer);
  } else {
    DEBUG("starting in local-only mode\n");
  }

  trace_worker_trace(&tracer->workers[0]);

  DEBUG("trace finished\n");
}

#endif // PARALLEL_TRACER_H
