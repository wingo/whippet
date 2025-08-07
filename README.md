# Whippet Garbage Collector

This repository is for development of Whippet, a new garbage collector
implementation, initially targetted for use in [Guile
Scheme](https://gnu.org/s/guile).

Whippet is an embed-only C library, designed to be copied into a
program's source tree.  It exposes an abstract C API for managed memory
allocation, and provides a number of implementations of that API.

## Documentation

See the [documentation](./doc/README.md).

## Features

 - Per-object pinning (with `mmc` collectors)
 - Finalization (supporting resuscitation)
 - Ephemerons (except on `bdw`, which has a polyfill)
 - Conservative roots (optionally with `mmc` or always with `bdw`)
 - Precise roots (optionally with `mmc` or always with `semi` / `pcc`)
 - Precise embedder-parameterized heap tracing (except with `bdw`)
 - Conservative heap tracing (optionally with `mmc`, always with `bdw`)
 - Parallel tracing (except `semi`)
 - Parallel mutators (except `semi`)
 - Inline allocation / write barrier / safepoint fast paths (supporting JIT)
 - One unified API with no-overhead abstraction: switch collectors when
   you like
 - Three policies for sizing heaps: fixed, proportional to live size, and
   [MemBalancer](http://marisa.moe/balancer.html)

## Source repository structure

 * [api/](./api/): The user-facing API.  Also, the "embedder API"; see
   the [manual](./doc/manual.md) for more.
 * [doc/](./doc/): Documentation, such as it is.
 * [src/](./src/): The actual GC implementation, containing a number of
   collector implementations.  The embedder chooses which collector to
   use at compile-time.  See the [documentation](./doc/collectors.md)
   for more on the different collectors (`semi`, `bdw`, `pcc`, and the
   different flavors of `mmc`).
 * [benchmarks/](./benchmarks/): Benchmarks.  A work in progress.
 * [test/](./test/): A dusty attic of minimal testing.

## Status and roadmap

As of August 2025, Whippet is feature-complete, in the sense that it is
ready to replace [Guile](https://gnu.org/s/guile)'s use of the
Boehm-Demers-Weiser collector; see the [`wip-whippet` integration
branch](https://cgit.git.savannah.gnu.org/cgit/guile.git/tree/?h=wip-whippet).
We hope to incrementally merge this branch of Guile over to the main
Guile branch over the next months.

In the future, Whippet's internal collectors need to add support for
MacOS and Windows, to fix some usage of the C11 atomics API that clang
doesn't like, to test on AArch64, and

There are all kinds of new collector designs that it would be fun to
experiment with, but we are mostly in a getting-into-production phase at
the moment.  Feel very free to fork the repo to try out any ideas you
might have!

## About the name

It sounds better than WIP (work-in-progress) garbage collector, doesn't
it?  Also apparently a whippet is a kind of dog that is fast for its
size.  It would be nice if the Whippet collectors turn out to have this
property.

## License

```
Copyright (c) 2022-2025 Andy Wingo

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
