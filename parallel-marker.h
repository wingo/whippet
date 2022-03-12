#ifndef SERIAL_TRACE_H
#define SERIAL_TRACE_H

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

#undef LOAD_RELAXED
#undef STORE_RELAXED
#undef LOAD_ACQUIRE
#undef STORE_RELEASE
#undef LOAD_CONSUME

#define LOCAL_MARK_QUEUE_SIZE 64
#define LOCAL_MARK_QUEUE_MASK 63
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
static inline int
local_mark_queue_empty(struct local_mark_queue *q) {
  return q->read == q->write;
}
static inline int
local_mark_queue_full(struct local_mark_queue *q) {
  return q->read + LOCAL_MARK_QUEUE_SIZE == q->write;
}
static inline void
local_mark_queue_push(struct local_mark_queue *q, uintptr_t v) {
  q->data[q->write++ & LOCAL_MARK_QUEUE_MASK] = v;
}
static inline uintptr_t
local_mark_queue_pop(struct local_mark_queue *q) {
  return q->data[q->read++ & LOCAL_MARK_QUEUE_MASK];
}

struct mark_notify {
  size_t notifiers;
  int pending;
  pthread_mutex_t lock;
  pthread_cond_t cond;
};

static void
mark_notify_init(struct mark_notify *notify) {
  notify->notifiers = 0;
  notify->pending = 0;
  pthread_mutex_init(&notify->lock, NULL);
  pthread_cond_init(&notify->cond, NULL);
}

static void
mark_notify_destroy(struct mark_notify *notify) {
  pthread_mutex_destroy(&notify->lock);
  pthread_cond_destroy(&notify->cond);
}

static void
mark_notify_add_notifier(struct mark_notify *notify) {
  pthread_mutex_lock(&notify->lock);
  notify->notifiers++;
  pthread_mutex_unlock(&notify->lock);
}

static void
mark_notify_remove_notifier(struct mark_notify *notify) {
  pthread_mutex_lock(&notify->lock);
  notify->notifiers--;
  if (notify->notifiers == 0)
    pthread_cond_signal(&notify->cond);
  pthread_mutex_unlock(&notify->lock);
}

enum mark_notify_status {
  MARK_NOTIFY_DONE,
  MARK_NOTIFY_WOKE
};
static enum mark_notify_status
mark_notify_wait(struct mark_notify *notify) {
  enum mark_notify_status res;

  pthread_mutex_lock(&notify->lock);

  if (notify->pending) {
    res = MARK_NOTIFY_WOKE;
    notify->pending = 0;
    goto done;
  }

  if (notify->notifiers == 0) {
    res = MARK_NOTIFY_DONE;
    goto done;
  }

  // Spurious wakeup is OK.
  pthread_cond_wait(&notify->cond, &notify->lock);
  res = MARK_NOTIFY_WOKE;
  notify->pending = 0;

done:
  pthread_mutex_unlock(&notify->lock);
  return res;
}

static void
mark_notify_wake(struct mark_notify *notify) {
  pthread_mutex_lock(&notify->lock);
  notify->pending = 1;
  pthread_cond_signal(&notify->cond);
  pthread_mutex_unlock(&notify->lock);
}

// A mostly lock-free multi-producer, single consumer queue, largely
// inspired by Rust's std::sync::channel.
//
// https://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue

struct mark_channel_message {
  struct mark_channel_message * _Atomic next;
  // Payload will be zero only for free messages, and for the sentinel
  // message.
  atomic_uintptr_t payload;
};

#define MARK_CHANNEL_WRITER_MESSAGE_COUNT ((size_t)1024)

struct mark_channel {
  union {
    struct mark_channel_message* _Atomic head;
    char head_padding[64];
  };
  union {
    atomic_size_t length;
    char length_padding[64];
  };
  struct mark_channel_message* tail;
  struct mark_channel_message sentinel;

  struct mark_notify notify;
};

struct mark_channel_writer {
  struct mark_channel_message messages[MARK_CHANNEL_WRITER_MESSAGE_COUNT];
  size_t next_message;

  struct mark_channel *channel;
};

static void
mark_channel_init(struct mark_channel *ch) {
  memset(ch, 0, sizeof(*ch));
  atomic_init(&ch->head, &ch->sentinel);
  atomic_init(&ch->length, 0);
  mark_notify_init(&ch->notify);
  ch->tail = &ch->sentinel;
}

static void
mark_channel_destroy(struct mark_channel *ch) {
  mark_notify_destroy(&ch->notify);
}

