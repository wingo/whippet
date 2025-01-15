#ifndef FIELD_SET_H
#define FIELD_SET_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "assert.h"
#include "gc-edge.h"
#include "gc-lock.h"
#include "tracer.h"

#define GC_EDGE_BUFFER_CAPACITY 510

struct gc_edge_buffer {
  struct gc_edge_buffer *next;
  size_t size;
  struct gc_edge edges[GC_EDGE_BUFFER_CAPACITY];
};

// Lock-free.
struct gc_edge_buffer_list {
  struct gc_edge_buffer *head;
};

// With a lock.
struct gc_edge_buffer_stack {
  struct gc_edge_buffer_list list;
};

struct gc_field_set {
  struct gc_edge_buffer_list full;
  struct gc_edge_buffer_stack partly_full;
  struct gc_edge_buffer_list empty;
  size_t count;
  pthread_mutex_t lock;
};

struct gc_field_set_writer {
  struct gc_edge_buffer *buf;
  struct gc_field_set *set;
};

static void
gc_edge_buffer_list_push(struct gc_edge_buffer_list *list,
                         struct gc_edge_buffer *buf) {
  GC_ASSERT(!buf->next);
  struct gc_edge_buffer *next =
    atomic_load_explicit(&list->head, memory_order_relaxed);
  do {
    buf->next = next;
  } while (!atomic_compare_exchange_weak_explicit(&list->head, &next, buf,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
}

static struct gc_edge_buffer*
gc_edge_buffer_list_pop(struct gc_edge_buffer_list *list) {
  struct gc_edge_buffer *head =
    atomic_load_explicit(&list->head, memory_order_acquire);
  struct gc_edge_buffer *next;
  do {
    if (!head) return NULL;
    next = head->next;
  } while (!atomic_compare_exchange_weak_explicit(&list->head, &head, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
  head->next = NULL;
  return head;
}

static void
gc_edge_buffer_stack_push(struct gc_edge_buffer_stack *stack,
                          struct gc_edge_buffer *buf,
                          const struct gc_lock *lock) {
  GC_ASSERT(!buf->next);
  buf->next = stack->list.head;
  stack->list.head = buf;
}

static struct gc_edge_buffer*
gc_edge_buffer_stack_pop(struct gc_edge_buffer_stack *stack,
                         const struct gc_lock *lock) {
  struct gc_edge_buffer *head = stack->list.head;
  if (head) {
    stack->list.head = head->next;
    head->next = NULL;
  }
  return head;
}

static void
gc_field_set_init(struct gc_field_set *set) {
  memset(set, 0, sizeof(*set));
  pthread_mutex_init(&set->lock, NULL);
}

static struct gc_edge_buffer*
gc_field_set_acquire_buffer(struct gc_field_set *set) {
  struct gc_edge_buffer *ret;

  ret = gc_edge_buffer_list_pop(&set->empty);
  if (ret) return ret;

  struct gc_lock lock = gc_lock_acquire(&set->lock);
  ret = gc_edge_buffer_stack_pop(&set->partly_full, &lock);
  gc_lock_release(&lock);
  if (ret) return ret;

  // atomic inc count
  ret = malloc(sizeof(*ret));
  if (!ret) {
    perror("Failed to allocate remembered set");
    GC_CRASH();
  }
  memset(ret, 0, sizeof(*ret));
  return ret;
}

static void
gc_field_set_release_buffer(struct gc_field_set *set,
                            struct gc_edge_buffer *buf) {
  if (buf->size == GC_EDGE_BUFFER_CAPACITY) {
    gc_edge_buffer_list_push(&set->full, buf);
  } else {
    struct gc_lock lock = gc_lock_acquire(&set->lock);
    gc_edge_buffer_stack_push(&set->partly_full, buf, &lock);
    gc_lock_release(&lock);
  }
}

static void
gc_field_set_add_roots(struct gc_field_set *set, struct gc_tracer *tracer) {
  struct gc_edge_buffer *buf;
  struct gc_lock lock = gc_lock_acquire(&set->lock);
  while ((buf = gc_edge_buffer_stack_pop(&set->partly_full, &lock)))
    gc_tracer_add_root(tracer, gc_root_edge_buffer(buf));
  while ((buf = gc_edge_buffer_list_pop(&set->full)))
    gc_tracer_add_root(tracer, gc_root_edge_buffer(buf));
  gc_lock_release(&lock);
}

static void
gc_field_set_clear(struct gc_field_set *set,
                   void (*forget_edge)(struct gc_edge, struct gc_heap*),
                   struct gc_heap *heap) {
  struct gc_edge_buffer *partly_full = set->partly_full.list.head;
  struct gc_edge_buffer *full = set->full.head;
  // Clear the full and partly full sets now so that if a collector
  // wanted to it could re-add an edge to the remembered set.
  set->partly_full.list.head = NULL;
  set->full.head = NULL;
  struct gc_edge_buffer *buf, *next;
  for (buf = partly_full; buf; buf = next) {
    next = buf->next;
    buf->next = NULL;
    if (forget_edge)
      for (size_t i = 0; i < buf->size; i++)
        forget_edge(buf->edges[i], heap);
    buf->size = 0;
    gc_edge_buffer_list_push(&set->empty, buf);
  }
  for (buf = full; buf; buf = next) {
    next = buf->next;
    buf->next = NULL;
    if (forget_edge)
      for (size_t i = 0; i < buf->size; i++)
        forget_edge(buf->edges[i], heap);
    buf->size = 0;
    gc_edge_buffer_list_push(&set->empty, buf);
  }
}

static inline void
gc_field_set_visit_edge_buffer(struct gc_field_set *set,
                               struct gc_edge_buffer *buf,
                               int (*visit)(struct gc_edge,
                                            struct gc_heap*,
                                            void *data),
                               struct gc_heap *heap,
                               void *data) GC_ALWAYS_INLINE;
static inline void
gc_field_set_visit_edge_buffer(struct gc_field_set *set,
                               struct gc_edge_buffer *buf,
                               int (*visit)(struct gc_edge,
                                            struct gc_heap*,
                                            void *data),
                               struct gc_heap *heap,
                               void *data) {
  size_t i = 0;
  while (i < buf->size) {
    if (visit(buf->edges[i], heap, data))
      i++;
    else
      buf->edges[i] = buf->edges[--buf->size];
  }
  gc_field_set_release_buffer(set, buf);
}

static void
gc_field_set_writer_release_buffer(struct gc_field_set_writer *writer) {
  if (writer->buf) {
    gc_field_set_release_buffer(writer->set, writer->buf);
    writer->buf = NULL;
  }
}

static void
gc_field_set_writer_init(struct gc_field_set_writer *writer,
                         struct gc_field_set *set) {
  writer->set = set;
  writer->buf = NULL;
}

static void
gc_field_set_writer_add_edge(struct gc_field_set_writer *writer,
                             struct gc_edge edge) {
  struct gc_edge_buffer *buf = writer->buf;
  if (GC_UNLIKELY(!buf))
    writer->buf = buf = gc_field_set_acquire_buffer(writer->set);
  GC_ASSERT(buf->size < GC_EDGE_BUFFER_CAPACITY);
  buf->edges[buf->size++] = edge;
  if (GC_UNLIKELY(buf->size == GC_EDGE_BUFFER_CAPACITY)) {
    gc_edge_buffer_list_push(&writer->set->full, buf);
    writer->buf = NULL;
  }
}

#endif // FIELD_SET_H
