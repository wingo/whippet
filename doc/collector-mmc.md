# Mostly-marking collector

The `mmc` collector is mainly a mark-region collector, inspired by
[Immix](http://users.cecs.anu.edu.au/~steveb/pubs/papers/immix-pldi-2008.pdf).
To a first approximation, `mmc` is a whole-heap Immix collector with a
large object space on the side.

When tracing, `mmc` mostly marks objects in place.  If the heap is
too fragmented, it can compact the heap by choosing to evacuate
sparsely-populated heap blocks instead of marking in place.  However
evacuation is strictly optional, which means that `mmc` is also
compatible with conservative root-finding, making it a good replacement
for embedders that currently use the [Boehm-Demers-Weiser
collector](./collector-bdw.md).

## Differences from Immix

The original Immix divides the heap into 32kB blocks, and then divides
those blocks into 128B lines.  An Immix allocation can span lines but
not blocks; allocations larger than 8kB go into a separate large object
space.  Mutators request blocks from the global store and allocate into
those blocks using bump-pointer allocation.  When all blocks are
consumed, Immix stops the world and traces the object graph, marking
objects but also the lines that objects are on.  After marking, blocks
contain some lines with live objects and others that are completely
free.  Spans of free lines are called holes.  When a mutator gets a
recycled block from the global block store, it allocates into those
holes.  For an exposition of Immix, see the lovely detailed [Rust
implementation](http://users.cecs.anu.edu.au/~steveb/pubs/papers/rust-ismm-2016.pdf).

The essential difference of `mmc` from Immix stems from a simple
observation: Immix needs a side table of line mark bytes and also a mark
bit or bits in each object (or in a side table).  But if instead you
choose to store mark bytes instead of bits (for concurrency reasons) in
a side table, with one mark byte per granule (unit of allocation,
perhaps 16 bytes), then you effectively have a line mark table where the
granule size is the line size.  You can bump-pointer allocate into holes
in the mark byte table.

You might think this is a bad tradeoff, and perhaps it is: I don't know
yet.  If your granule size is two pointers, then one mark byte per
granule is 6.25% overhead on 64-bit, or 12.5% on 32-bit.  Especially on
32-bit, it's a lot!  On the other hand, instead of the worst case of one
survivor object wasting a line (or two, in the case of conservative line
marking), granule-size-is-line-size instead wastes nothing.  Also, you
don't need GC bits in the object itself, and you can use the mark byte
array to record the object end, so that finding holes in a block can
just read the mark table and can avoid looking at object memory.

## Optional features

The `mmc` collector has a few feature flags that can be turned on or
off.  If you use the [standard embedder makefile include](../embed.mk),
then there is a name for each combination of features: `mmc` has no
additional features, `parallel-mmc` enables parallel marking,
`parallel-generational-mmc` enables generations,
`stack-conservative-parallel-generational-mmc` uses conservative
root-finding, and `heap-conservative-parallel-generational-mmc`
additionally traces the heap conservatively.  You can leave off
components of the name to get a collector without those features.
Underneath this corresponds to some pre-processor definitions passed to
the compiler on the command line.

### Generations

`mmc` supports generational tracing via the [sticky mark-bit
algorithm](https://wingolog.org/archives/2022/10/22/the-sticky-mark-bit-algorithm).
This requires that the embedder emit [write
barriers](https://github.com/wingo/whippet/blob/main/doc/manual.md#write-barriers);
if your embedder cannot ensure write barriers are always invoked, then
generational collection is not for you.  (We could perhaps relax this a
bit, following what [Ruby developers
did](http://rvm.jp/~ko1/activities/rgengc_ismm.pdf).)

The write barrier is currently a card-marking barrier emitted on stores,
with one card byte per 256 object bytes, where the card location can be
computed from the object address because blocks are allocated in
two-megabyte aligned slabs.

### Parallel tracing

You almost certainly want this on!  `parallel-mmc` uses a the
[fine-grained work-stealing parallel tracer](../src/parallel-tracer.h).
Each trace worker maintains a [local queue of objects that need
tracing](../src/local-worklist.h), which currently has a capacity of
1024 entries.  If the local queue becomes full, the worker will publish
3/4 of those entries to the worker's [shared
worklist](../src/shared-worklist.h).  When a worker runs out of local
work, it will first try to remove work from its own shared worklist,
then will try to steal from other workers.

The memory used for the external worklist is dynamically allocated from
the OS and is not currently counted as contributing to the heap size.
If you absolutely need to avoid dynamic allocation during GC, `mmc`
(even `serial-mmc`) would need some work for your use case, to allocate
a fixed-size space for a marking queue and to gracefully handle mark
queue overflow.

### Conservative stack scanning

With `semi` and `pcc`, embedders must precisely enumerate the set of
*roots*: the edges into the heap from outside.  Commonly, roots include
global variables, as well as working variables from each mutator's
stack.  `mmc` can optionally mark mutator stacks *conservatively*:
treating each word on the stack as if it may be an object reference, and
marking any object at that address.

After all these years, *whether* to mark stacks conservatively or not is
still an open research question.  Conservative stack scanning can retain
too much data if an integer is confused for an object reference and
removes a layer of correctness-by-construction from a system.  Sometimes
conservative stack-scanning is required, for example if your embedder
cannot enumerate roots precisely.  But there are reasons to consider it
even if you can do precise roots: conservative scanning removes the need
for the compiler to produce a stack map to store the precise root
enumeration at every safepoint; it removes the need to look up a stack
map when tracing; and it allows C or C++ support code to avoid having to
place roots in traceable locations published to the garbage collector.
And the [performance question is still
open](https://dl.acm.org/doi/10.1145/2660193.2660198).

Anyway.  `mmc` can scan roots conservatively.  Those roots are pinned
for the collection; even if the collection will compact via evacuation,
referents of conservative roots won't be moved.  Objects not directly
referenced by roots can be evacuated, however.

### Conservative heap scanning

In addition to stack and global references, the Boehm-Demers-Weiser
collector scans heap objects conservatively as well, treating each word
of each heap object as if it were a reference.  `mmc` can do that, if
the embedder is unable to provide a `gc_trace_object` implementation.
However this is generally a performance lose, and it prevents
evacuation.

## Other implementation tidbits

`mmc` does lazy sweeping: as a mutator grabs a fresh block, it
reclaims memory that was unmarked in the previous collection before
making the memory available for allocation.  This makes sweeping
naturally cache-friendly and parallel.

The mark byte array facilitates conservative collection by being an
oracle for "does this address start an object".

For a detailed introduction, see [Whippet: Towards a new local
maximum](https://wingolog.org/archives/2023/02/07/whippet-towards-a-new-local-maximum),
a talk given at FOSDEM 2023.
