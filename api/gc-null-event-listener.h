#ifndef GC_NULL_EVENT_LISTENER_H
#define GC_NULL_EVENT_LISTENER_H

#include "gc-event-listener.h"

static inline void gc_null_event_listener_init(void *data, size_t size) {}
static inline void gc_null_event_listener_requesting_stop(void *data) {}
static inline void gc_null_event_listener_waiting_for_stop(void *data) {}
static inline void gc_null_event_listener_mutators_stopped(void *data) {}
static inline void gc_null_event_listener_prepare_gc(void *data,
                                                     enum gc_collection_kind) {}
static inline void gc_null_event_listener_roots_traced(void *data) {}
static inline void gc_null_event_listener_heap_traced(void *data) {}
static inline void gc_null_event_listener_ephemerons_traced(void *data) {}
static inline void gc_null_event_listener_finalizers_traced(void *data) {}
static inline void gc_null_event_listener_restarting_mutators(void *data) {}

static inline void* gc_null_event_listener_mutator_added(void *data) {}
static inline void gc_null_event_listener_mutator_cause_gc(void *mutator_data) {}
static inline void gc_null_event_listener_mutator_stopping(void *mutator_data) {}
static inline void gc_null_event_listener_mutator_stopped(void *mutator_data) {}
static inline void gc_null_event_listener_mutator_restarted(void *mutator_data) {}
static inline void gc_null_event_listener_mutator_removed(void *mutator_data) {}

static inline void gc_null_event_listener_heap_resized(void *, size_t) {}
static inline void gc_null_event_listener_live_data_size(void *, size_t) {}

#define GC_NULL_EVENT_LISTENER                                         \
  ((struct gc_event_listener) {                                        \
    gc_null_event_listener_init,                                       \
    gc_null_event_listener_requesting_stop,                            \
    gc_null_event_listener_waiting_for_stop,                           \
    gc_null_event_listener_mutators_stopped,                           \
    gc_null_event_listener_prepare_gc,                                 \
    gc_null_event_listener_roots_traced,                               \
    gc_null_event_listener_heap_traced,                                \
    gc_null_event_listener_ephemerons_traced,                          \
    gc_null_event_listener_finalizers_traced,                          \
    gc_null_event_listener_restarting_mutators,                        \
    gc_null_event_listener_mutator_added,                              \
    gc_null_event_listener_mutator_cause_gc,                           \
    gc_null_event_listener_mutator_stopping,                           \
    gc_null_event_listener_mutator_stopped,                            \
    gc_null_event_listener_mutator_restarted,                          \
    gc_null_event_listener_mutator_removed,                            \
    gc_null_event_listener_heap_resized,                               \
    gc_null_event_listener_live_data_size,                             \
  })

#endif // GC_NULL_EVENT_LISTENER_H_
