#include <stdatomic.h>

#include "simple-tagging-scheme.h"
#include "gc-embedder-api.h"

static inline void gc_trace_object(void *object,
                                   void (*trace_edge)(struct gc_edge edge,
                                                      void *trace_data),
                                   void *trace_data,
                                   size_t *size) {
  switch (tag_live_alloc_kind(*tag_word(object))) {
#define SCAN_OBJECT(name, Name, NAME)                                   \
    case ALLOC_KIND_##NAME:                                             \
      if (trace_edge)                                                   \
        visit_##name##_fields((Name*)object, trace_edge, trace_data);   \
      if (size)                                                         \
        *size = name##_size(object);                                    \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    GC_CRASH();
  }
}

#if GC_PRECISE
#include "precise-roots-embedder.h"
#else
#include "conservative-roots-embedder.h"
#endif

static inline uintptr_t gc_object_forwarded_nonatomic(void *object) {
  uintptr_t tag = *tag_word(object);
  return (tag & gcobj_not_forwarded_bit) ? 0 : tag;
}

static inline void gc_object_forward_nonatomic(void *object,
                                               uintptr_t new_addr) {
  *tag_word(object) = new_addr;
}

static inline struct gc_atomic_forward
gc_atomic_forward_begin(void *object) {
  uintptr_t tag = atomic_load_explicit(tag_word(object), memory_order_acquire);
  enum gc_forwarding_state state;
  if (tag == gcobj_busy)
    state = GC_FORWARDING_STATE_BUSY;
  else if (tag & gcobj_not_forwarded_bit)
    state = GC_FORWARDING_STATE_NOT_FORWARDED;
  else
    state = GC_FORWARDING_STATE_FORWARDED;
  return (struct gc_atomic_forward){ object, tag, state };
}

static inline int
gc_atomic_forward_retry_busy(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_BUSY);
  uintptr_t tag = atomic_load_explicit(tag_word(fwd->object),
                                       memory_order_acquire);
  if (tag == gcobj_busy)
    return 0;
  if (tag & gcobj_not_forwarded_bit)
    fwd->state = GC_FORWARDING_STATE_ABORTED;
  else {
    fwd->state = GC_FORWARDING_STATE_FORWARDED;
    fwd->data = tag;
  }
  return 1;
}
  
static inline void
gc_atomic_forward_acquire(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_NOT_FORWARDED);
  if (atomic_compare_exchange_strong(tag_word(fwd->object), &fwd->data,
                                     gcobj_busy))
    fwd->state = GC_FORWARDING_STATE_ACQUIRED;
  else if (fwd->data == gcobj_busy)
    fwd->state = GC_FORWARDING_STATE_BUSY;
  else {
    GC_ASSERT((fwd->data & gcobj_not_forwarded_bit) == 0);
    fwd->state = GC_FORWARDING_STATE_FORWARDED;
  }
}

static inline void
gc_atomic_forward_abort(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_ACQUIRED);
  atomic_store_explicit(tag_word(fwd->object), fwd->data, memory_order_release);
  fwd->state = GC_FORWARDING_STATE_ABORTED;
}

static inline void
gc_atomic_forward_commit(struct gc_atomic_forward *fwd, uintptr_t new_addr) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_ACQUIRED);
  *tag_word((void*)new_addr) = fwd->data;
  atomic_store_explicit(tag_word(fwd->object), new_addr, memory_order_release);
  fwd->state = GC_FORWARDING_STATE_FORWARDED;
}

static inline uintptr_t
gc_atomic_forward_address(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_FORWARDED);
  return fwd->data;
}
