#ifndef FREELIST_H
#define FREELIST_H

// A size-segregated freelist with linear-log buckets à la
// https://pvk.ca/Blog/2015/06/27/linear-log-bucketing-fast-versatile-simple/.

#include "gc-assert.h"
#include "gc-histogram.h"

#include <string.h>

#define DEFINE_FREELIST(name, max_value_bits, precision, node)          \
  struct name { node buckets[((max_value_bits) << (precision)) + 1]; }; \
  static inline size_t name##_num_size_classes(void) {                  \
    return ((max_value_bits) << (precision)) + 1;                       \
  }                                                                     \
  static inline uint64_t name##_bucket_min_val(size_t idx) {            \
    GC_ASSERT(idx < name##_num_size_classes());                         \
    return gc_histogram_bucket_min_val((precision), idx);               \
  }                                                                     \
  static inline void name##_init(struct name *f) {                      \
    memset(f, 0, sizeof(*f));                                           \
  }                                                                     \
  static inline size_t name##_size_class(uint64_t val) {                \
    return gc_histogram_bucket((max_value_bits), (precision), val);     \
  }                                                                     \
  static inline node* name##_bucket(struct name *f, uint64_t val) {     \
    return &f->buckets[name##_size_class(val)];                         \
  }

#endif // FREELIST_H
