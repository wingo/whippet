#ifndef GC_STACK_ADDR_H
#define GC_STACK_ADDR_H

#include "gc-assert.h"
#include "gc-visibility.h"

struct gc_stack_addr {
  uintptr_t addr;
};

static inline struct gc_stack_addr gc_empty_stack_addr (void) {
  return (struct gc_stack_addr){ 0 };
};

static inline int gc_stack_addr_is_empty (struct gc_stack_addr addr) {
  return addr.addr == 0;
};

static inline char* gc_stack_addr_as_pointer (struct gc_stack_addr addr) {
  GC_ASSERT(!gc_stack_addr_is_empty(addr));
  return (char*)addr.addr;
};

static inline struct gc_stack_addr gc_stack_addr (uintptr_t addr) {
  GC_ASSERT(addr);
  return (struct gc_stack_addr){ addr };
};

GC_API_ void* gc_call_with_stack_addr(void* (*f)(struct gc_stack_addr,
                                                 void *),
                                      void *data) GC_NEVER_INLINE;

GC_API_ int gc_stack_addr_is_colder(struct gc_stack_addr a,
                                    struct gc_stack_addr b);

#endif // GC_STACK_ADDR_H
