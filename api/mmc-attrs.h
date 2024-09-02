#ifndef MMC_ATTRS_H
#define MMC_ATTRS_H

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-attrs.h"

static inline enum gc_allocator_kind gc_allocator_kind(void) {
  return GC_ALLOCATOR_INLINE_BUMP_POINTER;
}
static inline size_t gc_allocator_small_granule_size(void) {
  return 16;
}
static inline size_t gc_allocator_large_threshold(void) {
  return 8192;
}

static inline size_t gc_allocator_allocation_pointer_offset(void) {
  return sizeof(uintptr_t) * 0;
}
static inline size_t gc_allocator_allocation_limit_offset(void) {
  return sizeof(uintptr_t) * 1;
}

static inline size_t gc_allocator_freelist_offset(size_t size) {
  GC_CRASH();
}

static inline size_t gc_allocator_alloc_table_alignment(void) {
  return 4 * 1024 * 1024;
}
static inline uint8_t gc_allocator_alloc_table_begin_pattern(void) {
  return 1;
}
static inline uint8_t gc_allocator_alloc_table_end_pattern(void) {
  return 16;
}

static inline int gc_allocator_needs_clear(void) {
  return 0;
}

static inline enum gc_write_barrier_kind gc_write_barrier_kind(size_t obj_size) {
  if (GC_GENERATIONAL) {
    if (obj_size <= gc_allocator_large_threshold())
      return GC_WRITE_BARRIER_CARD;
    return GC_WRITE_BARRIER_EXTERN;
  }
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_write_barrier_card_table_alignment(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 4 * 1024 * 1024;
}
static inline size_t gc_write_barrier_card_size(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 256;
}

static inline enum gc_safepoint_mechanism gc_safepoint_mechanism(void) {
  return GC_SAFEPOINT_MECHANISM_COOPERATIVE;
}

#endif // MMC_ATTRS_H
