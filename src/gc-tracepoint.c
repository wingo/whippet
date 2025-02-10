#include <assert.h>
#ifdef GC_TRACEPOINT_LTTNG
#define LTTNG_UST_TRACEPOINT_DEFINE
#define LTTNG_UST_TRACEPOINT_CREATE_PROBES
#include "gc-lttng.h"
#endif // GC_TRACEPOINT_LTTNG
