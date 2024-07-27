# Parallel copying collector

Whippet's `pcc` collector is a copying collector, like the more simple
[`semi`](./collector-semi.md), but supporting multiple mutator threads
and multiple tracing threads.

Like `semi`, `pcc` traces by evacuation: it moves all live objects on
every collection.  (Exception:  objects larger than 8192 bytes are
placed into a partitioned space which traces by marking in place instead
of copying.)  Evacuation requires precise roots, so if your embedder
does not support precise roots, `pcc` is not for you.

Again like `semi`, `pcc` generally requires a heap size at least twice
as large as the maximum live heap size, and performs best with ample
heap sizes; between 3× and 5× is best.

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

However, unlike the simple semi-space collector which uses a Cheney grey
worklist, `pcc` uses the [fine-grained work-stealing parallel
tracer](../src/parallel-tracer.h) originally developed for [Whippet's
Immix-like collector](./collector-whippet.md).  Each trace worker
maintains a [local queue of objects that need
tracing](../src/local-worklist.h), which currently has 1024 entries.  If
the local queue becomes full, the worker will publish 3/4 of those
entries to the worker's [shared worklist](../src/shared-worklist.h).
When a worker runs out of local work, it will first try to remove work
from its own shared worklist, then will try to steal from other workers.

Because threads compete to evacuate objects, `pcc` uses [atomic
compare-and-swap instead of simple forwarding pointer
updates](./manual.md#forwarding-objects), which imposes around a ~30%
performance penalty.  `pcc` generally starts to outperform `semi` when
it can trace with 2 threads, and gets better with each additional
thread.

The memory used for the external worklist is dynamically allocated from
the OS and is not currently counted as contributing to the heap size.
If you are targetting a microcontroller or something, probably you need
to choose a different kind of collector that never dynamically
allocates, such as `semi`.
