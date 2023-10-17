#ifndef GC_API_H_
#define GC_API_H_

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-attrs.h"
#include "gc-edge.h"
#include "gc-event-listener.h"
#include "gc-inline.h"
#include "gc-options.h"
#include "gc-ref.h"
#include "gc-visibility.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

struct gc_heap;
struct gc_mutator;

struct gc_stack_addr;
GC_API_ void* gc_call_with_stack_addr(void* (*f)(struct gc_stack_addr *,
                                                 void *),
                                      void *data) GC_NEVER_INLINE;

GC_API_ int gc_init(const struct gc_options *options,
                    struct gc_stack_addr *base, struct gc_heap **heap,
                    struct gc_mutator **mutator,
                    struct gc_event_listener event_listener,
                    void *event_listener_data);

struct gc_mutator_roots;
GC_API_ void gc_mutator_set_roots(struct gc_mutator *mut,
                                  struct gc_mutator_roots *roots);

struct gc_heap_roots;
GC_API_ void gc_heap_set_roots(struct gc_heap *heap,
                               struct gc_heap_roots *roots);

struct gc_extern_space;
GC_API_ void gc_heap_set_extern_space(struct gc_heap *heap,
                                      struct gc_extern_space *space);

GC_API_ struct gc_mutator* gc_init_for_thread(struct gc_stack_addr *base,
                                              struct gc_heap *heap);
GC_API_ void gc_finish_for_thread(struct gc_mutator *mut);
GC_API_ void* gc_call_without_gc(struct gc_mutator *mut, void* (*f)(void*),
                                 void *data) GC_NEVER_INLINE;

GC_API_ void gc_collect(struct gc_mutator *mut);

static inline void gc_clear_fresh_allocation(struct gc_ref obj,
                                             size_t size) GC_ALWAYS_INLINE;
static inline void gc_clear_fresh_allocation(struct gc_ref obj,
                                             size_t size) {
  if (!gc_allocator_needs_clear()) return;
  memset(gc_ref_heap_object(obj), 0, size);
}

static inline void gc_update_alloc_table(struct gc_mutator *mut,
                                         struct gc_ref obj,
                                         size_t size) GC_ALWAYS_INLINE;
static inline void gc_update_alloc_table(struct gc_mutator *mut,
                                         struct gc_ref obj,
                                         size_t size) {
  size_t alignment = gc_allocator_alloc_table_alignment();
  if (!alignment) return;

  uintptr_t addr = gc_ref_value(obj);
  uintptr_t base = addr & ~(alignment - 1);
  size_t granule_size = gc_allocator_small_granule_size();
  uintptr_t granule = (addr & (alignment - 1)) / granule_size;
  uint8_t *alloc = (uint8_t*)(base + granule);

  uint8_t begin_pattern = gc_allocator_alloc_table_begin_pattern();
  uint8_t end_pattern = gc_allocator_alloc_table_end_pattern();
  if (end_pattern) {
    size_t granules = size / granule_size;
    if (granules == 1) {
      alloc[0] = begin_pattern | end_pattern;
    } else {
      alloc[0] = begin_pattern;
      if (granules > 2)
        memset(alloc + 1, 0, granules - 2);
      alloc[granules - 1] = end_pattern;
    }
  } else {
    alloc[0] = begin_pattern;
  }
}

GC_API_ void* gc_allocate_slow(struct gc_mutator *mut, size_t bytes) GC_NEVER_INLINE;

static inline void*
gc_allocate_small_fast_bump_pointer(struct gc_mutator *mut, size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_small_fast_bump_pointer(struct gc_mutator *mut, size_t size) {
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

  gc_clear_fresh_allocation(gc_ref(hp), size);
  gc_update_alloc_table(mut, gc_ref(hp), size);

  return (void*)hp;
}

static inline void* gc_allocate_small_fast_freelist(struct gc_mutator *mut,
                                                    size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_small_fast_freelist(struct gc_mutator *mut, size_t size) {
  GC_ASSERT(size <= gc_allocator_large_threshold());

  size_t freelist_offset = gc_allocator_freelist_offset(size);
  uintptr_t base_addr = (uintptr_t)mut;
  void **freelist_loc = (void**)(base_addr + freelist_offset);

  void *head = *freelist_loc;
  if (GC_UNLIKELY(!head))
    return NULL;

  *freelist_loc = *(void**)head;

  gc_clear_fresh_allocation(gc_ref_from_heap_object(head), size);
  gc_update_alloc_table(mut, gc_ref_from_heap_object(head), size);

  return head;
}

static inline void* gc_allocate_small_fast(struct gc_mutator *mut, size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_small_fast(struct gc_mutator *mut, size_t size) {
  GC_ASSERT(size != 0);
  GC_ASSERT(size <= gc_allocator_large_threshold());

  switch (gc_allocator_kind()) {
  case GC_ALLOCATOR_INLINE_BUMP_POINTER:
    return gc_allocate_small_fast_bump_pointer(mut, size);
  case GC_ALLOCATOR_INLINE_FREELIST:
    return gc_allocate_small_fast_freelist(mut, size);
  case GC_ALLOCATOR_INLINE_NONE:
    return NULL;
  default:
    GC_CRASH();
  }
}

static inline void* gc_allocate_fast(struct gc_mutator *mut, size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_fast(struct gc_mutator *mut, size_t size) {
  GC_ASSERT(size != 0);
  if (size > gc_allocator_large_threshold())
    return NULL;

  return gc_allocate_small_fast(mut, size);
}

static inline void* gc_allocate(struct gc_mutator *mut, size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate(struct gc_mutator *mut, size_t size) {
  void *ret = gc_allocate_fast(mut, size);
  if (GC_LIKELY(ret != NULL))
    return ret;

  return gc_allocate_slow(mut, size);
}

// FIXME: remove :P
GC_API_ void* gc_allocate_pointerless(struct gc_mutator *mut, size_t bytes);

GC_API_ void gc_write_barrier_extern(struct gc_ref obj, size_t obj_size,
                                     struct gc_edge edge, struct gc_ref new_val) GC_NEVER_INLINE;

static inline void gc_write_barrier(struct gc_ref obj, size_t obj_size,
                                    struct gc_edge edge, struct gc_ref new_val) GC_ALWAYS_INLINE;
static inline void gc_write_barrier(struct gc_ref obj, size_t obj_size,
                                    struct gc_edge edge, struct gc_ref new_val) {
  switch (gc_write_barrier_kind(obj_size)) {
  case GC_WRITE_BARRIER_NONE:
    return;
  case GC_WRITE_BARRIER_CARD: {
    size_t card_table_alignment = gc_write_barrier_card_table_alignment();
    size_t card_size = gc_write_barrier_card_size();
    uintptr_t addr = gc_ref_value(obj);
    uintptr_t base = addr & ~(card_table_alignment - 1);
    uintptr_t card = (addr & (card_table_alignment - 1)) / card_size;
    atomic_store_explicit((uint8_t*)(base + card), 1, memory_order_relaxed);
    return;
  }
  case GC_WRITE_BARRIER_EXTERN:
    gc_write_barrier_extern(obj, obj_size, edge, new_val);
    return;
  default:
    GC_CRASH();
  }
}

#endif // GC_API_H_
