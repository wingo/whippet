#ifndef GC_HISTOGRAM_H
#define GC_HISTOGRAM_H

#include "gc-assert.h"

#include <stdint.h>

static inline size_t gc_histogram_bucket(uint64_t max_value_bits,
                                         uint64_t precision,
                                         uint64_t val) {
  uint64_t major = val < (1ULL << precision)
    ? 0ULL
    : 64ULL - __builtin_clzl(val) - precision;
  uint64_t minor = val < (1 << precision)
    ? val
    : (val >> (major - 1ULL)) & ((1ULL << precision) - 1ULL);
  uint64_t idx = (major << precision) | minor;
  if (idx >= (max_value_bits << precision))
    idx = max_value_bits << precision;
  return idx;
}

static inline uint64_t gc_histogram_bucket_min_val(uint64_t precision,
                                                   size_t idx) {
  uint64_t major = idx >> precision;
  uint64_t minor = idx & ((1ULL << precision) - 1ULL);
  uint64_t min_val = major
    ? ((1ULL << precision) | minor) << (major - 1ULL)
    : minor;
  return min_val;
}

#define GC_DEFINE_HISTOGRAM(name, max_value_bits, precision)            \
  struct name { uint32_t buckets[((max_value_bits) << (precision)) + 1]; }; \
  static inline size_t name##_size(void) {                              \
    return ((max_value_bits) << (precision)) + 1;                       \
  }                                                                     \
  static inline uint64_t name##_bucket_min_val(size_t idx) {            \
    GC_ASSERT(idx < name##_size());                                     \
    return gc_histogram_bucket_min_val((precision), idx);               \
  }                                                                     \
  static inline struct name make_##name(void) {                         \
    return (struct name) { { 0, }};                                     \
  }                                                                     \
  static inline void name##_record(struct name *h, uint64_t val) {      \
    h->buckets[gc_histogram_bucket((max_value_bits), (precision), val)]++; \
  }                                                                     \
  static inline uint64_t name##_ref(struct name *h, size_t idx) {       \
    GC_ASSERT(idx < name##_size());                                     \
    return h->buckets[idx];                                             \
  }                                                                     \
  static inline uint64_t name##_min(struct name *h) {                   \
    for (size_t bucket = 0; bucket < name##_size(); bucket++)           \
      if (h->buckets[bucket]) return name##_bucket_min_val(bucket);     \
    return -1;                                                          \
  }                                                                     \
  static inline uint64_t name##_max(struct name *h) {                   \
    if (h->buckets[name##_size()-1]) return -1LL;                       \
    for (ssize_t bucket = name##_size() - 1; bucket >= 0; bucket--)     \
      if (h->buckets[bucket]) return name##_bucket_min_val(bucket+1);   \
    return 0;                                                           \
  }                                                                     \
  static inline uint64_t name##_count(struct name *h) {                 \
    uint64_t sum = 0;                                                   \
    for (size_t bucket = 0; bucket < name##_size(); bucket++)           \
      sum += h->buckets[bucket];                                        \
    return sum;                                                         \
  }                                                                     \
  static inline uint64_t name##_percentile(struct name *h, double p) {  \
    uint64_t n = name##_count(h) * p;                                   \
    uint64_t sum = 0;                                                   \
    for (size_t bucket = 0; bucket + 1 < name##_size(); bucket++) {     \
      sum += h->buckets[bucket];                                        \
      if (sum >= n) return name##_bucket_min_val(bucket+1);             \
    }                                                                   \
    return -1ULL;                                                       \
  }                                                                     \
  static inline uint64_t name##_median(struct name *h) {                \
    return name##_percentile(h, 0.5);                                   \
  }

#endif // GC_HISTOGRAM_H
