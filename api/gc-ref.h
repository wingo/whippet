#ifndef GC_REF_H
#define GC_REF_H

#include "gc-assert.h"
#include "gc-config.h"

#include <stdint.h>

struct gc_ref {
  uintptr_t value;
};

static inline struct gc_ref gc_ref(uintptr_t value) {
  return (struct gc_ref){value};
}
static inline uintptr_t gc_ref_value(struct gc_ref ref) {
  return ref.value;
}

static inline struct gc_ref gc_ref_null(void) {
  return gc_ref(0);
}
static inline int gc_ref_is_null(struct gc_ref ref) {
  return ref.value == 0;
}
static inline int gc_ref_is_immediate(struct gc_ref ref) {
  GC_ASSERT(!gc_ref_is_null(ref));
  return GC_HAS_IMMEDIATES && (ref.value & (sizeof(void*) - 1));
}
static inline struct gc_ref gc_ref_immediate(uintptr_t val) {
  GC_ASSERT(val & (sizeof(void*) - 1));
  GC_ASSERT(GC_HAS_IMMEDIATES);
  return gc_ref(val);
}
static inline int gc_ref_is_heap_object(struct gc_ref ref) {
  return !gc_ref_is_immediate(ref);
}
static inline struct gc_ref gc_ref_from_heap_object_or_null(void *obj) {
  return gc_ref((uintptr_t) obj);
}
static inline struct gc_ref gc_ref_from_heap_object(void *obj) {
  GC_ASSERT(obj);
  return gc_ref_from_heap_object_or_null(obj);
}
static inline void* gc_ref_heap_object(struct gc_ref ref) {
  GC_ASSERT(gc_ref_is_heap_object(ref));
  return (void *) gc_ref_value(ref);
}

#endif // GC_REF_H
