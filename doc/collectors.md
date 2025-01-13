# Whippet collectors

Whippet has four collectors currently:
 - [Semi-space collector (`semi`)](./collector-semi.md): For
   single-threaded embedders who are not too tight on memory.
 - [Parallel copying collector (`pcc`)](./collector-pcc.md): Like
   `semi`, but with support for multiple mutator and tracing threads and
   generational collection.
 - [Mostly marking collector (`mmc`)](./collector-mmc.md):
   Immix-inspired collector.  Optionally parallel, conservative (stack
   and/or heap), and/or generational.
 - [Boehm-Demers-Weiser collector (`bdw`)](./collector-bdw.md):
   Conservative mark-sweep collector, implemented by
   Boehm-Demers-Weiser library.

## How to choose?

If you are migrating an embedder off BDW-GC, then it could be reasonable
to first go to `bdw`, then `stack-conservative-parallel-mmc`.

If you have an embedder with precise roots, use `pcc`.  That will shake
out mutator/embedder bugs.  Then if memory is tight, switch to
`parallel-mmc`, possibly `parallel-generational-mmc`.

If you are aiming for maximum simplicity and minimal code size (ten
kilobytes or so), use `semi`.

If you are writing a new project, you have a choice as to whether to pay
the development cost of precise roots or not.  If you choose to not have
precise roots, then go for `stack-conservative-parallel-mmc` directly.

## More collectors

It would be nice to have a generational GC that uses the space from
`parallel-mmc` for the old generation but a pcc-style copying nursery.
We have `generational-pcc` now, so this should be possible.

Support for concurrent marking in `mmc` would be good as well, perhaps
with a SATB barrier.  (Or, if you are the sort of person to bet on
conservative stack scanning, perhaps a retreating-wavefront barrier
would be more appropriate.)

Contributions are welcome, provided they have no more dependencies!
