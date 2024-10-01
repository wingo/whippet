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

static inline enum gc_write_barrier_kind gc_write_barrier_kind(size_t) {
  return GC_WRITE_BARRIER_NONE;
}
static inline size_t gc_write_barrier_card_table_alignment(void) {
  GC_CRASH();
}
static inline size_t gc_write_barrier_card_size(void) {
  GC_CRASH();
}
static inline size_t gc_write_barrier_field_table_alignment(void) {
  GC_CRASH();
}
static inline size_t gc_write_barrier_field_fields_per_byte(void) {
  GC_CRASH();
}
static inline uint8_t gc_write_barrier_field_first_bit_pattern(void) {
  GC_CRASH();
}

static inline enum gc_safepoint_mechanism gc_safepoint_mechanism(void) {
  return GC_SAFEPOINT_MECHANISM_SIGNAL;
}

static inline enum gc_cooperative_safepoint_kind gc_cooperative_safepoint_kind(void) {
  return GC_COOPERATIVE_SAFEPOINT_NONE;
}

static inline int gc_can_pin_objects(void) {
  return 1;
}

#endif // BDW_ATTRS_H
