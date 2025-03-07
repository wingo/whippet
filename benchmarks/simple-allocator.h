#ifndef SIMPLE_ALLOCATOR_H
#define SIMPLE_ALLOCATOR_H

#include "simple-tagging-scheme.h"
#include "gc-api.h"

static inline void*
gc_allocate_with_kind(struct gc_mutator *mut, enum alloc_kind kind, size_t bytes) {
  void *obj = gc_allocate(mut, bytes, GC_ALLOCATION_TAGGED);
  *tag_word(gc_ref_from_heap_object(obj)) = tag_live(kind);
  return obj;
}

static inline void*
gc_allocate_pointerless_with_kind(struct gc_mutator *mut, enum alloc_kind kind, size_t bytes) {
  void *obj = gc_allocate(mut, bytes, GC_ALLOCATION_TAGGED_POINTERLESS);
  *tag_word(gc_ref_from_heap_object(obj)) = tag_live(kind);
  return obj;
}

#endif // SIMPLE_ALLOCATOR_H