static void
mark_channel_push(struct mark_channel *ch, struct mark_channel_message *msg) {
  ASSERT(msg->payload);
  atomic_store_explicit(&msg->next, NULL, memory_order_relaxed);

  struct mark_channel_message *prev =
    atomic_exchange_explicit(&ch->head, msg, memory_order_acq_rel);

  atomic_store_explicit(&prev->next, msg, memory_order_release);

  size_t old_length =
    atomic_fetch_add_explicit(&ch->length, 1, memory_order_relaxed);
  if (old_length == 0)
    mark_notify_wake(&ch->notify);
}

static uintptr_t
mark_channel_try_pop(struct mark_channel *ch) {
  struct mark_channel_message *tail = ch->tail;
  struct mark_channel_message *next =
    atomic_load_explicit(&tail->next, memory_order_acquire);

  if (next) {
    ch->tail = next;
    uintptr_t payload =
      atomic_load_explicit(&next->payload, memory_order_acquire);
    ASSERT(payload != 0);
    // Indicate to the writer that the old tail node can now be re-used.
    // Note though that the new tail node is floating garbage; its
    // payload has been popped but the node itself is still part of the
    // queue.  Care has to be taken to ensure that any remaining queue
    // entries are popped before the associated channel writer's
    // messages are deallocated.
    atomic_store_explicit(&tail->payload, 0, memory_order_release);
    atomic_fetch_sub_explicit(&ch->length, 1, memory_order_relaxed);
    return payload;
  }

  // if (atomic_load_explicit(&ch->head) == tail) return EMPTY else INCONSISTENT
  return 0;
}

static uintptr_t
mark_channel_pop(struct mark_channel *ch) {
  while (1) {
    uintptr_t ret = mark_channel_try_pop(ch);
    if (ret)
      return ret;

    if (atomic_load_explicit(&ch->length, memory_order_relaxed) == 0) {
      if (mark_notify_wait(&ch->notify) == MARK_NOTIFY_DONE)
        return 0;
    }
  }
}

static void
mark_channel_writer_init(struct mark_channel *ch,
                         struct mark_channel_writer *writer) {
  memset(writer, 0, sizeof(*writer));
  writer->channel = ch;
}

static void
mark_channel_write(struct mark_channel_writer *writer, uintptr_t payload) {
  ASSERT(payload);
  struct mark_channel_message *msg = &writer->messages[writer->next_message];
  while (atomic_load_explicit(&msg->payload, memory_order_acquire) != 0)
    sched_yield();
  writer->next_message++;
  if (writer->next_message == MARK_CHANNEL_WRITER_MESSAGE_COUNT)
    writer->next_message = 0;
  atomic_store_explicit(&msg->payload, payload, memory_order_release);
  mark_channel_push(writer->channel, msg);
}

static void
mark_channel_writer_activate(struct mark_channel_writer *writer) {
  mark_notify_add_notifier(&writer->channel->notify);
}
static void
mark_channel_writer_deactivate(struct mark_channel_writer *writer) {
  mark_notify_remove_notifier(&writer->channel->notify);
}

enum mark_worker_state {
  MARK_WORKER_STOPPED,
  MARK_WORKER_IDLE,
  MARK_WORKER_MARKING,
  MARK_WORKER_STOPPING,
  MARK_WORKER_DEAD
};

struct mark_worker {
  struct context *cx;
  pthread_t thread;
  enum mark_worker_state state;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct mark_channel_writer writer;
};

#define MARK_WORKERS_MAX_COUNT 8

struct marker {
  struct mark_deque deque;
  struct mark_channel overflow;
  size_t worker_count;
  struct mark_worker workers[MARK_WORKERS_MAX_COUNT];
};

struct local_marker {
  struct mark_worker *worker;
  struct mark_deque *deque;
  struct context *cx;
  struct local_mark_queue local;
};

struct context;
static inline struct marker* context_marker(struct context *cx);

static size_t number_of_current_processors(void) { return 1; }

static void
mark_worker_init(struct mark_worker *worker, struct context *cx,
                   struct marker *marker) {
  worker->cx = cx;
  worker->thread = 0;
  worker->state = MARK_WORKER_STOPPED;
  pthread_mutex_init(&worker->lock, NULL);
  pthread_cond_init(&worker->cond, NULL);
  mark_channel_writer_init(&marker->overflow, &worker->writer);
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
  pthread_mutex_lock(&worker->lock);
  ASSERT(worker->state == MARK_WORKER_IDLE);
  mark_channel_writer_activate(&worker->writer);
  worker->state = MARK_WORKER_MARKING;
  pthread_cond_signal(&worker->cond);
  pthread_mutex_unlock(&worker->lock);
}  

