#ifndef GC_EMBEDDER_API_H
#define GC_EMBEDDER_API_H

#include "gc-edge.h"
#include "gc-forwarding.h"

#ifndef GC_EMBEDDER_API
#define GC_EMBEDDER_API static
#endif

GC_EMBEDDER_API inline void gc_trace_object(void *object,
                                            void (*trace_edge)(struct gc_edge edge,
                                                               void *trace_data),
                                            void *trace_data,
                                            size_t *size) GC_ALWAYS_INLINE;

GC_EMBEDDER_API inline uintptr_t gc_object_forwarded_nonatomic(void *object);
GC_EMBEDDER_API inline void gc_object_forward_nonatomic(void *object, uintptr_t new_addr);

GC_EMBEDDER_API inline struct gc_atomic_forward gc_atomic_forward_begin(void *obj);
GC_EMBEDDER_API inline void gc_atomic_forward_acquire(struct gc_atomic_forward *);
GC_EMBEDDER_API inline int gc_atomic_forward_retry_busy(struct gc_atomic_forward *);
GC_EMBEDDER_API inline void gc_atomic_forward_abort(struct gc_atomic_forward *);
GC_EMBEDDER_API inline void gc_atomic_forward_commit(struct gc_atomic_forward *,
                                                     uintptr_t new_addr);
GC_EMBEDDER_API inline uintptr_t gc_atomic_forward_address(struct gc_atomic_forward *);

#endif // GC_EMBEDDER_API_H
