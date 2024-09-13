#ifndef GC_OPTIONS_INTERNAL_H
#define GC_OPTIONS_INTERNAL_H

#ifndef GC_IMPL
#error internal header file, not part of API
#endif

#include "gc-options.h"

struct gc_common_options {
  enum gc_heap_size_policy heap_size_policy;
  size_t heap_size;
  size_t maximum_heap_size;
  double heap_size_multiplier;
  double heap_expansiveness;
  int parallelism;
};

GC_INTERNAL void gc_init_common_options(struct gc_common_options *options);

GC_INTERNAL int gc_common_option_from_string(const char *str);

GC_INTERNAL int gc_common_options_set_int(struct gc_common_options *options,
                                          int option, int value);
GC_INTERNAL int gc_common_options_set_size(struct gc_common_options *options,
                                           int option, size_t value);
GC_INTERNAL int gc_common_options_set_double(struct gc_common_options *options,
                                             int option, double value);
GC_INTERNAL int gc_common_options_parse_and_set(struct gc_common_options *options,
                                                int option, const char *value);

#endif // GC_OPTIONS_INTERNAL_H
