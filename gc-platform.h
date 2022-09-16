#ifndef GC_PLATFORM_H
#define GC_PLATFORM_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include <stdint.h>

#include "gc-visibility.h"

GC_INTERNAL void gc_platform_init(void);
GC_INTERNAL uintptr_t gc_platform_current_thread_stack_base(void);
GC_INTERNAL
void gc_platform_visit_global_conservative_roots(void (*f)(uintptr_t start,
                                                           uintptr_t end,
                                                           void *data),
                                                 void *data);

#endif // GC_PLATFORM_H
