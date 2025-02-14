#!/usr/bin/env python3
# Any copyright is dedicated to the Public Domain.
# https://creativecommons.org/publicdomain/zero/1.0/
#
# Originally written by Andy Wingo <wingo@igalia.com>.

import bt2 # From the babeltrace2 package.
import sys
import json
from enum import Enum

# Usage: ./ctf_to_json.py ~/lttng-traces/name-of-your-trace > foo.json
#
# Convert a Common Trace Format (CTF) trace, for example as produced by
# LTTng, to the JSON-based Trace Event Format (TEF), for example as
# consumed by `chrome://tracing`, `https://ui.perfetto.dev/`, or
# `https://profiler.firefox.com`.

# The Trace Event Format is documented here:
#
# https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview?tab=t.0

# By default, events are emitted as EventPhase.INSTANT.  We also support
# rewriting the event stream so as to generate EventPhase.BEGIN /
# EventPhase.END events for specific named events.

synthetic_events = {
    'gc': ['whippet:mutator_cause_gc',
           'whippet:restarting_mutators'],
    'stop-the-world': ['whippet:requesting_stop',
                       'whippet:mutators_stopped'],
    'trace': ['whippet:prepare_gc',
              'whippet:restarting_mutators'],
    'mutator-stopped': ['whippet:mutator_stopping',
                        'whippet:mutator_restarted'],
    'trace-roots': ['whippet:trace_roots_begin',
                    'whippet:trace_roots_end'],
    'trace-check-termination': ['whippet:trace_check_termination_begin',
                                'whippet:trace_check_termination_end'],
    'trace-objects': ['whippet:trace_objects_begin',
                      'whippet:trace_objects_end'],
    'trace-worker': ['whippet:trace_worker_begin',
                     'whippet:trace_worker_end']
}

class EventPhase(Enum):
    BEGIN = 'B'
    END = 'E'
    COMPLETE = 'X'
    INSTANT = 'i'
    COUNTER = 'C'
    NESTABLE_START = 'b'
    NESTABLE_INSTANT = 'n'
    NESTABLE_END = 'e'
    FLOW_START = 's'
    FLOW_STEP = 't'
    FLOW_END = 'f'
    SAMPLE = 'P'
    OBJECT_CREATED = 'N'
    OBJECT_SNAPSHOT = 'O'
    OBJECT_DESTROYED = 'D'
    METADATA = 'M'
    MEMORY_DUMP_GLOBAL = 'V'
    MEMORY_DUMP_PROCESS = 'V'
    MARK = 'R'
    CLOCK_SYNC = 'c'
    CONTEXT_BEGIN = '('
    CONTEXT_END = ')'

base_time = None
def event_us(msg):
    assert(msg.default_clock_snapshot.clock_class.name == 'monotonic')
    assert(msg.default_clock_snapshot.clock_class.frequency == 1e9)
    global base_time
    ns = msg.default_clock_snapshot.value
    if base_time is None:
        base_time = ns
    return (ns - base_time) * 1e-3

def lower(x):
    if isinstance(x, str) or isinstance(x, int) or isinstance(x, float):
        return x
    if isinstance(x, dict) or isinstance(x, bt2._StructureFieldConst):
        return {lower(k):lower(v) for k, v in x.items()}
    if isinstance(x, bt2._BoolValueConst) or isinstance(x, bt2._BoolFieldConst):
        return bool(x)
    if isinstance(x, bt2._EnumerationFieldConst):
        return repr(x)
    if isinstance(x, bt2._IntegerValueConst) or isinstance(x, bt2._IntegerFieldConst):
        return int(x)
    if isinstance(x, bt2._RealValueConst) or isinstance(x, bt2._RealFieldConst):
        return float(x)
    if isinstance(x, bt2._StringValueConst) or isinstance(x, bt2._StringFieldConst):
        return str(x)
    raise ValueError("Unexpected value from trace", x)

# Specific Whippet events.
synthetic_begin = {}
synthetic_end = {}
for synthetic, [begin, end] in synthetic_events.items():
    synthetic_begin[begin] = []
    synthetic_end[end] = []
for synthetic, [begin, end] in synthetic_events.items():
    synthetic_begin[begin].append(synthetic)
    synthetic_end[end].append(synthetic)

def put(str):
    sys.stdout.write(str)

need_comma = False
def print_event(ev):
    global need_comma
    if need_comma:
        sys.stdout.write(',\n    ')
    else:
        need_comma = True
    # It appears to be faster to make a string, then print the string,
    # than to call json.dump with a file object.
    # json.dump(ev, sys.stdout, ensure_ascii=False, check_circular=False)
    put(json.dumps(ev, ensure_ascii=False, check_circular=False))

def emit_event(msg, name, phase):
    ev = {'name': name,
          'cat': 'whippet',
          'ph': phase.value,
          'ts': event_us(msg),
          'pid': lower(msg.event.common_context_field['vpid']),
          'tid': lower(msg.event.common_context_field['vtid']),
          'args': lower(msg.event.payload_field)}
    print_event(ev)
def emit_begin_event(msg, name):
    emit_event(msg, name, EventPhase.BEGIN)
def emit_end_event(msg, name):
    emit_event(msg, name, EventPhase.END)

def emit_events(msg):
    emit_event(msg, msg.event.name, EventPhase.INSTANT)
    for begin in synthetic_begin.get(msg.event.name, []):
        emit_begin_event(msg, begin)
    for end in synthetic_end.get(msg.event.name, []):
        emit_end_event(msg, end)

def ctf_to_json(path):
    msg_it = bt2.TraceCollectionMessageIterator(path)
    put('{\n')
    put('  "traceEvents": [\n    ')
    for msg in msg_it:
        if hasattr(msg, 'event'):
            emit_events(msg)
    put('\n')
    put('\n  ],\n')
    put('  "displayTimeUnit": "ns"\n')
    put('}\n')

if len(sys.argv) != 2:
    sys.stderr.write(
        'usage: ' + sys.argv[0] + ' ~/lttng-traces/name-of-your-trace\n')
    sys.exit(1)
else:
    ctf_to_json(sys.argv[1])
