#ifndef GC_API_H_
#define GC_API_H_

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-inline.h"
#include "gc-ref.h"
#include "gc-edge.h"

#include <stdint.h>

// FIXME: prefix with gc_
struct heap;
struct mutator;

enum {
  GC_OPTION_FIXED_HEAP_SIZE,
  GC_OPTION_PARALLELISM
};

struct gc_option {
  int option;
  double value;
};

struct gc_mutator {
  void *user_data;
};

// FIXME: Conflict with bdw-gc GC_API.  Switch prefix?
#ifndef GC_API_
#define GC_API_ static
#endif

GC_API_ int gc_option_from_string(const char *str);
GC_API_ int gc_init(int argc, struct gc_option argv[],
                    struct heap **heap, struct mutator **mutator);

GC_API_ struct mutator* gc_init_for_thread(uintptr_t *stack_base,
                                           struct heap *heap);
GC_API_ void gc_finish_for_thread(struct mutator *mut);
GC_API_ void* gc_call_without_gc(struct mutator *mut, void* (*f)(void*),
                                 void *data) GC_NEVER_INLINE;

GC_API_ void* gc_allocate_small(struct mutator *mut, size_t bytes) GC_NEVER_INLINE;
GC_API_ void* gc_allocate_large(struct mutator *mut, size_t bytes) GC_NEVER_INLINE;
static inline void* gc_allocate(struct mutator *mut, size_t bytes) GC_ALWAYS_INLINE;
// FIXME: remove :P
static inline void* gc_allocate_pointerless(struct mutator *mut, size_t bytes);

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

static inline size_t gc_allocator_freelist_offset(size_t size) GC_ALWAYS_INLINE;

static inline void gc_allocator_inline_success(struct mutator *mut,
                                               struct gc_ref obj,
                                               uintptr_t aligned_size);
static inline void gc_allocator_inline_failure(struct mutator *mut,
                                               uintptr_t aligned_size);

static inline void*
gc_allocate_bump_pointer(struct mutator *mut, size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_bump_pointer(struct mutator *mut, size_t size) {
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

  if (GC_UNLIKELY (new_hp > limit)) {
    gc_allocator_inline_failure(mut, size);
    return gc_allocate_small(mut, size);
  }

  gc_allocator_inline_success(mut, gc_ref(hp), size);

  *hp_loc = new_hp;
  return (void*)hp;
}

static inline void* gc_allocate_freelist(struct mutator *mut,
                                         size_t size) GC_ALWAYS_INLINE;
static inline void* gc_allocate_freelist(struct mutator *mut, size_t size) {
  GC_ASSERT(size <= gc_allocator_large_threshold());

  size_t freelist_offset = gc_allocator_freelist_offset(size);
  uintptr_t base_addr = (uintptr_t)mut;
  void **freelist_loc = (void**)(base_addr + freelist_offset);

  void *head = *freelist_loc;
  if (GC_UNLIKELY(!head))
    return gc_allocate_small(mut, size);

  *freelist_loc = *(void**)head;
  return head;
}

static inline void* gc_allocate(struct mutator *mut, size_t size) {
  GC_ASSERT(size != 0);
  if (size > gc_allocator_large_threshold())
    return gc_allocate_large(mut, size);

  switch (gc_allocator_kind()) {
  case GC_ALLOCATOR_INLINE_BUMP_POINTER:
    return gc_allocate_bump_pointer(mut, size);
  case GC_ALLOCATOR_INLINE_FREELIST:
    return gc_allocate_freelist(mut, size);
  case GC_ALLOCATOR_INLINE_NONE:
    return gc_allocate_small(mut, size);
  default:
    abort();
  }
}

#endif // GC_API_H_
