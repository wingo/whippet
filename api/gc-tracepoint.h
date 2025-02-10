#ifndef GC_TRACEPOINT_H
#define GC_TRACEPOINT_H

#ifdef GC_TRACEPOINT_LTTNG

#include "gc-lttng.h"

#define GC_TRACEPOINT(...) \
  lttng_ust_tracepoint(whippet, __VA_ARGS__)

#else // GC_TRACEPOINT_LTTNG

#define GC_TRACEPOINT(...) do {} while (0)

#endif // GC_TRACEPOINT_LTTNG

#endif // GC_TRACEPOINT_H
