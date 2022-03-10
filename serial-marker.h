#ifndef SERIAL_TRACE_H
#define SERIAL_TRACE_H

#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"

struct mark_stack {
  size_t size;
  size_t next;
  uintptr_t *buf;
};

static const size_t mark_stack_max_size =
  (1ULL << (sizeof(uintptr_t) * 8 - 1)) / sizeof(uintptr_t);
static const size_t mark_stack_release_byte_threshold = 1 * 1024 * 1024;

static void*
mark_stack_alloc(size_t size) {
  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("Failed to grow mark stack");
    DEBUG("Failed to allocate %zu bytes", size);
    return NULL;
  }
  return mem;
}

static int
mark_stack_init(struct mark_stack *stack) {
  stack->size = getpagesize();
  stack->next = 0;
  stack->buf = mark_stack_alloc(stack->size);
  return !!stack->buf;
}
  
static int
mark_stack_grow(struct mark_stack *stack) {
  uintptr_t size = stack->size;
  if (size >= mark_stack_max_size) {
    DEBUG("mark stack already at max size of %zu bytes", size);
    return 0;
  }
  size *= 2;
  uintptr_t *buf = mark_stack_alloc(size);
  if (!buf)
    return 0;
  memcpy(buf, stack->buf, stack->next * sizeof(uintptr_t));
  munmap(stack->buf, stack->size * sizeof(uintptr_t));
  stack->size = size;
  stack->buf = buf;
  return 1;
}
  
static inline void
mark_stack_push(struct mark_stack *stack, void *p) {
  size_t next = stack->next;
  if (UNLIKELY(next == stack->size)) {
    if (!mark_stack_grow(stack))
      abort();
  }
  stack->buf[next] = (uintptr_t)p;
  stack->next = next + 1;
}

static inline void*
mark_stack_pop(struct mark_stack *stack) {
  size_t next = stack->next;
  if (UNLIKELY(next == 0))
    return NULL;
  uintptr_t ret = stack->buf[next - 1];
  stack->next = next - 1;
  return (void*)ret;
}

static void
mark_stack_release(struct mark_stack *stack) {
  size_t byte_size = stack->size * sizeof(uintptr_t);
  if (byte_size >= mark_stack_release_byte_threshold)
    madvise(stack->buf, byte_size, MADV_DONTNEED);
}

static void
mark_stack_destroy(struct mark_stack *stack) {
  size_t byte_size = stack->size * sizeof(uintptr_t);
  munmap(stack->buf, byte_size);
}

struct marker {
  struct mark_stack stack;
};

struct context;
static inline struct marker* context_marker(struct context *cx);

static int
marker_init(struct context *cx) {
  return mark_stack_init(&context_marker(cx)->stack);
}
static void marker_prepare(struct context *cx) {}
static void marker_release(struct context *cx) {
  mark_stack_release(&context_marker(cx)->stack);
}

struct gcobj;
static inline void marker_visit(struct context *cx, void **loc) __attribute__((always_inline));
static inline void marker_trace(struct context *cx,
                                void (*)(struct context *, struct gcobj *))
  __attribute__((always_inline));
static inline int mark_object(struct gcobj *obj) __attribute__((always_inline));

static inline void
marker_visit(struct context *cx, void **loc) {
  struct gcobj *obj = *loc;
  if (obj) {
    __builtin_prefetch(obj);
    mark_stack_push(&context_marker(cx)->stack, obj);
  }
}
static inline void
marker_visit_root(struct context *cx, void **loc) {
  marker_visit(cx, loc);
}
static inline void
marker_trace(struct context *cx,
             void (*process)(struct context *, struct gcobj *)) {
  struct gcobj *obj;
  while ((obj = mark_stack_pop(&context_marker(cx)->stack)))
    if (mark_object(obj))
      process(cx, obj);
}

#endif // SERIAL_MARK_H
