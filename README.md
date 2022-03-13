# GC workbench

This repository is a workbench for implementing different GCs.  It's a
scratch space.

## What's there

There's just the (modified) GCBench, which is an old but standard
benchmark that allocates different sizes of binary trees.  It takes a
heap of 25 MB or so, not very large, and causes somewhere between 20 and
50 collections, running in 100 to 500 milliseconds on 2022 machines.

Then there are currently three collectors:

 - `bdw.h`: The external BDW-GC conservative parallel stop-the-world
   mark-sweep segregated-fits collector with lazy sweeping.
 - `semi.h`: Semispace copying collector.
 - `mark-sweep.h`: Stop-the-world mark-sweep segregated-fits collector
   with lazy sweeping.  Two different marking algorithms:
   single-threaded and parallel.

The two latter collectors reserve one word per object on the header,
which makes them collect more frequently than `bdw` because the `Node`
data type takes 32 bytes instead of 24 bytes.

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
 - [ ] Adapt all GC implementations to allow multiple mutator threads.
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
