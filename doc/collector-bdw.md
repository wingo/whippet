# Boehm-Demers-Weiser collector

Whippet's `bdw` collector is backed by a third-party garbage collector,
the [Boehm-Demers-Weiser collector](https://github.com/ivmai/bdwgc).

BDW-GC is a mark-sweep collector with conservative root-finding,
conservative heap tracing, and parallel tracing.

Whereas the other Whippet collectors which rely on mutators to
[periodically check if they need to
stop](https://github.com/wingo/whippet/blob/main/doc/manual.md#safepoints),
`bdw` will stop mutators with a POSIX signal.  Also, it doesn't really
support ephemerons (the Whippet `bdw` collector simulates them using
finalizers), and both ephemerons and finalizers only approximate the
Whippet behavior, because they are implemented in terms of what BDW-GC
provides.

`bdw` supports the `fixed` and `growable` heap-sizing policies, but not
`adaptive`, as BDW-GC can't reliably return memory to the OS.  Also,
[`growable` has an effective limit of a 3x heap
multiplier](https://github.com/wingo/whippet/blob/main/src/bdw.c#L478).
Oh well!

It's a bit of an oddball from a Whippet perspective, but useful as a
migration path if you have an embedder that is already using BDW-GC.
And, it is a useful performance comparison.
