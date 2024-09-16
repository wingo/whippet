#ifndef GROWABLE_HEAP_SIZER_H
#define GROWABLE_HEAP_SIZER_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "heap-sizer.h"

// This is a simple heap-sizing algorithm that will grow the heap if it is
// smaller than a given multiplier of the live data size.  It does not shrink
// the heap.

struct gc_growable_heap_sizer {
  struct gc_heap *heap;
  double multiplier;
  pthread_mutex_t lock;
};

static void
gc_growable_heap_sizer_set_multiplier(struct gc_growable_heap_sizer *sizer,
                                      double multiplier) {
  pthread_mutex_lock(&sizer->lock);
  sizer->multiplier = multiplier;
  pthread_mutex_unlock(&sizer->lock);
}

static void
gc_growable_heap_sizer_on_gc(struct gc_growable_heap_sizer *sizer,
                             size_t heap_size, size_t live_bytes,
                             uint64_t pause_ns,
                             void (*set_heap_size)(struct gc_heap*, size_t)) {
  pthread_mutex_lock(&sizer->lock);
  size_t target_size = live_bytes * sizer->multiplier;
  if (target_size > heap_size)
    set_heap_size(sizer->heap, target_size);
  pthread_mutex_unlock(&sizer->lock);
}

static struct gc_growable_heap_sizer*
gc_make_growable_heap_sizer(struct gc_heap *heap, double multiplier) {
  struct gc_growable_heap_sizer *sizer;
  sizer = malloc(sizeof(*sizer));
  if (!sizer)
    GC_CRASH();
  memset(sizer, 0, sizeof(*sizer));
  sizer->heap = heap;
  sizer->multiplier = multiplier;
  pthread_mutex_init(&sizer->lock, NULL);
  return sizer;
}

static void
gc_destroy_growable_heap_sizer(struct gc_growable_heap_sizer *sizer) {
  free(sizer);
}

#endif // GROWABLE_HEAP_SIZER_H
