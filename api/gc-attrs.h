#ifndef GC_ATTRS_H
#define GC_ATTRS_H

#include "gc-inline.h"
#include "gc-allocation-kind.h"

#include <stddef.h>
#include <stdint.h>

enum gc_allocator_kind {
  GC_ALLOCATOR_INLINE_BUMP_POINTER,
  GC_ALLOCATOR_INLINE_FREELIST,
  GC_ALLOCATOR_INLINE_NONE
};

static inline enum gc_allocator_kind gc_allocator_kind(void) GC_ALWAYS_INLINE;
static inline size_t gc_allocator_large_threshold(void) GC_ALWAYS_INLINE;
static inline size_t gc_allocator_small_granule_size(void) GC_ALWAYS_INLINE;

static inline size_t gc_allocator_allocation_pointer_offset(void) GC_ALWAYS_INLINE;
static inline size_t gc_allocator_allocation_limit_offset(void) GC_ALWAYS_INLINE;

static inline size_t gc_allocator_freelist_offset(size_t size,
                                                  enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline size_t gc_allocator_alloc_table_alignment(void) GC_ALWAYS_INLINE;
static inline uint8_t gc_allocator_alloc_table_begin_pattern(enum gc_allocation_kind kind) GC_ALWAYS_INLINE;
static inline uint8_t gc_allocator_alloc_table_end_pattern(void) GC_ALWAYS_INLINE;

enum gc_old_generation_check_kind {
  GC_OLD_GENERATION_CHECK_NONE,
  GC_OLD_GENERATION_CHECK_ALLOC_TABLE,
  GC_OLD_GENERATION_CHECK_SMALL_OBJECT_NURSERY,
  GC_OLD_GENERATION_CHECK_SLOW
};

static inline enum gc_old_generation_check_kind gc_old_generation_check_kind(size_t obj_size) GC_ALWAYS_INLINE;

static inline uint8_t gc_old_generation_check_alloc_table_tag_mask(void) GC_ALWAYS_INLINE;
static inline uint8_t gc_old_generation_check_alloc_table_young_tag(void) GC_ALWAYS_INLINE;

enum gc_write_barrier_kind {
  GC_WRITE_BARRIER_NONE,
  GC_WRITE_BARRIER_FIELD,
  GC_WRITE_BARRIER_SLOW
};

static inline enum gc_write_barrier_kind gc_write_barrier_kind(size_t obj_size) GC_ALWAYS_INLINE;
static inline size_t gc_write_barrier_field_table_alignment(void) GC_ALWAYS_INLINE;
static inline ptrdiff_t gc_write_barrier_field_table_offset(void) GC_ALWAYS_INLINE;
static inline size_t gc_write_barrier_field_fields_per_byte(void) GC_ALWAYS_INLINE;
static inline uint8_t gc_write_barrier_field_first_bit_pattern(void) GC_ALWAYS_INLINE;

enum gc_safepoint_mechanism {
  GC_SAFEPOINT_MECHANISM_COOPERATIVE,
  GC_SAFEPOINT_MECHANISM_SIGNAL,
};
static inline enum gc_safepoint_mechanism gc_safepoint_mechanism(void) GC_ALWAYS_INLINE;

enum gc_cooperative_safepoint_kind {
  GC_COOPERATIVE_SAFEPOINT_NONE,
  GC_COOPERATIVE_SAFEPOINT_MUTATOR_FLAG,
  GC_COOPERATIVE_SAFEPOINT_HEAP_FLAG,
};
static inline enum gc_cooperative_safepoint_kind gc_cooperative_safepoint_kind(void) GC_ALWAYS_INLINE;

static inline int gc_can_pin_objects(void) GC_ALWAYS_INLINE;

#endif // GC_ATTRS_H
