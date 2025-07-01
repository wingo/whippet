#ifndef GC_SAFEPOINT_H_
#define GC_SAFEPOINT_H_

#include "gc-api.h"

GC_API_ void gc_safepoint_slow(struct gc_mutator *mut) GC_NEVER_INLINE;
GC_API_ int* gc_safepoint_flag_loc(struct gc_mutator *mut);
static inline void gc_safepoint(struct gc_mutator *mut) GC_ALWAYS_INLINE;

GC_API_ int gc_safepoint_signal_number(void);
GC_API_ void gc_safepoint_signal_inhibit(struct gc_mutator *mut);
GC_API_ void gc_safepoint_signal_reallow(struct gc_mutator *mut);

static inline void gc_inhibit_preemption(struct gc_mutator *mut) GC_ALWAYS_INLINE;
static inline void gc_reallow_preemption(struct gc_mutator *mut) GC_ALWAYS_INLINE;

static inline int gc_should_stop_for_safepoint(struct gc_mutator *mut) {
  switch (gc_cooperative_safepoint_kind()) {
  case GC_COOPERATIVE_SAFEPOINT_NONE:
    return 0;
  case GC_COOPERATIVE_SAFEPOINT_MUTATOR_FLAG:
  case GC_COOPERATIVE_SAFEPOINT_HEAP_FLAG: {
    return atomic_load_explicit(gc_safepoint_flag_loc(mut),
                                memory_order_relaxed);
  }
  default:
    GC_CRASH();
  }
}

static inline void gc_safepoint(struct gc_mutator *mut) {
  if (GC_UNLIKELY(gc_should_stop_for_safepoint(mut)))
    gc_safepoint_slow(mut);
}

static inline void gc_inhibit_preemption(struct gc_mutator *mut) {
  if (gc_safepoint_mechanism() == GC_SAFEPOINT_MECHANISM_SIGNAL)
    gc_safepoint_signal_inhibit(mut);
}

static inline void gc_reallow_preemption(struct gc_mutator *mut) {
  if (gc_safepoint_mechanism() == GC_SAFEPOINT_MECHANISM_SIGNAL)
    gc_safepoint_signal_reallow(mut);
}

#endif // GC_SAFEPOINT_H_
