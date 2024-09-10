#ifndef GC_EVENT_LISTENER_CHAIN_H
#define GC_EVENT_LISTENER_CHAIN_H

#include "gc-event-listener.h"

struct gc_event_listener_chain {
  struct gc_event_listener head; void *head_data;
  struct gc_event_listener tail; void *tail_data;
};

struct gc_event_listener_chain_mutator {
  struct gc_event_listener_chain *chain;
  void *head_mutator_data;
  void *tail_mutator_data;
};

static inline void gc_event_listener_chain_init(void *data, size_t heap_size) {
  struct gc_event_listener_chain *chain = data;
  chain->head.init(chain->head_data, heap_size);
  chain->tail.init(chain->tail_data, heap_size);
}

static inline void gc_event_listener_chain_requesting_stop(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.requesting_stop(chain->head_data);
  chain->tail.requesting_stop(chain->tail_data);
}
static inline void gc_event_listener_chain_waiting_for_stop(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.waiting_for_stop(chain->head_data);
  chain->tail.waiting_for_stop(chain->tail_data);
}
static inline void gc_event_listener_chain_mutators_stopped(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.mutators_stopped(chain->head_data);
  chain->tail.mutators_stopped(chain->tail_data);
}
static inline void
gc_event_listener_chain_prepare_gc(void *data, enum gc_collection_kind kind) {
  struct gc_event_listener_chain *chain = data;
  chain->head.prepare_gc(chain->head_data, kind);
  chain->tail.prepare_gc(chain->tail_data, kind);
}
static inline void gc_event_listener_chain_roots_traced(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.roots_traced(chain->head_data);
  chain->tail.roots_traced(chain->tail_data);
}
static inline void gc_event_listener_chain_heap_traced(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.heap_traced(chain->head_data);
  chain->tail.heap_traced(chain->tail_data);
}
static inline void gc_event_listener_chain_ephemerons_traced(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.ephemerons_traced(chain->head_data);
  chain->tail.ephemerons_traced(chain->tail_data);
}
static inline void gc_event_listener_chain_finalizers_traced(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.finalizers_traced(chain->head_data);
  chain->tail.finalizers_traced(chain->tail_data);
}

static inline void gc_event_listener_chain_restarting_mutators(void *data) {
  struct gc_event_listener_chain *chain = data;
  chain->head.restarting_mutators(chain->head_data);
  chain->tail.restarting_mutators(chain->tail_data);
}

static inline void* gc_event_listener_chain_mutator_added(void *data) {
  struct gc_event_listener_chain *chain = data;
  struct gc_event_listener_chain_mutator *mutator = malloc(sizeof(*mutator));;
  if (!mutator) abort();
  mutator->chain = chain;
  mutator->head_mutator_data = chain->head.mutator_added(chain->head_data);
  mutator->tail_mutator_data = chain->tail.mutator_added(chain->tail_data);
  return mutator;
}

static inline void gc_event_listener_chain_mutator_cause_gc(void *mutator_data) {
  struct gc_event_listener_chain_mutator *mutator = mutator_data;
  mutator->chain->head.restarting_mutators(mutator->head_data);
  mutator->chain->tail.restarting_mutators(mutator->tail_data);
}
static inline void gc_event_listener_chain_mutator_stopping(void *mutator_data) {
  struct gc_event_listener_chain_mutator *mutator = mutator_data;
  mutator->chain->head.mutator_stopping(mutator->head_data);
  mutator->chain->tail.mutator_stopping(mutator->tail_data);
}
static inline void gc_event_listener_chain_mutator_stopped(void *mutator_data) {
  struct gc_event_listener_chain_mutator *mutator = mutator_data;
  mutator->chain->head.mutator_stopped(mutator->head_data);
  mutator->chain->tail.mutator_stopped(mutator->tail_data);
}
static inline void gc_event_listener_chain_mutator_restarted(void *mutator_data) {
  struct gc_event_listener_chain_mutator *mutator = mutator_data;
  mutator->chain->head.mutator_restarted(mutator->head_data);
  mutator->chain->tail.mutator_restarted(mutator->tail_data);
}
static inline void gc_event_listener_chain_mutator_removed(void *mutator_data) {
  struct gc_event_listener_chain_mutator *mutator = mutator_data;
  mutator->chain->head.mutator_removed(mutator->head_data);
  mutator->chain->tail.mutator_removed(mutator->tail_data);
  free(mutator);
}

static inline void gc_event_listener_chain_heap_resized(void *data, size_t size) {
  struct gc_event_listener_chain *chain = data;
  chain->head.heap_resized(chain->head_data, size);
  chain->tail.heap_resized(chain->tail_data, size);
}

static inline void gc_event_listener_chain_live_data_size(void *data, size_t size) {
  struct gc_event_listener_chain *chain = data;
  chain->head.live_data_size(chain->head_data, size);
  chain->tail.live_data_size(chain->tail_data, size);
}

#define GC_EVENT_LISTENER_CHAIN                                         \
  ((struct gc_event_listener) {                                         \
    gc_event_listener_chain_init,                                       \
    gc_event_listener_chain_requesting_stop,                            \
    gc_event_listener_chain_waiting_for_stop,                           \
    gc_event_listener_chain_mutators_stopped,                           \
    gc_event_listener_chain_prepare_gc,                                 \
    gc_event_listener_chain_roots_traced,                               \
    gc_event_listener_chain_heap_traced,                                \
    gc_event_listener_chain_ephemerons_traced,                          \
    gc_event_listener_chain_finalizers_traced,                          \
    gc_event_listener_chain_restarting_mutators,                        \
    gc_event_listener_chain_mutator_added,                              \
    gc_event_listener_chain_mutator_cause_gc,                           \
    gc_event_listener_chain_mutator_stopping,                           \
    gc_event_listener_chain_mutator_stopped,                            \
    gc_event_listener_chain_mutator_restarted,                          \
    gc_event_listener_chain_mutator_removed,                            \
    gc_event_listener_chain_heap_resized,                               \
    gc_event_listener_chain_live_data_size,                             \
  })

#define GC_EVENT_LISTENER_CHAIN_DATA(head, head_data, tail, tail_data)  \
  ((struct gc_event_listener_chain_data){head, head_data, tail, tail_data})

#endif // GC_EVENT_LISTENER_CHAIN_H
