#define LTTNG_UST_TRACEPOINT_PROVIDER whippet

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "gc-lttng.h"

#if !defined(_TP_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _TP_H

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_ENUM(
  whippet, gc_kind,
  LTTNG_UST_TP_ENUM_VALUES
  (lttng_ust_field_enum_value("MINOR", 1)
   lttng_ust_field_enum_value("MAJOR", 2)
   lttng_ust_field_enum_value("COMPACTING", 3)))

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
  whippet, tracepoint,
  LTTNG_UST_TP_ARGS(),
  LTTNG_UST_TP_FIELDS())

LTTNG_UST_TRACEPOINT_EVENT_CLASS(
  whippet, size_tracepoint,
  LTTNG_UST_TP_ARGS(size_t, size),
  LTTNG_UST_TP_FIELDS(lttng_ust_field_integer(size_t, size, size)))


/* The tracepoint instances */
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, size_tracepoint, whippet, init,
  LTTNG_UST_TP_ARGS(size_t, size))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, size_tracepoint, whippet, heap_resized,
  LTTNG_UST_TP_ARGS(size_t, size))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, size_tracepoint, whippet, live_data_size,
  LTTNG_UST_TP_ARGS(size_t, size))

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, requesting_stop, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, waiting_for_stop, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutators_stopped, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT(
  whippet, prepare_gc,
  LTTNG_UST_TP_ARGS(int, gc_kind),
  LTTNG_UST_TP_FIELDS(
    lttng_ust_field_enum(whippet, gc_kind, int, gc_kind, gc_kind)))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, roots_traced, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, heap_traced, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, ephemerons_traced, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, finalizers_traced, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, restarting_mutators, LTTNG_UST_TP_ARGS())

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_added, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_cause_gc, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_stopping, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_stopped, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_restarted, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, mutator_removed, LTTNG_UST_TP_ARGS())

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_unpark_all, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_share, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_check_termination_begin, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_check_termination_end, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_steal, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_roots_begin, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_roots_end, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_objects_begin, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_objects_end, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_worker_begin, LTTNG_UST_TP_ARGS())
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(
  whippet, tracepoint, whippet, trace_worker_end, LTTNG_UST_TP_ARGS())

#endif /* _TP_H */

#include <lttng/tracepoint-event.h>
