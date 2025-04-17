#ifndef GC_BASIC_STATS_H
#define GC_BASIC_STATS_H

#include "gc-event-listener.h"
#include "gc-histogram.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

GC_DEFINE_HISTOGRAM(gc_latency, 25, 4);

struct gc_basic_stats {
  uint64_t major_collection_count;
  uint64_t minor_collection_count;
  uint64_t last_time_usec;
  uint64_t last_cpu_time_usec;
  uint64_t elapsed_mutator_usec;
  uint64_t elapsed_collector_usec;
  uint64_t cpu_mutator_usec;
  uint64_t cpu_collector_usec;
  size_t heap_size;
  size_t max_heap_size;
  size_t live_data_size;
  size_t max_live_data_size;
  struct gc_latency pause_times;
};

static inline uint64_t gc_basic_stats_now(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) GC_CRASH();
  uint64_t ret = tv.tv_sec;
  ret *= 1000 * 1000;
  ret += tv.tv_usec;
  return ret;
}

static inline uint64_t gc_basic_stats_cpu_time(void) {
  struct timespec ts;
  clock_gettime (CLOCK_PROCESS_CPUTIME_ID, &ts);
  uint64_t ret = ts.tv_sec;
  ret *= 1000 * 1000;
  ret += ts.tv_nsec / 1000;
  return ret;
}

static inline void gc_basic_stats_init(void *data, size_t heap_size) {
  struct gc_basic_stats *stats = data;
  memset(stats, 0, sizeof(*stats));
  stats->last_time_usec = gc_basic_stats_now();
  stats->last_cpu_time_usec = gc_basic_stats_cpu_time();
  stats->heap_size = stats->max_heap_size = heap_size;
}

static inline void gc_basic_stats_requesting_stop(void *data) {
  struct gc_basic_stats *stats = data;
  uint64_t now = gc_basic_stats_now();
  uint64_t cpu_time = gc_basic_stats_cpu_time();
  stats->elapsed_mutator_usec += now - stats->last_time_usec;
  stats->cpu_mutator_usec += cpu_time - stats->last_cpu_time_usec;
  stats->last_time_usec = now;
  stats->last_cpu_time_usec = cpu_time;
}
static inline void gc_basic_stats_waiting_for_stop(void *data) {}
static inline void gc_basic_stats_mutators_stopped(void *data) {}

static inline void gc_basic_stats_prepare_gc(void *data,
                                             enum gc_collection_kind kind) {
  struct gc_basic_stats *stats = data;
  if (kind == GC_COLLECTION_MINOR)
    stats->minor_collection_count++;
  else
    stats->major_collection_count++;
}

static inline void gc_basic_stats_roots_traced(void *data) {}
static inline void gc_basic_stats_heap_traced(void *data) {}
static inline void gc_basic_stats_ephemerons_traced(void *data) {}
static inline void gc_basic_stats_finalizers_traced(void *data) {}

static inline void gc_basic_stats_restarting_mutators(void *data) {
  struct gc_basic_stats *stats = data;
  uint64_t now = gc_basic_stats_now();
  uint64_t cpu_time = gc_basic_stats_cpu_time();
  uint64_t pause_time = now - stats->last_time_usec;
  uint64_t pause_cpu_time = cpu_time - stats->last_cpu_time_usec;
  stats->elapsed_collector_usec += pause_time;
  stats->cpu_collector_usec += pause_cpu_time;
  gc_latency_record(&stats->pause_times, pause_time);
  stats->last_time_usec = now;
  stats->last_cpu_time_usec = cpu_time;
}

static inline void* gc_basic_stats_mutator_added(void *data) {
  return NULL;
}
static inline void gc_basic_stats_mutator_cause_gc(void *mutator_data) {}
static inline void gc_basic_stats_mutator_stopping(void *mutator_data) {}
static inline void gc_basic_stats_mutator_stopped(void *mutator_data) {}
static inline void gc_basic_stats_mutator_restarted(void *mutator_data) {}
static inline void gc_basic_stats_mutator_removed(void *mutator_data) {}

