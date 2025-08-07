#ifndef GROWABLE_HEAP_SIZER_H
#define GROWABLE_HEAP_SIZER_H

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "heap-sizer.h"

// This is a simple heap-sizing algorithm that will grow the heap if it is
// smaller than a multiplier of the live data size, where the multiplier depends
// on the square root of the live data size.  It does not shrink the heap.

struct gc_growable_heap_sizer {
  struct gc_heap *heap;
  double sqrt_multiplier;
  pthread_mutex_t lock;
};

static void
gc_growable_heap_sizer_set_double_threshold(struct gc_growable_heap_sizer *sizer,
                                            size_t threshold) {
  pthread_mutex_lock(&sizer->lock);
  GC_ASSERT(threshold);
  sizer->sqrt_multiplier = sqrt(threshold / 2);
  pthread_mutex_unlock(&sizer->lock);
}

static size_t
gc_growable_heap_sizer_target_size(struct gc_growable_heap_sizer *sizer,
                                   size_t heap_size, size_t live_bytes) {
  size_t target_size = live_bytes;
  if (live_bytes)
    target_size += sqrt(live_bytes) * sizer->sqrt_multiplier;
  return target_size;
}

static void
gc_growable_heap_sizer_on_gc(struct gc_growable_heap_sizer *sizer,
                             size_t heap_size, size_t live_bytes,
                             uint64_t pause_ns,
                             void (*set_heap_size)(struct gc_heap*, size_t)) {
  pthread_mutex_lock(&sizer->lock);
  size_t target_size =
    gc_growable_heap_sizer_target_size(sizer, heap_size, live_bytes);
  if (target_size > heap_size)
    set_heap_size(sizer->heap, target_size);
  pthread_mutex_unlock(&sizer->lock);
}

static struct gc_growable_heap_sizer*
gc_make_growable_heap_sizer(struct gc_heap *heap, size_t threshold) {
  struct gc_growable_heap_sizer *sizer;
  sizer = malloc(sizeof(*sizer));
  if (!sizer)
    GC_CRASH();
  memset(sizer, 0, sizeof(*sizer));
  sizer->heap = heap;
  pthread_mutex_init(&sizer->lock, NULL);
  gc_growable_heap_sizer_set_double_threshold(sizer, threshold);
  return sizer;
}

static void
gc_destroy_growable_heap_sizer(struct gc_growable_heap_sizer *sizer) {
  free(sizer);
}

#endif // GROWABLE_HEAP_SIZER_H
