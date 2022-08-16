#ifndef SEMI_ATTRS_H
#define SEMI_ATTRS_H

#include "gc-attrs.h"
#include "gc-assert.h"

static const uintptr_t GC_ALIGNMENT = 8;
static const size_t GC_LARGE_OBJECT_THRESHOLD = 8192;

static inline enum gc_allocator_kind gc_allocator_kind(void) {
  return GC_ALLOCATOR_INLINE_BUMP_POINTER;
}
static inline size_t gc_allocator_small_granule_size(void) {
  return GC_ALIGNMENT;
}
static inline size_t gc_allocator_large_threshold(void) {
  return GC_LARGE_OBJECT_THRESHOLD;
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

static inline int gc_allocator_needs_clear(void) {
  return 1;
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

static inline enum gc_write_barrier_kind gc_small_write_barrier_kind(void) {
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_small_write_barrier_card_table_alignment(void) {
  GC_CRASH();
}
static inline size_t gc_small_write_barrier_card_size(void) {
  GC_CRASH();
}

#endif // SEMI_ATTRS_H
