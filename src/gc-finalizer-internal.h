#ifndef GC_FINALIZER_INTERNAL_H
#define GC_FINALIZER_INTERNAL_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "gc-finalizer.h"
#include "root.h"

struct gc_finalizer_state;

GC_INTERNAL
struct gc_finalizer_state* gc_make_finalizer_state(void);

GC_INTERNAL
void gc_finalizer_init_internal(struct gc_finalizer *f,
                                struct gc_ref object,
                                struct gc_ref closure);

GC_INTERNAL
void gc_finalizer_attach_internal(struct gc_finalizer_state *state,
                                  struct gc_finalizer *f,
                                  unsigned priority);

GC_INTERNAL
void gc_finalizer_externally_activated(struct gc_finalizer *f);

GC_INTERNAL
void gc_finalizer_externally_fired(struct gc_finalizer_state *state,
                                   struct gc_finalizer *finalizer);

GC_INTERNAL
struct gc_finalizer* gc_finalizer_state_pop(struct gc_finalizer_state *state);

GC_INTERNAL
void gc_finalizer_fire(struct gc_finalizer **fired_list_loc,
                       struct gc_finalizer *finalizer);

GC_INTERNAL
void gc_finalizer_state_set_callback(struct gc_finalizer_state *state,
                                     gc_finalizer_callback callback);

GC_INTERNAL
size_t gc_visit_finalizer_roots(struct gc_finalizer_state *state,
                                void (*visit)(struct gc_edge edge,
                                              struct gc_heap *heap,
                                              void *visit_data),
                                struct gc_heap *heap,
                                void *visit_data);

GC_INTERNAL
size_t gc_resolve_finalizers(struct gc_finalizer_state *state,
                             size_t priority,
                             void (*visit)(struct gc_edge edge,
                                           struct gc_heap *heap,
                                           void *visit_data),
                             struct gc_heap *heap,
                             void *visit_data);

GC_INTERNAL
void gc_notify_finalizers(struct gc_finalizer_state *state,
                          struct gc_heap *heap);

#endif // GC_FINALIZER_INTERNAL_H