static void
mark_worker_finished_marking(struct mark_worker *worker) {
  // Signal controller that we are done with marking.
  mark_channel_writer_deactivate(&worker->writer);
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
marker_init(struct context *cx) {
  struct marker *marker = context_marker(cx);
  if (!mark_deque_init(&marker->deque))
    return 0;
  mark_channel_init(&marker->overflow);
  size_t desired_worker_count = 0;
  if (getenv("GC_MARKERS"))
    desired_worker_count = atoi(getenv("GC_MARKERS"));
  if (desired_worker_count == 0)
    desired_worker_count = number_of_current_processors();
  if (desired_worker_count > MARK_WORKERS_MAX_COUNT)
    desired_worker_count = MARK_WORKERS_MAX_COUNT;
  for (size_t i = 0; i < desired_worker_count; i++) {
    mark_worker_init(&marker->workers[i], cx, marker);
    if (mark_worker_spawn(&marker->workers[i]))
      marker->worker_count++;
    else
      break;
  }
  return marker->worker_count > 0;
}

static void marker_prepare(struct context *cx) {}
static void marker_release(struct context *cx) {
  mark_deque_release(&context_marker(cx)->deque);
}

struct gcobj;
static inline void marker_visit(void **loc, void *mark_data) ALWAYS_INLINE;
static inline void trace_one(struct gcobj *obj, void *mark_data) ALWAYS_INLINE;
static inline int mark_object(struct context *cx,
                              struct gcobj *obj) ALWAYS_INLINE;

static inline void
marker_visit(void **loc, void *mark_data) {
  struct local_marker *mark = mark_data;
  struct gcobj *obj = *loc;
  if (obj && mark_object(mark->cx, obj)) {
    if (!local_mark_queue_full(&mark->local))
      local_mark_queue_push(&mark->local, (uintptr_t)obj);
    else {
      mark_channel_write(&mark->worker->writer, (uintptr_t)obj);
    }
  }
}

static void
mark_worker_mark(struct mark_worker *worker) {
  struct local_marker mark;
  mark.worker = worker;
  mark.deque = &context_marker(worker->cx)->deque;
  mark.cx = worker->cx;
  local_mark_queue_init(&mark.local);

  size_t n = 0;
  DEBUG("marker %p: running mark loop\n", worker);
  while (1) {
    uintptr_t addr;
    if (!local_mark_queue_empty(&mark.local)) {
      addr = local_mark_queue_pop(&mark.local);
    } else {
      addr = mark_deque_steal(mark.deque);
      if (addr == mark_deque_empty)
        break;
      if (addr == mark_deque_abort)
        continue;
    }
    trace_one((struct gcobj*)addr, &mark);
    n++;
  }
  DEBUG("marker %p: done marking, %zu objects traced\n", worker, n);

  mark_worker_finished_marking(worker);
}

static inline void
marker_visit_root(void **loc, struct context *cx) {
  struct gcobj *obj = *loc;
  if (obj && mark_object(cx, obj))
    mark_deque_push(&context_marker(cx)->deque, (uintptr_t)obj);
}

static inline void
marker_trace(struct context *cx) {
  struct marker *marker = context_marker(cx);

  DEBUG("starting trace; %zu workers\n", marker->worker_count);
  while (1) {
    DEBUG("waking workers\n");
    for (size_t i = 0; i < marker->worker_count; i++)
      mark_worker_request_mark(&marker->workers[i]);

    DEBUG("running controller loop\n");
    size_t n = 0;
    while (1) {
      uintptr_t addr = mark_channel_pop(&marker->overflow);
      if (!addr)
        break;
      mark_deque_push(&marker->deque, addr);
      n++;
    }
    DEBUG("controller loop done, %zu objects sent for rebalancing\n", n);

    // As in the ISMM'16 paper, it's possible that a worker decides to
    // stop because the deque is empty, but actually there was an
    // in-flight object in the mark channel that we hadn't been able to
    // push yet.  Loop in that case.
    {
      uintptr_t addr = mark_deque_try_pop(&marker->deque);
      if (addr == mark_deque_empty)
        break;
      DEBUG("--> controller looping again due to slop\n");
      mark_deque_push(&marker->deque, addr);
    }
  }
  ASSERT(atomic_load(&marker->overflow.length) == 0);
  ASSERT(atomic_load(&marker->overflow.head) == marker->overflow.tail);
  DEBUG("trace finished\n");
}

#endif // SERIAL_MARK_H
