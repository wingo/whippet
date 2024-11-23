#ifndef GC_PLATFORM_H
#define GC_PLATFORM_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include <stdint.h>

#include "gc-visibility.h"

struct gc_heap;

GC_INTERNAL void gc_platform_init(void);
GC_INTERNAL uintptr_t gc_platform_current_thread_stack_base(void);
GC_INTERNAL
void gc_platform_visit_global_conservative_roots(void (*f)(uintptr_t start,
                                                           uintptr_t end,
                                                           struct gc_heap *heap,
                                                           void *data),
                                                 struct gc_heap *heap,
                                                 void *data);
GC_INTERNAL int gc_platform_processor_count(void);
GC_INTERNAL uint64_t gc_platform_monotonic_nanoseconds(void);

GC_INTERNAL size_t gc_platform_page_size(void);

struct gc_reservation {
  uintptr_t base;
  size_t size;
};

GC_INTERNAL
struct gc_reservation gc_platform_reserve_memory(size_t size, size_t alignment);
GC_INTERNAL
void*
gc_platform_acquire_memory_from_reservation(struct gc_reservation reservation,
                                            size_t offset, size_t size);
GC_INTERNAL
void gc_platform_release_reservation(struct gc_reservation reservation);

GC_INTERNAL void* gc_platform_acquire_memory(size_t size, size_t alignment);
GC_INTERNAL void gc_platform_release_memory(void *base, size_t size);

GC_INTERNAL int gc_platform_populate_memory(void *addr, size_t size);
GC_INTERNAL int gc_platform_discard_memory(void *addr, size_t size);

#endif // GC_PLATFORM_H
