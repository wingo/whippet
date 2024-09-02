# Whippet and Guile

If the `mmc` collector works out, it could replace Guile's garbage
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
