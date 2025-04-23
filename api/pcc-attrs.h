#ifndef PCC_ATTRS_H
#define PCC_ATTRS_H

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-attrs.h"

static const uintptr_t GC_ALIGNMENT = 8;
static const size_t GC_LARGE_OBJECT_THRESHOLD = 8192;

static inline enum gc_inline_allocator_kind gc_inline_allocator_kind(enum gc_allocation_kind kind) {
  return GC_INLINE_ALLOCATOR_BUMP_POINTER;
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

static inline size_t gc_allocator_freelist_offset(size_t size, enum gc_allocation_kind kind) {
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

static inline enum gc_old_generation_check_kind gc_old_generation_check_kind(size_t size) {
  if (!GC_GENERATIONAL)
    return GC_OLD_GENERATION_CHECK_NONE;
  if (size <= gc_allocator_large_threshold())
    return GC_OLD_GENERATION_CHECK_SMALL_OBJECT_NURSERY;
  return GC_OLD_GENERATION_CHECK_SLOW;
}
static inline uint8_t gc_old_generation_check_alloc_table_tag_mask(void) {
  GC_CRASH();
}
static inline uint8_t gc_old_generation_check_alloc_table_young_tag(void) {
  GC_CRASH();
}

static inline enum gc_write_barrier_kind gc_write_barrier_kind(size_t obj_size) {
  if (!GC_GENERATIONAL)
    return GC_WRITE_BARRIER_NONE;
  if (obj_size <= gc_allocator_large_threshold())
    return GC_WRITE_BARRIER_FIELD;
  return GC_WRITE_BARRIER_SLOW;
}
static inline size_t gc_write_barrier_field_table_alignment(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 64 * 1024 * 1024;
}
static inline ptrdiff_t gc_write_barrier_field_table_offset(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 128 * 1024;
}
static inline size_t gc_write_barrier_field_fields_per_byte(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 8;
}
static inline uint8_t gc_write_barrier_field_first_bit_pattern(void) {
  GC_ASSERT(GC_GENERATIONAL);
  return 1;
}

static inline enum gc_safepoint_mechanism gc_safepoint_mechanism(void) {
  return GC_SAFEPOINT_MECHANISM_COOPERATIVE;
}

static inline enum gc_cooperative_safepoint_kind gc_cooperative_safepoint_kind(void) {
  return GC_COOPERATIVE_SAFEPOINT_HEAP_FLAG;
}

static inline int gc_can_pin_objects(void) {
  return 0;
}

#endif // PCC_ATTRS_H
