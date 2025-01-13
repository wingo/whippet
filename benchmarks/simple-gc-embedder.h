#include <stdatomic.h>

#include "simple-tagging-scheme.h"
#include "simple-roots-types.h"
#include "gc-config.h"
#include "gc-embedder-api.h"

#define GC_EMBEDDER_EPHEMERON_HEADER struct gc_header header;
#define GC_EMBEDDER_FINALIZER_HEADER struct gc_header header;

static inline size_t gc_finalizer_priority_count(void) { return 2; }

static inline int
gc_is_valid_conservative_ref_displacement(uintptr_t displacement) {
#if GC_CONSERVATIVE_ROOTS || GC_CONSERVATIVE_TRACE
  // Here is where you would allow tagged heap object references.
  return displacement == 0;
#else
  // Shouldn't get here.
  GC_CRASH();
#endif
}

// No external objects in simple benchmarks.
static inline int gc_extern_space_visit(struct gc_extern_space *space,
                                        struct gc_edge edge,
                                        struct gc_ref ref) {
  GC_CRASH();
}
static inline void gc_extern_space_start_gc(struct gc_extern_space *space,
                                            int is_minor_gc) {
}
static inline void gc_extern_space_finish_gc(struct gc_extern_space *space,
                                             int is_minor_gc) {
}

static inline void gc_trace_object(struct gc_ref ref,
                                   void (*trace_edge)(struct gc_edge edge,
                                                      struct gc_heap *heap,
                                                      void *trace_data),
                                   struct gc_heap *heap,
                                   void *trace_data,
                                   size_t *size) {
#if GC_CONSERVATIVE_TRACE
  // Shouldn't get here.
  GC_CRASH();
#else
  switch (tag_live_alloc_kind(*tag_word(ref))) {
#define SCAN_OBJECT(name, Name, NAME)                                   \
    case ALLOC_KIND_##NAME:                                             \
      if (trace_edge)                                                   \
        visit_##name##_fields(gc_ref_heap_object(ref), trace_edge,      \
                              heap, trace_data);                        \
      if (size)                                                         \
        *size = name##_size(gc_ref_heap_object(ref));                   \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    GC_CRASH();
  }
#endif
}

static inline void visit_roots(struct handle *roots,
                               void (*trace_edge)(struct gc_edge edge,
                                                  struct gc_heap *heap,
                                                  void *trace_data),
                               struct gc_heap *heap,
                               void *trace_data) {
  for (struct handle *h = roots; h; h = h->next)
    trace_edge(gc_edge(&h->v), heap, trace_data);
}

static inline void gc_trace_mutator_roots(struct gc_mutator_roots *roots,
                                          void (*trace_edge)(struct gc_edge edge,
                                                             struct gc_heap *heap,
                                                             void *trace_data),
                                          struct gc_heap *heap,
                                          void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, heap, trace_data);
}

static inline void gc_trace_heap_roots(struct gc_heap_roots *roots,
                                       void (*trace_edge)(struct gc_edge edge,
                                                          struct gc_heap *heap,
                                                          void *trace_data),
                                       struct gc_heap *heap,
                                       void *trace_data) {
  if (roots)
    visit_roots(roots->roots, trace_edge, heap, trace_data);
}

static inline uintptr_t gc_object_forwarded_nonatomic(struct gc_ref ref) {
  uintptr_t tag = *tag_word(ref);
  return (tag & gcobj_not_forwarded_bit) ? 0 : tag;
}

static inline void gc_object_forward_nonatomic(struct gc_ref ref,
                                               struct gc_ref new_ref) {
  *tag_word(ref) = gc_ref_value(new_ref);
}

static inline struct gc_atomic_forward
gc_atomic_forward_begin(struct gc_ref ref) {
  uintptr_t tag = atomic_load_explicit(tag_word(ref), memory_order_acquire);
  enum gc_forwarding_state state;
  if (tag == gcobj_busy)
    state = GC_FORWARDING_STATE_BUSY;
  else if (tag & gcobj_not_forwarded_bit)
    state = GC_FORWARDING_STATE_NOT_FORWARDED;
  else
    state = GC_FORWARDING_STATE_FORWARDED;
  return (struct gc_atomic_forward){ ref, tag, state };
}

static inline int
gc_atomic_forward_retry_busy(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_BUSY);
  uintptr_t tag = atomic_load_explicit(tag_word(fwd->ref),
                                       memory_order_acquire);
  if (tag == gcobj_busy)
    return 0;
  if (tag & gcobj_not_forwarded_bit) {
    fwd->state = GC_FORWARDING_STATE_NOT_FORWARDED;
    fwd->data = tag;
  } else {
    fwd->state = GC_FORWARDING_STATE_FORWARDED;
    fwd->data = tag;
  }
  return 1;
}
  
static inline void
gc_atomic_forward_acquire(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_NOT_FORWARDED);
  if (atomic_compare_exchange_strong(tag_word(fwd->ref), &fwd->data,
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
  atomic_store_explicit(tag_word(fwd->ref), fwd->data, memory_order_release);
  fwd->state = GC_FORWARDING_STATE_NOT_FORWARDED;
}

static inline size_t
gc_atomic_forward_object_size(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_ACQUIRED);
  switch (tag_live_alloc_kind(fwd->data)) {
#define OBJECT_SIZE(name, Name, NAME)                                   \
    case ALLOC_KIND_##NAME:                                             \
      return name##_size(gc_ref_heap_object(fwd->ref));
    FOR_EACH_HEAP_OBJECT_KIND(OBJECT_SIZE)
#undef OBJECT_SIZE
  default:
    GC_CRASH();
  }
}

static inline void
gc_atomic_forward_commit(struct gc_atomic_forward *fwd, struct gc_ref new_ref) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_ACQUIRED);
  *tag_word(new_ref) = fwd->data;
  atomic_store_explicit(tag_word(fwd->ref), gc_ref_value(new_ref),
                        memory_order_release);
  fwd->state = GC_FORWARDING_STATE_FORWARDED;
}

static inline uintptr_t
gc_atomic_forward_address(struct gc_atomic_forward *fwd) {
  GC_ASSERT(fwd->state == GC_FORWARDING_STATE_FORWARDED);
  return fwd->data;
}
