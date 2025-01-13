# Parallel copying collector

Whippet's `pcc` collector is a copying collector, like the more simple
[`semi`](./collector-semi.md), but supporting multiple mutator threads,
multiple tracing threads, and using an external FIFO worklist instead of
a Cheney worklist.

Like `semi`, `pcc` traces by evacuation: it moves all live objects on
every collection.  (Exception:  objects larger than 8192 bytes are
placed into a partitioned space which traces by marking in place instead
of copying.)  Evacuation requires precise roots, so if your embedder
does not support precise roots, `pcc` is not for you.

Again like `semi`, `pcc` generally requires a heap size at least twice
as large as the maximum live heap size, and performs best with ample
heap sizes; between 3× and 5× is best.

Overall, `pcc` is a better version of `semi`.  It should have broadly
the same performance characteristics with a single mutator and with
parallelism disabled, additionally allowing multiple mutators, and
scaling better with multiple tracing threads.

`pcc` has a generational configuration, conventionally referred to as
`generational-pcc`, in which both the nursery and the old generation are
copy spaces.  Objects stay in the nursery for one cycle before moving on
to the old generation.  This configuration is a bit new (January 2025)
and still needs some tuning.

## Implementation notes

Unlike `semi` which has a single global bump-pointer allocation region,
`pcc` structures the heap into 64-kB blocks.  In this way it supports
multiple mutator threads: mutators do local bump-pointer allocation into
their own block, and when their block is full, they fetch another from
the global store.

The block size is 64 kB, but really it's 128 kB, because each block has
two halves: the active region and the copy reserve.  Dividing each block
in two allows the collector to easily grow and shrink the heap while
ensuring there is always enough reserve space.

Blocks are allocated in 64-MB aligned slabs, so there are 512 blocks in
a slab.  The first block in a slab is used by the collector itself, to
keep metadata for the rest of the blocks, for example a chain pointer
allowing blocks to be collected in lists, a saved allocation pointer for
partially-filled blocks, whether the block is paged in or out, and so
on.

`pcc` supports tracing in parallel.  This mechanism works somewhat like
allocation, in which multiple trace workers compete to evacuate objects
into their local allocation buffers; when an allocation buffer is full,
the trace worker grabs another, just like mutators do.

Unlike the simple semi-space collector which uses a Cheney grey
worklist, `pcc` uses an external worklist.  If parallelism is disabled
at compile-time, it uses a simple first-in, first-out queue of objects
to be traced.  Like a Cheney worklist, this should result in objects
being copied in breadth-first order.  The literature would suggest that
depth-first is generally better for locality, but that preserving
allocation order is generally best.  This is something to experiment
with in the future.

If parallelism is enabled, as it is by default, `pcc` uses a
[fine-grained work-stealing parallel tracer](../src/parallel-tracer.h).
Each trace worker maintains a [local queue of objects that need
tracing](../src/local-worklist.h), which currently has 1024 entries.  If
the local queue becomes full, the worker will publish 3/4 of those
entries to the worker's [shared worklist](../src/shared-worklist.h).
When a worker runs out of local work, it will first try to remove work
from its own shared worklist, then will try to steal from other workers.

If only one tracing thread is enabled at run-time (`parallelism=1`) (or
if parallelism is disabled at compile-time), `pcc` will evacuate by
non-atomic forwarding, but if multiple threads compete to evacuate
objects, `pcc` uses [atomic compare-and-swap instead of simple
forwarding pointer updates](./manual.md#forwarding-objects).  This
imposes around a ~30% performance penalty but having multiple tracing
threads is generally worth it, unless the object graph is itself serial.

The memory used for the external worklist is dynamically allocated from
the OS and is not currently counted as contributing to the heap size.
If you are targetting a microcontroller or something, probably you need
to choose a different kind of collector that never dynamically
allocates, such as `semi`.
