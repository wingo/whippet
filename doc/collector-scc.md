# Serial copying collector

Whippet's `scc` collector is a copying collector, like the more simple
[`semi`](./collector-semi.md), but supporting multiple mutator threads,
and using an external FIFO worklist instead of a Cheney worklist.

Like `semi`, `scc` traces by evacuation: it moves all live objects on
every collection.  (Exception:  objects larger than 8192 bytes are
placed into a partitioned space which traces by marking in place instead
of copying.)  Evacuation requires precise roots, so if your embedder
does not support precise roots, `scc` is not for you.

Again like `semi`, `scc` generally requires a heap size at least twice
as large as the maximum live heap size, and performs best with ample
heap sizes; between 3× and 5× is best.

Overall, `scc` is most useful for isolating the performance implications
of using a block-structured heap and of using an external worklist
rather than a Cheney worklist as `semi` does.  It also supports multiple
mutator threads, so it is generally more useful than `semi`.  Also,
compared to `pcc`, we can measure the overhead that `pcc` imposes to
atomically forward objects.

But given a choice, you probably want `pcc`; though it's slower with
only one tracing thread, once you have more than once tracing thread
it's a win over `scc`.

## Implementation notes

Unlike `semi` which has a single global bump-pointer allocation region,
`scc` structures the heap into 64-kB blocks.  In this way it supports
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

Unlike the simple semi-space collector which uses a Cheney grey
worklist, `scc` uses a [simple first-in, first-out queue of objects to
be traced](../src/simple-worklist.h) originally developed for [Whippet's
Immix-like collector](./collector-whippet.md).  Like a Cheney worklist,
this should result in objects being copied in breadth-first order.  The
literature would suggest that depth-first is generally better for
locality, but that preserving allocation order is generally best.  This
is something to experiment with in the future.

The memory used for the external worklist is dynamically allocated from
the OS and is not currently counted as contributing to the heap size.
If you are targetting a microcontroller or something, probably you need
to choose a different kind of collector that never dynamically
allocates, such as `semi`.

