# Benchmarks

 - [`mt-gcbench.c`](./mt-gcbench.c): The multi-threaded [GCBench
   benchmark](https://hboehm.info/gc/gc_bench.html).  An old but
   standard benchmark that allocates different sizes of binary trees.
   As parameters it takes a heap multiplier and a number of mutator
   threads.  We analytically compute the peak amount of live data and
   then size the GC heap as a multiplier of that size.  It has a peak
   heap consumption of 10 MB or so per mutator thread: not very large.
   At a 2x heap multiplier, it causes about 30 collections for the `mmc`
   collector, and runs somewhere around 200-400 milliseconds in
   single-threaded mode, on the machines I have in 2022.  For low thread
   counts, the GCBench benchmark is small; but then again many Guile
   processes also are quite short-lived, so perhaps it is useful to
   ensure that small heaps remain lightweight.

   To stress `mmc`'s handling of fragmentation, we modified this
   benchmark to intersperse pseudorandomly-sized holes between tree
   nodes.

 - [`quads.c`](./quads.c): A synthetic benchmark that allocates quad
   trees.  The mutator begins by allocating one long-lived tree of depth
   N, and then allocates 13% of the heap in depth-3 trees, 20 times,
   simulating a fixed working set and otherwise an allocation-heavy
   workload.  By observing the times to allocate 13% of the heap in
   garbage we can infer mutator overheads, and also note the variance
   for the cycles in which GC hits.

## License

mt-gcbench.c was originally from https://hboehm.info/gc/gc_bench/, which
has a somewhat unclear license.  I have modified GCBench significantly
so that I can slot in different GC implementations.  Other files are
distributed under the Whippet license; see the top-level
[README.md](../README.md) for more.
