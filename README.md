# Whippet Garbage Collector

This repository is for development of Whippet, a new garbage collector
implementation, eventually for use in [Guile
Scheme](https://gnu.org/s/guile).

Whippet is an embed-only C library, designed to be copied into a
program's source tree.  It exposes an abstract C API for managed memory
allocation, and provides a number of implementations of that API.

One of the implementations is also called "whippet", and is the
motivation for creating this library.  For a detailed introduction, see
[Whippet: Towards a new local
maximum](https://wingolog.org/archives/2023/02/07/whippet-towards-a-new-local-maximum),
a talk given at FOSDEM 2023.

## Documentation

 * [Design](./doc/design.md): What's the general idea?
 * [Manual](./doc/manual.md): How do you get your program to use
   Whippet?  What is the API?
 * [Guile](./doc/guile.md): Some notes on a potential rebase of Guile on
   top of Whippet.

## Source repository structure

 * [api/](./api/): The user-facing API.  Also, the "embedder API"; see
   the [manual](./doc/manual.md) for more.
 * [doc/](./doc/): Documentation, such as it is.
 * [src/](./src/): The actual GC implementation.  The specific
   implementations of the Whippet API are [`semi.c`](./src/semi.c), a
   semi-space collector; [`bdw.c`](./src/bdw.c), the third-party
   [BDW-GC](https://github.com/ivmai/bdwgc) conservative parallel
   stop-the-world mark-sweep segregated-fits collector with lazy
   sweeping; and [`whippet.c`](./src/whippet.c), the whippet collector.
 * [benchmarks/](./benchmarks/): Benchmarks.  A work in progress.
 * [test/](./test/): A dusty attic of minimal testing.

## To do

### Missing features before Guile can use Whippet

 - [X] Pinning
 - [X] Conservative stacks
 - [X] Conservative data segments
 - [ ] Heap growth/shrinking
 - [ ] Debugging/tracing
 - [X] Finalizers
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

```
Copyright (c) 2022-2023 Andy Wingo

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

Note that some benchmarks have other licenses; see
[`benchmarks/README.md`](./benchmarks/README.md) for more.
