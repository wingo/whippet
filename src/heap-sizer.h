#ifndef HEAP_SIZER_H
#define HEAP_SIZER_H

#include "gc-api.h"

#include "gc-options-internal.h"
#include "growable-heap-sizer.h"
#include "adaptive-heap-sizer.h"

struct gc_heap_sizer {
  enum gc_heap_size_policy policy;
  union {
    struct gc_growable_heap_sizer* growable;
    struct gc_adaptive_heap_sizer* adaptive;
  };
};

static struct gc_heap_sizer
gc_make_heap_sizer(struct gc_heap *heap,
                   const struct gc_common_options *options,
                   uint64_t (*get_allocation_counter_from_thread)(struct gc_heap*),
                   void (*set_heap_size_from_thread)(struct gc_heap*, size_t),
                   struct gc_background_thread *thread) {
  struct gc_heap_sizer ret = { options->heap_size_policy, };
  switch (options->heap_size_policy) {
    case GC_HEAP_SIZE_FIXED:
      break;

    case GC_HEAP_SIZE_GROWABLE:
      ret.growable =
        gc_make_growable_heap_sizer(heap, options->heap_size_multiplier);
      break;

    case GC_HEAP_SIZE_ADAPTIVE:
      ret.adaptive =
        gc_make_adaptive_heap_sizer (heap, options->heap_expansiveness,
                                     get_allocation_counter_from_thread,
                                     set_heap_size_from_thread,
                                     thread);
      break;

    default:
      GC_CRASH();
  }
  return ret;
}

static void
gc_heap_sizer_on_gc(struct gc_heap_sizer sizer, size_t heap_size,
                    size_t live_bytes, size_t pause_ns,
                    void (*set_heap_size)(struct gc_heap*, size_t)) {
  switch (sizer.policy) {
    case GC_HEAP_SIZE_FIXED:
      break;

    case GC_HEAP_SIZE_GROWABLE:
      gc_growable_heap_sizer_on_gc(sizer.growable, heap_size, live_bytes,
                                   pause_ns, set_heap_size);
      break;

    case GC_HEAP_SIZE_ADAPTIVE:
      if (sizer.adaptive->background_task_id < 0)
        gc_adaptive_heap_sizer_background_task(sizer.adaptive);
      gc_adaptive_heap_sizer_on_gc(sizer.adaptive, live_bytes, pause_ns,
                                   set_heap_size);
      break;

    default:
      GC_CRASH();
  }
}
                    

#endif // HEAP_SIZER_H
