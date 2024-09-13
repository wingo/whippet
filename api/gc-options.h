#ifndef GC_OPTIONS_H
#define GC_OPTIONS_H

#include "gc-visibility.h"

enum gc_heap_size_policy {
  GC_HEAP_SIZE_FIXED,
  GC_HEAP_SIZE_GROWABLE,
  GC_HEAP_SIZE_ADAPTIVE,
};

enum {
  GC_OPTION_HEAP_SIZE_POLICY,
  GC_OPTION_HEAP_SIZE,
  GC_OPTION_MAXIMUM_HEAP_SIZE,
  GC_OPTION_HEAP_SIZE_MULTIPLIER,
  GC_OPTION_HEAP_EXPANSIVENESS,
  GC_OPTION_PARALLELISM
};

struct gc_options;

GC_API_ int gc_option_from_string(const char *str);

GC_API_ struct gc_options* gc_allocate_options(void);

GC_API_ int gc_options_set_int(struct gc_options *options, int option,
                               int value);
GC_API_ int gc_options_set_size(struct gc_options *options, int option,
                                size_t value);
GC_API_ int gc_options_set_double(struct gc_options *options, int option,
                                  double value);

GC_API_ int gc_options_parse_and_set(struct gc_options *options,
                                     int option, const char *value);
GC_API_ int gc_options_parse_and_set_many(struct gc_options *options,
                                          const char *str);

#endif // GC_OPTIONS_H
