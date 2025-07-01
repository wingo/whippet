#ifndef GC_BARRIER_H_
#define GC_BARRIER_H_

#include "gc-api.h"

GC_API_ int gc_object_is_old_generation_slow(struct gc_mutator *mut,
                                             struct gc_ref obj) GC_NEVER_INLINE;

static inline int gc_object_is_old_generation(struct gc_mutator *mut,
                                              struct gc_ref obj,
                                              size_t obj_size) GC_ALWAYS_INLINE;

GC_API_ void gc_write_barrier_slow(struct gc_mutator *mut, struct gc_ref obj,
                                   size_t obj_size, struct gc_edge edge,
                                   struct gc_ref new_val) GC_NEVER_INLINE;

static inline int gc_write_barrier_fast(struct gc_mutator *mut, struct gc_ref obj,
                                        size_t obj_size, struct gc_edge edge,
                                        struct gc_ref new_val) GC_ALWAYS_INLINE;

static inline void gc_write_barrier(struct gc_mutator *mut, struct gc_ref obj,
                                    size_t obj_size, struct gc_edge edge,
                                    struct gc_ref new_val) GC_ALWAYS_INLINE;

static inline int gc_object_is_old_generation(struct gc_mutator *mut,
                                              struct gc_ref obj,
                                              size_t obj_size) {
  switch (gc_old_generation_check_kind(obj_size)) {
  case GC_OLD_GENERATION_CHECK_ALLOC_TABLE: {
    size_t alignment = gc_allocator_alloc_table_alignment();
    GC_ASSERT(alignment);
    uintptr_t addr = gc_ref_value(obj);
    uintptr_t base = addr & ~(alignment - 1);
    size_t granule_size = gc_allocator_small_granule_size();
    uintptr_t granule = (addr & (alignment - 1)) / granule_size;
    uint8_t *byte_loc = (uint8_t*)(base + granule);
    uint8_t byte = atomic_load_explicit(byte_loc, memory_order_relaxed);
    uint8_t mask = gc_old_generation_check_alloc_table_tag_mask();
    uint8_t young = gc_old_generation_check_alloc_table_young_tag();
    return (byte & mask) != young;
  }
  case GC_OLD_GENERATION_CHECK_SMALL_OBJECT_NURSERY: {
    struct gc_heap *heap = gc_mutator_heap(mut);
    // Note that these addresses are fixed and that the embedder might
    // want to store them somewhere or inline them into the output of
    // JIT-generated code.  They may also be power-of-two aligned.
    uintptr_t low_addr = gc_small_object_nursery_low_address(heap);
    uintptr_t high_addr = gc_small_object_nursery_high_address(heap);
    uintptr_t size = high_addr - low_addr;
    uintptr_t addr = gc_ref_value(obj);
    return addr - low_addr >= size;
  }
  case GC_OLD_GENERATION_CHECK_SLOW:
    return gc_object_is_old_generation_slow(mut, obj);
  default:
    GC_CRASH();
  }
}

static inline int gc_write_barrier_fast(struct gc_mutator *mut, struct gc_ref obj,
                                        size_t obj_size, struct gc_edge edge,
                                        struct gc_ref new_val) {
  switch (gc_write_barrier_kind(obj_size)) {
  case GC_WRITE_BARRIER_NONE:
    return 0;
  case GC_WRITE_BARRIER_FIELD: {
    if (!gc_object_is_old_generation(mut, obj, obj_size))
      return 0;

    size_t field_table_alignment = gc_write_barrier_field_table_alignment();
    size_t fields_per_byte = gc_write_barrier_field_fields_per_byte();
    uint8_t first_bit_pattern = gc_write_barrier_field_first_bit_pattern();
    ssize_t table_offset = gc_write_barrier_field_table_offset();

    uintptr_t addr = gc_edge_address(edge);
    uintptr_t base = addr & ~(field_table_alignment - 1);
    uintptr_t field = (addr & (field_table_alignment - 1)) / sizeof(uintptr_t);
    uintptr_t log_byte = field / fields_per_byte;
    uint8_t log_bit = first_bit_pattern << (field % fields_per_byte);
    uint8_t *byte_loc = (uint8_t*)(base + table_offset + log_byte);
    uint8_t byte = atomic_load_explicit(byte_loc, memory_order_relaxed);
    return !(byte & log_bit);
  }
  case GC_WRITE_BARRIER_SLOW:
    return 1;
  default:
    GC_CRASH();
  }
}

static inline void gc_write_barrier(struct gc_mutator *mut, struct gc_ref obj,
                                    size_t obj_size, struct gc_edge edge,
                                    struct gc_ref new_val) {
  if (GC_UNLIKELY(gc_write_barrier_fast(mut, obj, obj_size, edge, new_val)))
    gc_write_barrier_slow(mut, obj, obj_size, edge, new_val);
}

#endif // GC_BARRIER_H_
