#ifndef GC_EVENT_LISTENER_H
#define GC_EVENT_LISTENER_H

struct gc_event_listener {
  void (*init)(void *data, size_t heap_size);
  void (*prepare_gc)(void *data, int is_minor, int is_compacting);
  void (*requesting_stop)(void *data);
  void (*waiting_for_stop)(void *data);
  void (*mutators_stopped)(void *data);
  void (*roots_traced)(void *data);
  void (*heap_traced)(void *data);
  void (*ephemerons_traced)(void *data);
  void (*restarting_mutators)(void *data);

  void* (*mutator_added)(void *data);
  void (*mutator_cause_gc)(void *mutator_data);
  void (*mutator_stopping)(void *mutator_data);
  void (*mutator_stopped)(void *mutator_data);
  void (*mutator_restarted)(void *mutator_data);
  void (*mutator_removed)(void *mutator_data);

  void (*heap_resized)(void *data, size_t size);
  void (*live_data_size)(void *data, size_t size);
};

#endif // GC_EVENT_LISTENER_H
