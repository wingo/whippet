#ifndef GC_ALIGN_H
#define GC_ALIGN_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include <stdint.h>

static inline uintptr_t align_down(uintptr_t addr, size_t align) {
  return addr & ~(align - 1);
}
static inline uintptr_t align_up(uintptr_t addr, size_t align) {
  return align_down(addr + align - 1, align);
}

// Poor man's equivalent of std::hardware_destructive_interference_size.
#define AVOID_FALSE_SHARING 128
#define ALIGNED_TO_AVOID_FALSE_SHARING \
  __attribute__((aligned(AVOID_FALSE_SHARING)))

#endif // GC_ALIGN_H
