#ifndef SIMPLE_TAGGING_SCHEME_H
#define SIMPLE_TAGGING_SCHEME_H

#include <stdint.h>

struct gc_header {
  uintptr_t tag;
};

// Alloc kind is in bits 1-7, for live objects.
static const uintptr_t gcobj_alloc_kind_mask = 0x7f;
static const uintptr_t gcobj_alloc_kind_shift = 1;
static const uintptr_t gcobj_forwarded_mask = 0x1;
static const uintptr_t gcobj_not_forwarded_bit = 0x1;
static const uintptr_t gcobj_busy = 0;
static inline uint8_t tag_live_alloc_kind(uintptr_t tag) {
  return (tag >> gcobj_alloc_kind_shift) & gcobj_alloc_kind_mask;
}
static inline uintptr_t tag_live(uint8_t alloc_kind) {
  return ((uintptr_t)alloc_kind << gcobj_alloc_kind_shift)
    | gcobj_not_forwarded_bit;
}

static inline uintptr_t* tag_word(struct gc_ref ref) {
  struct gc_header *header = gc_ref_heap_object(ref);
  return &header->tag;
}

#endif // SIMPLE_TAGGING_SCHEME_H
