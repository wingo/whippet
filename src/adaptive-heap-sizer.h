#ifndef ADAPTIVE_HEAP_SIZER_H
#define ADAPTIVE_HEAP_SIZER_H

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "assert.h"
#include "debug.h"
#include "heap-sizer.h"
#include "gc-platform.h"

// This is the MemBalancer algorithm from "Optimal Heap Limits for Reducing
// Browser Memory Use" by Marisa Kirisame, Pranav Shenoy, and Pavel Panchekha
// (https://arxiv.org/abs/2204.10455).
//
// This implementation differs slightly in that the constant "c" of the paper
// has been extracted outside the radical, and notionally reversed: it is a
// unitless "expansiveness" parameter whose domain is [0,+∞].  Also there are
// minimum and maximum heap size multipliers, and a minimum amount of free
// space.  The initial collection rate is an informed guess.  The initial
// allocation rate estimate is high, considering that allocation rates are often
// high on program startup.

struct gc_adaptive_heap_sizer {
  uint64_t (*get_allocation_counter)(void *callback_data);
  void (*set_heap_size)(size_t size, void *callback_data);
  void *callback_data;
  uint64_t smoothed_pause_time;
  uint64_t smoothed_live_bytes;
  uint64_t live_bytes;
  double smoothed_allocation_rate;
  double collection_smoothing_factor;
  double allocation_smoothing_factor;
  double minimum_multiplier;
  double maximum_multiplier;
  double minimum_free_space;
  double expansiveness;
  int stopping;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
};

// With lock
static uint64_t
gc_adaptive_heap_sizer_calculate_size(struct gc_adaptive_heap_sizer *sizer) {
  double allocation_rate = sizer->smoothed_allocation_rate;
  double collection_rate =
    (double)sizer->smoothed_pause_time / (double)sizer->smoothed_live_bytes;
  double radicand = sizer->live_bytes * allocation_rate / collection_rate;
  double multiplier = 1.0 + sizer->expansiveness * sqrt(radicand);
  if (isnan(multiplier) || multiplier < sizer->minimum_multiplier)
    multiplier = sizer->minimum_multiplier;
  else if (multiplier > sizer->maximum_multiplier)
    multiplier = sizer->maximum_multiplier;
  uint64_t size = sizer->live_bytes * multiplier;
  if (size - sizer->live_bytes < sizer->minimum_free_space)
    size = sizer->live_bytes + sizer->minimum_free_space;
  return size;
}

static uint64_t
gc_adaptive_heap_sizer_set_expansiveness(struct gc_adaptive_heap_sizer *sizer,
                                         double expansiveness) {
  pthread_mutex_lock(&sizer->lock);
  sizer->expansiveness = expansiveness;
  uint64_t heap_size = gc_adaptive_heap_sizer_calculate_size(sizer);
  pthread_mutex_unlock(&sizer->lock);
  return heap_size;
}

static void
gc_adaptive_heap_sizer_on_gc(struct gc_adaptive_heap_sizer *sizer,
                             size_t live_bytes, uint64_t pause_ns,
                             void (*set_heap_size)(size_t, void*),
                             void *data) {
  pthread_mutex_lock(&sizer->lock);
  sizer->live_bytes = live_bytes;
  sizer->smoothed_live_bytes *= 1.0 - sizer->collection_smoothing_factor;
  sizer->smoothed_live_bytes += sizer->collection_smoothing_factor * live_bytes;
  sizer->smoothed_pause_time *= 1.0 - sizer->collection_smoothing_factor;
  sizer->smoothed_pause_time += sizer->collection_smoothing_factor * pause_ns;
  set_heap_size(gc_adaptive_heap_sizer_calculate_size(sizer), data);
  pthread_mutex_unlock(&sizer->lock);
}

static void*
gc_adaptive_heap_sizer_thread(void *data) {
  struct gc_adaptive_heap_sizer *sizer = data;
  uint64_t last_bytes_allocated =
    sizer->get_allocation_counter(sizer->callback_data);
  uint64_t last_heartbeat = gc_platform_monotonic_nanoseconds();
  pthread_mutex_lock(&sizer->lock);
  while (!sizer->stopping) {
    {
      struct timespec ts;
      if (clock_gettime(CLOCK_REALTIME, &ts)) {
        perror("adaptive heap sizer thread: failed to get time!");
        break;
      }
      ts.tv_sec += 1;
      pthread_cond_timedwait(&sizer->cond, &sizer->lock, &ts);
    }
    uint64_t bytes_allocated =
      sizer->get_allocation_counter(sizer->callback_data);
    uint64_t heartbeat = gc_platform_monotonic_nanoseconds();
    double rate = (double) (bytes_allocated - last_bytes_allocated) /
      (double) (heartbeat - last_heartbeat);
    // Just smooth the rate, under the assumption that the denominator is almost
    // always 1.
    sizer->smoothed_allocation_rate *= 1.0 - sizer->allocation_smoothing_factor;
    sizer->smoothed_allocation_rate += rate * sizer->allocation_smoothing_factor;
    last_heartbeat = heartbeat;
    last_bytes_allocated = bytes_allocated;
    sizer->set_heap_size(gc_adaptive_heap_sizer_calculate_size(sizer),
                         sizer->callback_data);
  }
  pthread_mutex_unlock(&sizer->lock);
  return NULL;
}

static struct gc_adaptive_heap_sizer*
gc_make_adaptive_heap_sizer(double expansiveness,
                            uint64_t (*get_allocation_counter)(void *),
                            void (*set_heap_size)(size_t , void *),
                            void *callback_data) {
  struct gc_adaptive_heap_sizer *sizer;
  sizer = malloc(sizeof(*sizer));
  if (!sizer)
    GC_CRASH();
  memset(sizer, 0, sizeof(*sizer));
  sizer->get_allocation_counter = get_allocation_counter;
  sizer->set_heap_size = set_heap_size;
  sizer->callback_data = callback_data;
  // Baseline estimate of GC speed: 10 MB/ms, or 10 bytes/ns.  However since we
  // observe this speed by separately noisy measurements, we have to provide
  // defaults for numerator and denominator; estimate 2ms for initial GC pauses
  // for 20 MB of live data during program startup.
  sizer->smoothed_pause_time = 2 * 1000 * 1000;
  sizer->smoothed_live_bytes = 20 * 1024 * 1024;
  // Baseline estimate of allocation rate during startup: 50 MB in 10ms, or 5
  // bytes/ns.
  sizer->smoothed_allocation_rate = 5;
  sizer->collection_smoothing_factor = 0.5;
  sizer->allocation_smoothing_factor = 0.95;
  sizer->minimum_multiplier = 1.1;
  sizer->maximum_multiplier = 5;
  sizer->minimum_free_space = 4 * 1024 * 1024;
  sizer->expansiveness = expansiveness;
  pthread_mutex_init(&sizer->lock, NULL);
  pthread_cond_init(&sizer->cond, NULL);
  if (pthread_create(&sizer->thread, NULL, gc_adaptive_heap_sizer_thread,
                     sizer)) {
    perror("spawning adaptive heap size thread failed");
    GC_CRASH();
  }
  return sizer;
}

static void
gc_destroy_adaptive_heap_sizer(struct gc_adaptive_heap_sizer *sizer) {
  pthread_mutex_lock(&sizer->lock);
  GC_ASSERT(!sizer->stopping);
  sizer->stopping = 1;
  pthread_mutex_unlock(&sizer->lock);
  pthread_cond_signal(&sizer->cond);
  pthread_join(sizer->thread, NULL);
  free(sizer);
}

#endif // ADAPTIVE_HEAP_SIZER_H
