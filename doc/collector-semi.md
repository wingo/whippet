# Semi-space collector

The `semi` collector is simple.  It is mostly useful as a first
collector to try out, to make sure that a mutator correctly records all
roots: because `semi` moves every live object on every collection, it is
very effective at shaking out mutator bugs.

If your embedder chooses to not precisely record roots, for example
instead choosing to conservatively scan the stack, then the semi-space
collector is not for you: `semi` requires precise roots.

For more on semi-space collectors, see
https://wingolog.org/archives/2022/12/10/a-simple-semi-space-collector.

Whippet's `semi` collector incorporates a large-object space, which
marks objects in place instead of moving.  Otherwise, `semi` generally
requires a heap size at least twice as large as the maximum live heap
size, and performs best with ample heap sizes; between 3× and 5× is
best.

The semi-space collector doesn't support multiple mutator threads.  If
you want a copying collector for a multi-threaded mutator, look at
[pcc](./collector-pcc.md).
