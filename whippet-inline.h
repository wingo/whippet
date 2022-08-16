#ifndef WHIPPET_INLINE_H
#define WHIPPET_INLINE_H

#include "gc-config.h"
#include "gc-api.h"

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
  abort();
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

static inline enum gc_write_barrier_kind gc_small_write_barrier_kind(void) {
  if (GC_GENERATIONAL)
    return GC_WRITE_BARRIER_CARD;
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_small_write_barrier_card_table_alignment(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 4 * 1024 * 1024;
}
static inline size_t gc_small_write_barrier_card_size(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 256;
}

#endif // WHIPPET_INLINE_H
