// For pthread_getattr_np.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>

#define GC_IMPL 1

#include "debug.h"
#include "gc-align.h"
#include "gc-assert.h"
#include "gc-inline.h"
#include "gc-platform.h"
#include "gc-stack.h"

static uintptr_t current_thread_hot_stack_addr(void) {
#ifdef __GNUC__
  return (uintptr_t)__builtin_frame_address(0);
#else
  uintptr_t local;
  return (uintptr_t)&local;
#endif
}

// FIXME: check platform stack growth direction.
#define HOTTER_THAN <=

static struct gc_stack_addr capture_current_thread_hot_stack_addr(void) {
  return gc_stack_addr(current_thread_hot_stack_addr());
}

static struct gc_stack_addr capture_current_thread_cold_stack_addr(void) {
  return gc_stack_addr(gc_platform_current_thread_stack_base());
}

void gc_stack_init(struct gc_stack *stack, struct gc_stack_addr base) {
  if (gc_stack_addr_is_empty (base))
    base = capture_current_thread_cold_stack_addr();
  stack->cold = stack->hot = base;
}

void gc_stack_capture_hot(struct gc_stack *stack) {
  stack->hot = capture_current_thread_hot_stack_addr();
  setjmp(stack->registers);
  GC_ASSERT(stack->hot.addr HOTTER_THAN stack->cold.addr);
}

static void* call_with_stack(void* (*)(struct gc_stack_addr, void*),
                             struct gc_stack_addr, void*) GC_NEVER_INLINE;
static void* call_with_stack(void* (*f)(struct gc_stack_addr, void *),
                             struct gc_stack_addr addr, void *arg) {
  return f(addr, arg);
}
void* gc_call_with_stack_addr(void* (*f)(struct gc_stack_addr base,
                                         void *arg),
                              void *arg) {
  struct gc_stack_addr base = capture_current_thread_hot_stack_addr();
  return call_with_stack(f, base, arg);
}

void gc_stack_visit(struct gc_stack *stack,
                    void (*visit)(uintptr_t low, uintptr_t high,
                                  struct gc_heap *heap, void *data),
                    struct gc_heap *heap,
                    void *data) {
  {
    uintptr_t low = (uintptr_t)stack->registers;
    GC_ASSERT(low == align_down(low, sizeof(uintptr_t)));
    uintptr_t high = low + sizeof(jmp_buf);
    DEBUG("found mutator register roots for %p: [%p,%p)\n", stack,
          (void*)low, (void*)high);
    visit(low, high, heap, data);
  }

  if (0 HOTTER_THAN 1) {
    DEBUG("found mutator stack roots for %p: [%p,%p)\n", stack,
          (void*)stack->hot.addr, (void*)stack->cold.addr);
    visit(align_up(stack->hot.addr, sizeof(uintptr_t)),
          align_down(stack->cold.addr, sizeof(uintptr_t)),
          heap, data);
  } else {
    DEBUG("found mutator stack roots for %p: [%p,%p)\n", stack,
          (void*)stack->cold.addr, (void*)stack->hot.addr);
    visit(align_up(stack->cold.addr, sizeof(uintptr_t)),
          align_down(stack->hot.addr, sizeof(uintptr_t)),
          heap, data);
  }
}
