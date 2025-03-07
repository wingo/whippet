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

static inline size_t gc_allocator_freelist_offset(size_t size,
                                                  enum gc_allocation_kind kind) {
  GC_CRASH();
}

static inline size_t gc_allocator_alloc_table_alignment(void) {
  return 0;
}
static inline uint8_t gc_allocator_alloc_table_begin_pattern(enum gc_allocation_kind kind) {
  GC_CRASH();
}
static inline uint8_t gc_allocator_alloc_table_end_pattern(void) {
  GC_CRASH();
}

static inline enum gc_old_generation_check_kind gc_old_generation_check_kind(size_t) {
  return GC_OLD_GENERATION_CHECK_NONE;
}
static inline uint8_t gc_old_generation_check_alloc_table_tag_mask(void) {
  GC_CRASH();
}
static inline uint8_t gc_old_generation_check_alloc_table_young_tag(void) {
  GC_CRASH();
}

static inline enum gc_write_barrier_kind gc_write_barrier_kind(size_t) {
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_write_barrier_field_table_alignment(void) {
  GC_CRASH();
}
static inline ptrdiff_t gc_write_barrier_field_table_offset(void) {
  GC_CRASH();
}
static inline size_t gc_write_barrier_field_fields_per_byte(void) {
  GC_CRASH();
}
static inline uint8_t gc_write_barrier_field_first_bit_pattern(void) {
  GC_CRASH();
}

static inline enum gc_safepoint_mechanism gc_safepoint_mechanism(void) {
  return GC_SAFEPOINT_MECHANISM_COOPERATIVE;
}

static inline enum gc_cooperative_safepoint_kind gc_cooperative_safepoint_kind(void) {
  return GC_COOPERATIVE_SAFEPOINT_NONE;
}

static inline int gc_can_pin_objects(void) {
  return 0;
}

#endif // SEMI_ATTRS_H
