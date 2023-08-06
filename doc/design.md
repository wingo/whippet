# Design

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
