#ifndef GC_ALLOCATE_H_
#define GC_ALLOCATE_H_

#include "gc-api.h"

struct gc_heap;
struct gc_mutator;

static inline void gc_update_alloc_table(struct gc_ref obj, size_t size,
                                         enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

GC_API_ void* gc_allocate_slow(struct gc_mutator *mut, size_t bytes,
                               enum gc_allocation_kind kind) GC_NEVER_INLINE;

static inline void*
gc_allocate_small_fast_bump_pointer(struct gc_mutator *mut, size_t size,
                                    enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline void* gc_allocate_small_fast_freelist(struct gc_mutator *mut,
                                                    size_t size,
                                                    enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline void* gc_allocate_small_fast(struct gc_mutator *mut, size_t size,
                                           enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline void* gc_allocate_fast(struct gc_mutator *mut, size_t size,
                                     enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline void* gc_allocate(struct gc_mutator *mut, size_t size,
                                enum gc_allocation_kind kind) GC_ALWAYS_INLINE;

static inline void gc_update_alloc_table(struct gc_ref obj, size_t size,
                                         enum gc_allocation_kind kind) {
  size_t alignment = gc_allocator_alloc_table_alignment();
  if (!alignment) return;

  uintptr_t addr = gc_ref_value(obj);
  uintptr_t base = addr & ~(alignment - 1);
  size_t granule_size = gc_allocator_small_granule_size();
  uintptr_t granule = (addr & (alignment - 1)) / granule_size;
  uint8_t *alloc = (uint8_t*)(base + granule);

  uint8_t begin_pattern = gc_allocator_alloc_table_begin_pattern(kind);
  uint8_t end_pattern = gc_allocator_alloc_table_end_pattern();
  if (end_pattern) {
    size_t granules = size / granule_size;
    if (granules == 1) {
      alloc[0] = begin_pattern | end_pattern;
    } else {
      alloc[0] = begin_pattern;
      alloc[granules - 1] = end_pattern;
    }
  } else {
    alloc[0] = begin_pattern;
  }
}

static inline void* gc_allocate_small_fast_bump_pointer(struct gc_mutator *mut,
                                                        size_t size,
                                                        enum gc_allocation_kind kind) {
  GC_ASSERT(size <= gc_allocator_large_threshold());

  size_t granule_size = gc_allocator_small_granule_size();
  size_t hp_offset = gc_allocator_allocation_pointer_offset();
  size_t limit_offset = gc_allocator_allocation_limit_offset();

  uintptr_t base_addr = (uintptr_t)mut;
  uintptr_t *hp_loc = (uintptr_t*)(base_addr + hp_offset);
  uintptr_t *limit_loc = (uintptr_t*)(base_addr + limit_offset);

  size = (size + granule_size - 1) & ~(granule_size - 1);
  uintptr_t hp = *hp_loc;
  uintptr_t limit = *limit_loc;
  uintptr_t new_hp = hp + size;

  if (GC_UNLIKELY (new_hp > limit))
    return NULL;

  *hp_loc = new_hp;

  gc_update_alloc_table(gc_ref(hp), size, kind);

  return (void*)hp;
}

static inline void* gc_allocate_small_fast_freelist(struct gc_mutator *mut,
                                                    size_t size,
                                                    enum gc_allocation_kind kind) {
  GC_ASSERT(size <= gc_allocator_large_threshold());

  size_t freelist_offset = gc_allocator_freelist_offset(size, kind);
  uintptr_t base_addr = (uintptr_t)mut;
  void **freelist_loc = (void**)(base_addr + freelist_offset);

  void *head = *freelist_loc;
  if (GC_UNLIKELY(!head))
    return NULL;

  *freelist_loc = *(void**)head;
  *(void**)head = NULL;

  gc_update_alloc_table(gc_ref_from_heap_object(head), size, kind);

  return head;
}

static inline void* gc_allocate_small_fast(struct gc_mutator *mut, size_t size,
                                           enum gc_allocation_kind kind) {
  GC_ASSERT(size != 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());

  switch (gc_inline_allocator_kind(kind)) {
  case GC_INLINE_ALLOCATOR_BUMP_POINTER:
    return gc_allocate_small_fast_bump_pointer(mut, size, kind);
  case GC_INLINE_ALLOCATOR_FREELIST:
    return gc_allocate_small_fast_freelist(mut, size, kind);
  case GC_INLINE_ALLOCATOR_NONE:
    return NULL;
  default:
    GC_CRASH();
  }
}

static inline void* gc_allocate_fast(struct gc_mutator *mut, size_t size,
                                     enum gc_allocation_kind kind) {
  GC_ASSERT(size != 0);
  if (size > gc_allocator_large_threshold())
    return NULL;

  return gc_allocate_small_fast(mut, size, kind);
}

static inline void* gc_allocate(struct gc_mutator *mut, size_t size,
                                          enum gc_allocation_kind kind) {
  void *ret = gc_allocate_fast(mut, size, kind);
  if (GC_LIKELY(ret != NULL))
    return ret;

  return gc_allocate_slow(mut, size, kind);
}

#endif // GC_ALLOCATE_H_
