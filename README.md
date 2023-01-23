# Whippet Garbage Collector

This repository is for development of Whippet, a new garbage collector
implementation, eventually for use in [Guile
Scheme](https://gnu.org/s/guile).

## Design

Whippet is mainly a mark-region collector, like
[Immix](http://users.cecs.anu.edu.au/~steveb/pubs/papers/immix-pldi-2008.pdf).
See also the lovely detailed [Rust
implementation](http://users.cecs.anu.edu.au/~steveb/pubs/papers/rust-ismm-2016.pdf).

To a first approximation, Whippet is a whole-heap Immix collector with a
large object space on the side.  See the Immix paper for full details,
but basically Immix divides the heap into 32kB blocks, and then divides
those blocks into 128B lines.  An Immix allocation never spans blocks;
allocations larger than 8kB go into a separate large object space.
Mutators request blocks from the global store and allocate into those
blocks using bump-pointer allocation.  When all blocks are consumed,
Immix stops the world and traces the object graph, marking objects but
also the lines that objects are on.  After marking, blocks contain some
lines with live objects and others that are completely free.  Spans of
free lines are called holes.  When a mutator gets a recycled block from
the global block store, it allocates into those holes.  Also, sometimes
Immix can choose to evacuate rather than mark.  Bump-pointer-into-holes
allocation is quite compatible with conservative roots, so it's an
interesting option for Guile, which has a lot of legacy C API users.

The essential difference of Whippet from Immix stems from a simple
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

Other ideas in Whippet:

 * Minimize stop-the-world phase via parallel marking and punting all
   sweeping to mutators

 * Enable mutator parallelism via lock-free block acquisition and lazy
   statistics collation

 * Allocate block space using aligned 4 MB slabs, with embedded metadata
   to allow metadata bytes, slab headers, and block metadata to be
   located via address arithmetic

 * Facilitate conservative collection via mark byte array, oracle for
   "does this address start an object"

 * Enable in-place generational collection via card table with one entry
   per 256B or so

 * Enable concurrent marking by having three mark bit states (dead,
   survivor, marked) that rotate at each collection, and sweeping a
   block clears metadata for dead objects; but concurrent marking and
   associated SATB barrier not yet implemented

## What's there

This repository is a workspace for Whippet implementation.  As such, it
has files implementing Whippet itself.  It also has some benchmarks to
use in optimizing Whippet:

 - [`mt-gcbench.c`](./mt-gcbench.c): The multi-threaded [GCBench
   benchmark](https://hboehm.info/gc/gc_bench.html).  An old but
   standard benchmark that allocates different sizes of binary trees.
   As parameters it takes a heap multiplier and a number of mutator
   threads.  We analytically compute the peak amount of live data and
   then size the GC heap as a multiplier of that size.  It has a peak
   heap consumption of 10 MB or so per mutator thread: not very large.
   At a 2x heap multiplier, it causes about 30 collections for the
   whippet collector, and runs somewhere around 200-400 milliseconds in
   single-threaded mode, on the machines I have in 2022.  For low thread
   counts, the GCBench benchmark is small; but then again many Guile
   processes also are quite short-lived, so perhaps it is useful to
   ensure that small heaps remain lightweight.

   To stress Whippet's handling of fragmentation, we modified this
   benchmark to intersperse pseudorandomly-sized holes between tree
   nodes.

 - [`quads.c`](./quads.c): A synthetic benchmark that allocates quad
   trees.  The mutator begins by allocating one long-lived tree of depth
   N, and then allocates 13% of the heap in depth-3 trees, 20 times,
   simulating a fixed working set and otherwise an allocation-heavy
   workload.  By observing the times to allocate 13% of the heap in
   garbage we can infer mutator overheads, and also note the variance
   for the cycles in which GC hits.

The repository has two other collector implementations, to appropriately
situate Whippet's performance in context:

 - `bdw.h`: The external BDW-GC conservative parallel stop-the-world
   mark-sweep segregated-fits collector with lazy sweeping.
 - `semi.h`: Semispace copying collector.
 - `whippet.h`: The whippet collector.  Two different marking
   implementations: single-threaded and parallel.  Generational and
   non-generational variants, also.

## Guile

If the Whippet collector works out, it could replace Guile's garbage
collector.  Guile currently uses BDW-GC.  Guile has a widely used C API
and implements part of its run-time in C.  For this reason it may be
infeasible to require precise enumeration of GC roots -- we may need to
allow GC roots to be conservatively identified from data sections and
from stacks.  Such conservative roots would be pinned, but other objects
can be moved by the collector if it chooses to do so.  We assume that
object references within a heap object can be precisely identified.
(However, Guile currently uses BDW-GC in its default configuration,
which scans for references conservatively even on the heap.)

The existing C API allows direct access to mutable object fields,
without the mediation of read or write barriers.  Therefore it may be
impossible to switch to collector strategies that need barriers, such as
generational or concurrent collectors.  However, we shouldn't write off
this possibility entirely; an ideal replacement for Guile's GC will
offer the possibility of migration to other GC designs without imposing
new requirements on C API users in the initial phase.

In this regard, the Whippet experiment also has the goal of identifying
a smallish GC abstraction in Guile, so that we might consider evolving
GC implementation in the future without too much pain.  If we switch
away from BDW-GC, we should be able to evaluate that it's a win for a
large majority of use cases.

## To do

### Missing features before Guile can use Whippet

 - [X] Pinning
 - [X] Conservative stacks
 - [X] Conservative data segments
 - [ ] Heap growth/shrinking
 - [ ] Debugging/tracing
 - [ ] Finalizers
 - [X] Weak references / weak maps

### Features that would improve Whippet performance

 - [X] Immix-style opportunistic evacuation
 - ~~[ ] Overflow allocation~~ (should just evacuate instead)
 - [X] Generational GC via sticky mark bits
 - [ ] Generational GC with semi-space nursery
 - [ ] Concurrent marking with SATB barrier

## About the name

It sounds better than WIP (work-in-progress) garbage collector, doesn't
it?  Also apparently a whippet is a kind of dog that is fast for its
size.  It would be nice if whippet-gc turns out to have this property.

## License

gcbench.c, MT_GCBench.c, and MT_GCBench2.c are from
https://hboehm.info/gc/gc_bench/ and have a somewhat unclear license.  I
have modified GCBench significantly so that I can slot in different GC
implementations.  The GC implementations themselves are available under
a MIT-style license, the text of which follows:

```
Copyright (c) 2022 Andy Wingo

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