static inline void gc_basic_stats_heap_resized(void *data, size_t size) {
  struct gc_basic_stats *stats = data;
  stats->heap_size = size;
  if (size > stats->max_heap_size)
    stats->max_heap_size = size;
}

static inline void gc_basic_stats_live_data_size(void *data, size_t size) {
  struct gc_basic_stats *stats = data;
  stats->live_data_size = size;
  if (size > stats->max_live_data_size)
    stats->max_live_data_size = size;
}

#define GC_BASIC_STATS                                                  \
  ((struct gc_event_listener) {                                         \
    gc_basic_stats_init,                                                \
    gc_basic_stats_requesting_stop,                                     \
    gc_basic_stats_waiting_for_stop,                                    \
    gc_basic_stats_mutators_stopped,                                    \
    gc_basic_stats_prepare_gc,                                          \
    gc_basic_stats_roots_traced,                                        \
    gc_basic_stats_heap_traced,                                         \
    gc_basic_stats_ephemerons_traced,                                   \
    gc_basic_stats_finalizers_traced,                                   \
    gc_basic_stats_restarting_mutators,                                 \
    gc_basic_stats_mutator_added,                                       \
    gc_basic_stats_mutator_cause_gc,                                    \
    gc_basic_stats_mutator_stopping,                                    \
    gc_basic_stats_mutator_stopped,                                     \
    gc_basic_stats_mutator_restarted,                                   \
    gc_basic_stats_mutator_removed,                                     \
    gc_basic_stats_heap_resized,                                        \
    gc_basic_stats_live_data_size,                                      \
  })

static inline void gc_basic_stats_finish(struct gc_basic_stats *stats) {
  uint64_t now = gc_basic_stats_now();
  uint64_t cpu_time = gc_basic_stats_cpu_time();
  stats->elapsed_mutator_usec += now - stats->last_time_usec;
  stats->cpu_mutator_usec += cpu_time - stats->last_cpu_time_usec;
  stats->last_time_usec = now;
  stats->last_cpu_time_usec = cpu_time;
}

static inline void gc_basic_stats_print(struct gc_basic_stats *stats, FILE *f) {
  fprintf(f, "Completed %" PRIu64 " major collections (%" PRIu64 " minor).\n",
          stats->major_collection_count, stats->minor_collection_count);
  uint64_t stopped = stats->elapsed_collector_usec;
  uint64_t elapsed = stats->elapsed_mutator_usec + stopped;
  uint64_t cpu_stopped = stats->cpu_collector_usec;
  uint64_t cpu_total = stats->cpu_mutator_usec + cpu_stopped;
  uint64_t ms = 1000; // per usec
  fprintf(f, "%" PRIu64 ".%.3" PRIu64 " ms total time "
          "(%" PRIu64 ".%.3" PRIu64 " stopped); "
          "%" PRIu64 ".%.3" PRIu64 " ms CPU time "
          "(%" PRIu64 ".%.3" PRIu64 " stopped).\n",
          elapsed / ms, elapsed % ms, stopped / ms, stopped % ms,
          cpu_total / ms, cpu_total % ms, cpu_stopped / ms, cpu_stopped % ms);
  uint64_t pause_median = gc_latency_median(&stats->pause_times);
  uint64_t pause_p95 = gc_latency_percentile(&stats->pause_times, 0.95);
  uint64_t pause_max = gc_latency_max(&stats->pause_times);
  fprintf(f, "%" PRIu64 ".%.3" PRIu64 " ms median pause time, "
          "%" PRIu64 ".%.3" PRIu64 " p95, "
          "%" PRIu64 ".%.3" PRIu64 " max.\n",
          pause_median / ms, pause_median % ms, pause_p95 / ms, pause_p95 % ms,
          pause_max / ms, pause_max % ms);
  double MB = 1e6;
  fprintf(f, "Heap size is %.3f MB (max %.3f MB); peak live data %.3f MB.\n",
          stats->heap_size / MB, stats->max_heap_size / MB,
          stats->max_live_data_size / MB);
}

#endif // GC_BASIC_STATS_H_
