#ifndef GC_STACK_H
#define GC_STACK_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "gc-inline.h"
#include <setjmp.h>

struct gc_stack_addr {
  uintptr_t addr;
};

struct gc_stack {
  struct gc_stack_addr cold;
  struct gc_stack_addr hot;
  jmp_buf registers;
};

GC_INTERNAL void gc_stack_init(struct gc_stack *stack,
                               struct gc_stack_addr *base);
GC_INTERNAL void gc_stack_capture_hot(struct gc_stack *stack);
GC_INTERNAL void gc_stack_visit(struct gc_stack *stack,
                                void (*visit)(uintptr_t low, uintptr_t high,
                                              void *data),
                                void *data);

#endif // GC_STACK_H
