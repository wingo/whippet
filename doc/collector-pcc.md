# Parallel copying collector

Whippet's `pcc` collector is a copying collector, exactly like
[`scc`](./collector-scc.md), but supporting multiple tracing threads.
See the discussion of `scc` for a general overview.

Also like `scc` and `semi`, `pcc` is not generational yet.  If and when
`pcc` grows a young generation, it would be a great collector.

## Implementation notes

`pcc` supports tracing in parallel.  This mechanism works somewhat like
allocation, in which multiple trace workers compete to evacuate objects
into their local allocation buffers; when an allocation buffer is full,
the trace worker grabs another, just like mutators do.

To maintain a queue of objects to trace, `pcc` uses the [fine-grained
work-stealing parallel tracer](../src/parallel-tracer.h) originally
developed for [Whippet's Immix-like collector](./collector-whippet.md).
Each trace worker maintains a [local queue of objects that need
tracing](../src/local-worklist.h), which currently has 1024 entries.  If
the local queue becomes full, the worker will publish 3/4 of those
entries to the worker's [shared worklist](../src/shared-worklist.h).
When a worker runs out of local work, it will first try to remove work
from its own shared worklist, then will try to steal from other workers.

If only one tracing thread is enabled (`parallelism=1`), `pcc` uses
non-atomic forwarding, but if multiple threads compete to evacuate
objects, `pcc` uses [atomic compare-and-swap instead of simple
forwarding pointer updates](./manual.md#forwarding-objects).  This
imposes around a ~30% performance penalty but having multiple tracing
threads is generally worth it, unless the object graph is itself serial.

As with `scc`, the memory used for the external worklist is dynamically
allocated from the OS and is not currently counted as contributing to
the heap size.  If you are targetting a microcontroller or something,
probably you need to choose a different kind of collector that never
dynamically allocates, such as `semi`.
