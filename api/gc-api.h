#ifndef GC_API_H_
#define GC_API_H_

#include "gc-config.h"
#include "gc-allocation-kind.h"
#include "gc-assert.h"
#include "gc-attrs.h"
#include "gc-collection-kind.h"
#include "gc-conservative-ref.h"
#include "gc-edge.h"
#include "gc-event-listener.h"
#include "gc-inline.h"
#include "gc-options.h"
#include "gc-ref.h"
#include "gc-stack-addr.h"
#include "gc-visibility.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

struct gc_heap;
struct gc_mutator;

GC_API_ int gc_init(const struct gc_options *options,
                    struct gc_stack_addr base, struct gc_heap **heap,
                    struct gc_mutator **mutator,
                    struct gc_event_listener event_listener,
                    void *event_listener_data);

GC_API_ uint64_t gc_allocation_counter(struct gc_heap *heap);

GC_API_ struct gc_heap* gc_mutator_heap(struct gc_mutator *mut);

GC_API_ uintptr_t gc_small_object_nursery_low_address(struct gc_heap *heap);
GC_API_ uintptr_t gc_small_object_nursery_high_address(struct gc_heap *heap);

struct gc_mutator_roots;
GC_API_ void gc_mutator_set_roots(struct gc_mutator *mut,
                                  struct gc_mutator_roots *roots);

struct gc_heap_roots;
GC_API_ void gc_heap_set_roots(struct gc_heap *heap,
                               struct gc_heap_roots *roots);

GC_API_ void gc_heap_set_allocation_failure_handler(struct gc_heap *heap,
                                                    void* (*)(struct gc_heap*,
                                                              size_t));

struct gc_extern_space;
GC_API_ void gc_heap_set_extern_space(struct gc_heap *heap,
                                      struct gc_extern_space *space);

GC_API_ struct gc_mutator* gc_init_for_thread(struct gc_stack_addr base,
                                              struct gc_heap *heap);
GC_API_ void gc_finish_for_thread(struct gc_mutator *mut);
GC_API_ void gc_deactivate(struct gc_mutator *mut);
GC_API_ void gc_reactivate(struct gc_mutator *mut);
GC_API_ void* gc_deactivate_for_call(struct gc_mutator *mut,
                                     void* (*f)(struct gc_mutator*, void*),
                                     void *data);
GC_API_ void* gc_reactivate_for_call(struct gc_mutator *mut,
                                     void* (*f)(struct gc_mutator*, void*),
                                     void *data);

GC_API_ void gc_collect(struct gc_mutator *mut,
                        enum gc_collection_kind requested_kind);

GC_API_ int gc_heap_contains(struct gc_heap *heap, struct gc_ref ref);

GC_API_ struct gc_ref gc_resolve_conservative_ref(struct gc_heap *heap,
                                                  struct gc_conservative_ref ref,
                                                  int possibly_interior);
GC_API_ void gc_pin_object(struct gc_mutator *mut, struct gc_ref obj);

#endif // GC_API_H_
