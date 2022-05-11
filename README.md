# Whippet Garbage Collector

This repository is for development of Whippet, a new garbage collector
implementation, eventually for use in [Guile
Scheme](https://gnu.org/s/guile).

## Design

Whippet is a mark-region collector, like
[Immix](http://users.cecs.anu.edu.au/~steveb/pubs/papers/immix-pldi-2008.pdf).
See also the lovely detailed [Rust
implementation](http://users.cecs.anu.edu.au/~steveb/pubs/papers/rust-ismm-2016.pdf).

To a first approximation, Whippet is a whole-heap Immix collector.  See
the Immix paper for full details, but basically Immix divides the heap
into 32kB blocks, and then divides those blocks into 128B lines.  An
Immix allocation never spans blocks; allocations larger than 8kB go into
a separate large object space.  Mutators request blocks from the global
store and allocate into those blocks using bump-pointer allocation.
When all blocks are consumed, Immix stops the world and traces the
object graph, marking objects but also the lines that objects are on.
After marking, blocks contain some lines with live objects and others
that are completely free.  Spans of free lines are called holes.  When a
mutator gets a recycled block from the global block store, it allocates
into those holes.  Also, sometimes Immix can choose to evacuate rather
than mark.  Bump-pointer-into-holes allocation is quite compatible with
conservative roots, so it's an interesting option for Guile, which has a
lot of legacy C API users.

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
don't need GC bits in the object itself, and you get a number of other
benefits from the mark byte table -- you can also stuff other per-object
data there, such as pinning bits, nursery and remset bits, multiple mark
colors for concurrent marking, and you can also use the mark byte (which
is now a metadata byte) to record the object end, so that finding holes
in a block can just read the mark table and can avoid looking at object
memory.

Other ideas in whippet:

 * Minimize stop-the-world phase via parallel marking and punting all
   sweeping to mutators

 * Enable mutator parallelism via lock-free block acquisition and lazy
   statistics collation

 * Allocate block space using aligned 4 MB slabs, with embedded metadata
   to allow metadata bytes, slab headers, and block metadata to be
   located via address arithmetic

 * Facilitate conservative collection via mark byte array, oracle for
   "does this address start an object"

 * Enable in-place generational collection via nursery bit in metadata
   byte for new allocations, remset bit for objects that should be
   traced for nursery roots, and a card table with one entry per 256B or
   so; but write barrier and generational trace not yet implemented

 * Enable concurrent marking by having three mark bit states (dead,
   survivor, marked) that rotate at each collection, and sweeping a
   block clears metadata for dead objects; but concurrent marking and
   associated SATB barrier not yet implemented

## What's there

There are currently three collectors:

 - `bdw.h`: The external BDW-GC conservative parallel stop-the-world
   mark-sweep segregated-fits collector with lazy sweeping.
 - `semi.h`: Semispace copying collector.
 - `mark-sweep.h`: The whippet collector.  Two different marking algorithms:
   single-threaded and parallel.

The two latter collectors reserve one word per object on the header,
which might make them collect more frequently than `bdw` because the
`Node` data type takes 32 bytes instead of 24 bytes.

These collectors are sketches and exercises for improving Guile's
garbage collector.  Guile currently uses BDW-GC.  In Guile if we have an
object reference we generally have to be able to know what kind of
object it is, because there are few global invariants enforced by
typing.  Therefore it is reasonable to consider allowing the GC and the
application to share the first word of an object, for example to maybe
store a mark bit (though an on-the-side mark byte seems to allow much
more efficient sweeping, for mark-sweep), to allow the application to
know what kind an object is, to allow the GC to find references within
the object, to allow the GC to compute the object's size, and so on.

There's just the (modified) GCBench, which is an old but standard
benchmark that allocates different sizes of binary trees.  As parameters
it takes a heap multiplier and a number of mutator threads.  We
analytically compute the peak amount of live data and then size the GC
heap as a multiplier of that size.  It has a peak heap consumption of 10
MB or so per mutator thread: not very large.  At a 2x heap multiplier,
it causes about 30 collections for the whippet collector, and runs
somewhere around 200-400 milliseconds in single-threaded mode, on the
machines I have in 2022.

The GCBench benchmark is small but then again many Guile processes also
are quite short-lived, so perhaps it is useful to ensure that small
heaps remain lightweight.

Guile has a widely used C API and implements part of its run-time in C.
For this reason it may be infeasible to require precise enumeration of
GC roots -- we may need to allow GC roots to be conservatively
identified from data sections and from stacks.  Such conservative roots
would be pinned, but other objects can be moved by the collector if it
chooses to do so.  We assume that object references within a heap object
can be precisely identified.  (The current BDW-GC scans for references
conservatively even on the heap.)

A generationa
A likely good solution for Guile would be an [Immix
collector](https://www.cs.utexas.edu/users/speedway/DaCapo/papers/immix-pldi-2008.pdf)
with conservative roots, and a parallel stop-the-world mark/evacuate
phase.  We would probably follow the [Rust
implementation](http://users.cecs.anu.edu.au/~steveb/pubs/papers/rust-ismm-2016.pdf),
more or less, with support for per-line pinning.  In an ideal world we
would work out some kind of generational solution as well, either via a
semispace nursery or via sticky mark bits, but this requires Guile to
use a write barrier -- something that's possible to do within Guile
itself but it's unclear if we can extend this obligation to users of
Guile's C API.

In any case, these experiments also have the goal of identifying a
smallish GC abstraction in Guile, so that we might consider evolving GC
implementation in the future without too much pain.  If we switch away
from BDW-GC, we should be able to evaluate that it's a win for a large
majority of use cases.

## To do

 - [X] Implement a parallel marker for the mark-sweep collector.
 - [X] Adapt all GC implementations to allow multiple mutator threads.
   Update gcbench.c.
 - [ ] Implement precise non-moving Immix whole-heap collector.
 - [ ] Add evacuation to Immix whole-heap collector.
 - [ ] Add parallelism to Immix stop-the-world phase.
 - [ ] Implement conservative root-finding for the mark-sweep collector.
 - [ ] Implement conservative root-finding and pinning for Immix.
 - [ ] Implement generational GC with semispace nursery and mark-sweep
   old generation.
 - [ ] Implement generational GC with semispace nursery and Immix
   old generation.

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
