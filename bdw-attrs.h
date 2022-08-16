#ifndef BDW_ATTRS_H
#define BDW_ATTRS_H

#include "gc-attrs.h"
#include "gc-assert.h"

static inline enum gc_allocator_kind gc_allocator_kind(void) {
  return GC_ALLOCATOR_INLINE_FREELIST;
}
static inline size_t gc_allocator_small_granule_size(void) {
  return 2 * sizeof(void *);
}
static inline size_t gc_allocator_large_threshold(void) {
  return 256;
}

static inline size_t gc_allocator_allocation_pointer_offset(void) {
  GC_CRASH();
}
static inline size_t gc_allocator_allocation_limit_offset(void) {
  GC_CRASH();
}

static inline size_t gc_allocator_freelist_offset(size_t size) {
  GC_ASSERT(size);
  return sizeof(void*) * ((size - 1) / gc_allocator_small_granule_size());
}

static inline size_t gc_allocator_alloc_table_alignment(void) {
  return 0;
}
static inline uint8_t gc_allocator_alloc_table_begin_pattern(void) {
  GC_CRASH();
}
static inline uint8_t gc_allocator_alloc_table_end_pattern(void) {
  GC_CRASH();
}

static inline int gc_allocator_needs_clear(void) {
  return 0;
}

static inline enum gc_write_barrier_kind gc_small_write_barrier_kind(void) {
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_small_write_barrier_card_table_alignment(void) {
  GC_CRASH();
}
static inline size_t gc_small_write_barrier_card_size(void) {
  GC_CRASH();
}

#endif // BDW_ATTRS_H
