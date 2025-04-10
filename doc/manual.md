# Whippet user's guide

Whippet is an embed-only library: it should be copied into the source
tree of the program that uses it.  The program's build system needs to
be wired up to compile Whippet, then link it into the program that uses
it.

## Subtree merges

One way is get Whippet is just to manually copy the files present in a
Whippet checkout into your project.  However probably the best way is to
perform a [subtree
merge](https://docs.github.com/en/get-started/using-git/about-git-subtree-merges)
of Whippet into your project's Git repository, so that you can easily
update your copy of Whippet in the future.

Performing the first subtree merge is annoying and full of arcane
incantations.  Follow the [subtree merge
page](https://docs.github.com/en/get-started/using-git/about-git-subtree-merges)
for full details, but for a cheat sheet, you might do something like
this to copy Whippet into the `whippet/` directory of your project root:

```
git remote add whippet https://github.com/wingo/whippet
git fetch whippet
git merge -s ours --no-commit --allow-unrelated-histories whippet/main
git read-tree --prefix=whippet/ -u whippet/main
git commit -m 'Added initial Whippet merge'
```

Then to later update your copy of whippet, assuming you still have the
`whippet` remote, just do:

```
git pull -s subtree whippet main
```

## `gc-embedder-api.h`

To determine the live set of objects, a tracing garbage collector starts
with a set of root objects, and then transitively visits all reachable
object edges.  Exactly how it goes about doing this depends on the
program that is using the garbage collector; different programs will
have different object representations, different strategies for
recording roots, and so on.

To traverse the heap in a program-specific way but without imposing an
abstraction overhead, Whippet requires that a number of data types and
inline functions be implemented by the program, for use by Whippet
itself.  This is the *embedder API*, and this document describes what
Whippet requires from a program.

A program should provide a header file implementing the API in
[`gc-embedder-api.h`](../api/gc-embedder-api.h).  This header should only be
included when compiling Whippet itself; it is not part of the API that
Whippet exposes to the program.

### Identifying roots

The collector uses two opaque struct types, `struct gc_mutator_roots`
and `struct gc_heap_roots`, that are used by the program to record
object roots.  Probably you should put the definition of these data
types in a separate header that is included both by Whippet, via the
embedder API, and via users of Whippet, so that programs can populate
the root set.  In any case the embedder-API use of these structs is via
`gc_trace_mutator_roots` and `gc_trace_heap_roots`, two functions that
are passed a trace visitor function `trace_edge`, and which should call
that function on all edges from a given mutator or heap.  (Usually
mutator roots are per-thread roots, such as from the stack, and heap
roots are global roots.)

### Tracing objects

The `gc_trace_object` is responsible for calling the `trace_edge`
visitor function on all outgoing edges in an object.  It also includes a
`size` out-parameter, for when the collector wants to measure the size
of an object.  `trace_edge` and `size` may be `NULL`, in which case no
tracing or size computation should be performed.

### Tracing ephemerons and finalizers

Most kinds of GC-managed object are defined by the program, but the GC
itself has support for two specific object kind: ephemerons and
finalizers.  If the program allocates ephemerons, it should trace them
in the `gc_trace_object` function by calling `gc_trace_ephemeron` from
[`gc-ephemerons.h`](../api/gc-ephemerons.h).  Likewise if the program
allocates finalizers, it should trace them by calling
`gc_trace_finalizer` from [`gc-finalizer.h`](../api/gc-finalizer.h).

### Forwarding objects

When built with a collector that moves objects, the embedder must also
allow for forwarding pointers to be installed in an object.  There are
two forwarding APIs: one that is atomic and one that isn't.

The nonatomic API is relatively simple; there is a
`gc_object_forwarded_nonatomic` function that returns an embedded
forwarding address, or 0 if the object is not yet forwarded, and
`gc_object_forward_nonatomic`, which installs a forwarding pointer.

The atomic API is gnarly.  It is used by parallel collectors, in which
multiple collector threads can race to evacuate an object.

There is a state machine associated with the `gc_atomic_forward`
structure from [`gc-forwarding.h`](../api/gc-forwarding.h); the embedder API
implements the state changes.  The collector calls
`gc_atomic_forward_begin` on an object to begin a forwarding attempt,
and the resulting `gc_atomic_forward` can be in the `NOT_FORWARDED`,
`FORWARDED`, or `BUSY` state.

If the `gc_atomic_forward`'s state is `BUSY`, the collector will call
`gc_atomic_forward_retry_busy`; a return value of 0 means the object is
still busy, because another thread is attempting to forward it.
Otherwise the forwarding state becomes either `FORWARDED`, if the other
thread succeeded in forwarding it, or go back to `NOT_FORWARDED`,
indicating that the other thread failed to forward it.

If the forwarding state is `FORWARDED`, the collector will call
`gc_atomic_forward_address` to get the new address.

If the forwarding state is `NOT_FORWARDED`, the collector may begin a
forwarding attempt by calling `gc_atomic_forward_acquire`.  The
resulting state is `ACQUIRED` on success, or `BUSY` if another thread
acquired the object in the meantime, or `FORWARDED` if another thread
acquired and completed the forwarding attempt.

An `ACQUIRED` object can then be forwarded via
`gc_atomic_forward_commit`, or the forwarding attempt can be aborted via
`gc_atomic_forward_abort`.  Also, when an object is acquired, the
collector may call `gc_atomic_forward_object_size` to compute how many
bytes to copy.  (The collector may choose instead to record object sizes
in a different way.)

All of these `gc_atomic_forward` functions are to be implemented by the
embedder.  Some programs may allocate a dedicated forwarding word in all
objects; some will manage to store the forwarding word in an initial
"tag" word, via a specific pattern for the low 3 bits of the tag that no
non-forwarded object will have.  The low-bits approach takes advantage
of the collector's minimum object alignment, in which objects are
aligned at least to an 8-byte boundary, so all objects have 0 for the
low 3 bits of their address.

### Conservative references

Finally, when configured in a mode in which root edges or intra-object
edges are *conservative*, the embedder can filter out which bit patterns
might be an object reference by implementing
`gc_is_valid_conservative_ref_displacement`.  Here, the collector masks
off the low bits of a conservative reference, and asks the embedder if a
value with those low bits might point to an object.  Usually the
embedder should return 1 only if the displacement is 0, but if the
program allows low-bit tagged pointers, then it should also return 1 for
those pointer tags.

### External objects

Sometimes a system will allocate objects outside the GC, for example on
the stack or in static data sections.  To support this use case, Whippet
allows the embedder to provide a `struct gc_extern_space`
implementation.  Whippet will call `gc_extern_space_start_gc` at the
start of each collection, and `gc_extern_space_finish_gc` at the end.
External objects will be visited by `gc_extern_space_mark`, which should
return nonzero if the object hasn't been seen before and needs to be
traced via `gc_trace_object` (coloring the object grey).  Note,
`gc_extern_space_mark` may be called concurrently from many threads; be
prepared!

## Configuration, compilation, and linking

To the user, Whippet presents an abstract API that does not encode the
specificities of any given collector.  Whippet currently includes four
implementations of that API: `semi`, a simple semi-space collector;
`pcc`, a parallel copying collector (like semi but multithreaded);
`bdw`, an implementation via the third-party
[Boehm-Demers-Weiser](https://github.com/ivmai/bdwgc) conservative
collector; and `mmc`, a mostly-marking collector inspired by Immix.

The program that embeds Whippet selects the collector implementation at
build-time.  For `pcc`, the program can also choose whether to be
generational or not.  For `mmc` collector, the program configures a
specific collector mode, again at build-time: generational or not,
parallel or not, stack-conservative or not, and heap-conservative or
not.  It may be nice in the future to be able to configure these at
run-time, but for the time being they are compile-time options so that
adding new features doesn't change the footprint of a more minimal
collector.

Different collectors have different allocation strategies: for example,
the BDW collector allocates from thread-local freelists, whereas the
semi-space collector has a bump-pointer allocator.  A collector may also
expose a write barrier, for example to enable generational collection.
For performance reasons, many of these details can't be hidden behind an
opaque functional API: they must be inlined into call sites.  Whippet's
approach is to expose fast paths as part of its inline API, but which
are *parameterized* on attributes of the selected garbage collector.
The goal is to keep the user's code generic and avoid any code
dependency on the choice of garbage collector.  Because of inlining,
however, the choice of garbage collector does need to be specified when
compiling user code.

### Compiling the collector

As an embed-only library, Whippet needs to be integrated into the build
system of its host (embedder).  There are two build systems supported
currently; we would be happy to add other systems over time.

#### GNU make

At a high level, first the embedder chooses a collector and defines how
to specialize the collector against the embedder.  Whippet's `embed.mk`
Makefile snippet then defines how to build the set of object files that
define the collector, and how to specialize the embedder against the
chosen collector.

As an example, say you have a file `program.c`, and you want to compile
it against a Whippet checkout in `whippet/`.  Your headers are in
`include/`, and you have written an implementation of the embedder
interface in `host-gc.h`.  In that case you would have a Makefile like
this:

```
HOST_DIR:=$(dir $(lastword $(MAKEFILE_LIST)))
WHIPPET_DIR=$(HOST_DIR)whippet/

all: out

# The collector to choose: e.g. semi, bdw, pcc, generational-pcc, mmc,
# parallel-mmc, etc.
GC_COLLECTOR=pcc

include $(WHIPPET_DIR)embed.mk

# Host cflags go here...
HOST_CFLAGS=

# Whippet's embed.mk uses this variable when it compiles code that
# should be specialized against the embedder.
EMBEDDER_TO_GC_CFLAGS=$(HOST_CFLAGS) -include $(HOST_DIR)host-gc.h

program.o: program.c
	$(GC_COMPILE) $(HOST_CFLAGS) $(GC_TO_EMBEDDER_CFLAGS) -c $<
program: program.o $(GC_OBJS)
	$(GC_LINK) $^ $(GC_LIBS)
```

The optimization settings passed to the C compiler are taken from
`GC_BUILD_CFLAGS`.  Embedders can override this variable directly, or
via the shorthand `GC_BUILD` variable.  A `GC_BUILD` of `opt` indicates
maximum optimization and no debugging assertions; `optdebug` adds
debugging assertions; and `debug` removes optimizations.

Though Whippet tries to put performance-sensitive interfaces in header
files, users should also compile with link-time optimization (LTO) to
remove any overhead imposed by the division of code into separate
compilation units.  `embed.mk` includes the necessary LTO flags in
`GC_CFLAGS` and `GC_LDFLAGS`.

#### GNU Autotools

To use Whippet from an autotools project, the basic idea is to include a
`Makefile.am` snippet from the subdirectory containing the Whippet
checkout.  That will build `libwhippet.la`, which you should link into
your binary.  There are some `m4` autoconf macros that need to be
invoked, for example to select the collector.

Let us imagine you have checked out Whippet in `whippet/`.  Let us also
assume for the moment that we are going to build `mt-gcbench`, a program
included in Whippet itself.

A top-level autoconf file (`configure.ac`) might look like this:

```autoconf
AC_PREREQ([2.69])
AC_INIT([whippet-autotools-example],[0.1.0])
AC_CONFIG_SRCDIR([whippet/benchmarks/mt-gcbench.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4 whippet])
AM_INIT_AUTOMAKE([subdir-objects foreign])

WHIPPET_ENABLE_LTO

LT_INIT

WARN_CFLAGS=-Wall
AC_ARG_ENABLE([Werror],
  AS_HELP_STRING([--disable-Werror],
                 [Don't stop the build on errors]),
  [],
  WARN_CFLAGS="-Wall -Werror")
CFLAGS="$CFLAGS $WARN_CFLAGS"

WHIPPET_PKG

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
```

Then your `Makefile.am` might look like this:

```automake
noinst_LTLIBRARIES =
WHIPPET_EMBEDDER_CPPFLAGS = -include $(srcdir)/whippet/benchmarks/mt-gcbench-embedder.h
include whippet/embed.am

noinst_PROGRAMS = whippet/benchmarks/mt-gcbench
whippet_benchmarks_mt_gcbench_SOURCES = \
  whippet/benchmarks/mt-gcbench.c \
  whippet/benchmarks/mt-gcbench-types.h \
  whippet/benchmarks/simple-tagging-scheme.h \
  whippet/benchmarks/simple-roots-api.h \
  whippet/benchmarks/simple-roots-types.h \
  whippet/benchmarks/simple-allocator.h \
  whippet/benchmarks/heap-objects.h \
  whippet/benchmarks/mt-gcbench-embedder.h

AM_CFLAGS = $(WHIPPET_CPPFLAGS) $(WHIPPET_CFLAGS) $(WHIPPET_TO_EMBEDDER_CPPFLAGS)
LDADD = libwhippet.la
```

We have to list all the little header files it uses because, well,
autotools.

To actually build, you do the usual autotools dance:

```bash
autoreconf -vif && ./configure && make
```

See `./configure --help` for a list of user-facing options.  Before the
`WHIPPET_PKG`, you can run e.g. `WHIPPET_PKG_COLLECTOR(mmc)` to set the
default collector to `mmc`; if you don't do that, the default collector
is `pcc`.  There are also `WHIPPET_PKG_DEBUG`, `WHIPPET_PKG_TRACING`,
and `WHIPPET_PKG_PLATFORM`; see `whippet.m4` for more details.

#### Compile-time options

There are a number of pre-processor definitions that can parameterize
the collector at build-time:

 * `GC_DEBUG`: If nonzero, then enable debugging assertions.
 * `NDEBUG`: This one is a bit weird; if not defined, then enable
   debugging assertions and some debugging printouts.  Probably
   Whippet's use of `NDEBUG` should be folded in to `GC_DEBUG`.
 * `GC_PARALLEL`: If nonzero, then enable parallelism in the collector.
   Defaults to 0.
 * `GC_GENERATIONAL`: If nonzero, then enable generational collection.
   Defaults to zero.
 * `GC_PRECISE_ROOTS`: If nonzero, then collect precise roots via
   `gc_heap_roots` and `gc_mutator_roots`.  Defaults to zero.
 * `GC_CONSERVATIVE_ROOTS`: If nonzero, then scan the stack and static
   data sections for conservative roots.  Defaults to zero.  Not
   mutually exclusive with `GC_PRECISE_ROOTS`.
 * `GC_CONSERVATIVE_TRACE`: If nonzero, heap edges are scanned
   conservatively.  Defaults to zero.

Some collectors require specific compile-time options.  For example, the
semi-space collector has to be able to move all objects; this is not
compatible with conservative roots or heap edges.

#### Tracing support

Whippet includes support for low-overhead run-time tracing via
[LTTng](https://lttng.org/).  If the support library `lttng-ust` is
present when Whippet is compiled (as checked via `pkg-config`),
tracepoint support will be present.  See
[tracepoints.md](./tracepoints.md) for more information on how to get
performance traces out of Whippet.

## Using the collector

Whew!  So you finally built the thing!  Did you also link it into your
program?  No, because your program isn't written yet?  Well this section
is for you: we describe the user-facing API of Whippet, where "user" in
this case denotes the embedding program.

What is the API, you ask?  It is in [`gc-api.h`](../api/gc-api.h).

### Heaps and mutators

To start with, you create a *heap*.  Usually an application will create
just one heap.  A heap has one or more associated *mutators*.  A mutator
is a thread-specific handle on the heap.  Allocating objects requires a
mutator.

The initial heap and mutator are created via `gc_init`, which takes
three logical input parameters: the *options*, a stack base address, and
an *event listener*.  The options specify the initial heap size and so
on.  The event listener is mostly for gathering statistics; see below
for more.  `gc_init` returns the new heap as an out parameter, and also
returns a mutator for the current thread.

To make a new mutator for a new thread, use `gc_init_for_thread`.  When
a thread is finished with its mutator, call `gc_finish_for_thread`.
Each thread that allocates or accesses GC-managed objects should have
its own mutator.

The stack base address allows the collector to scan the mutator's stack,
if conservative root-finding is enabled.  It may be omitted in the call
to `gc_init` and `gc_init_for_thread`; passing `NULL` tells Whippet to
ask the platform for the stack bounds of the current thread.  Generally
speaking, this works on all platforms for the main thread, but not
necessarily on other threads.  The most reliable solution is to
explicitly obtain a base address by trampolining through
`gc_call_with_stack_addr`.

### Options

There are some run-time parameters that programs and users might want to
set explicitly; these are encapsulated in the *options*.  Make an
options object with `gc_allocate_options()`; this object will be
consumed by its `gc_init`.  Then, the most convenient thing is to set
those options from `gc_options_parse_and_set_many` from a string passed
on the command line or an environment variable, but to get there we have
to explain the low-level first.  There are a few options that are
defined for all collectors:

 * `GC_OPTION_HEAP_SIZE_POLICY`: How should we size the heap?  Either
   it's `GC_HEAP_SIZE_FIXED` (which is 0), in which the heap size is
   fixed at startup; or `GC_HEAP_SIZE_GROWABLE` (1), in which the heap
   may grow but will never shrink; or `GC_HEAP_SIZE_ADAPTIVE` (2), in
   which we take an
   [adaptive](https://wingolog.org/archives/2023/01/27/three-approaches-to-heap-sizing)
   approach, depending on the rate of allocation and the cost of
   collection.  Really you want the adaptive strategy, but if you are
   benchmarking you definitely want the fixed policy.
 * `GC_OPTION_HEAP_SIZE`: The initial heap size.  For a
   `GC_HEAP_SIZE_FIXED` policy, this is also the final heap size.  In
   bytes.
 * `GC_OPTION_MAXIMUM_HEAP_SIZE`: For growable and adaptive heaps, the
   maximum heap size, in bytes.
 * `GC_OPTION_HEAP_SIZE_MULTIPLIER`: For growable heaps, the target heap
   multiplier.  A heap multiplier of 2.5 means that for 100 MB of live
   data, the heap should be 250 MB.
 * `GC_OPTION_HEAP_EXPANSIVENESS`: For adaptive heap sizing, an
   indication of how much free space will be given to heaps, as a
   proportion of the square root of the live data size.
 * `GC_OPTION_PARALLELISM`: How many threads to devote to collection
   tasks during GC pauses.  By default, the current number of
   processors, with a maximum of 8.

You can set these options via `gc_option_set_int` and so on; see
[`gc-options.h`](../api/gc-options.h).  Or, you can parse options from
strings: `heap-size-policy`, `heap-size`, `maximum-heap-size`, and so
on.  Use `gc_option_from_string` to determine if a string is really an
option.  Use `gc_option_parse_and_set` to parse a value for an option.
Use `gc_options_parse_and_set_many` to parse a number of comma-delimited
*key=value* settings from a string.

### Allocation

So you have a heap and a mutator; great!  Let's allocate!  Call
`gc_allocate`, passing the mutator and the number of bytes to allocate.

There is also `gc_allocate_fast`, which is an inlined fast-path.  If
that returns NULL, you need to call `gc_allocate_slow`.  The advantage
of this API is that you can punt some root-saving overhead to the slow
path.

Allocation always succeeds.  If it doesn't, it kills your program.  The
bytes in the resulting allocation will be initialized to 0.

The allocation fast path is parameterized by collector-specific
attributes.  JIT compilers can also read those attributes to emit
appropriate inline code that replicates the logic of `gc_allocate_fast`.

### Write barriers

For some collectors, mutators have to tell the collector whenever they
mutate an object.  They tell the collector by calling a *write barrier*;
in Whippet this is currently the case only for generational collectors.

The write barrier is `gc_write_barrier`; see `gc-api.h` for its
parameters.

As with allocation, the fast path for the write barrier is parameterized
by collector-specific attributes, to allow JIT compilers to inline write
barriers.

### Safepoints

Sometimes Whippet will need to synchronize all threads, for example as
part of the "stop" phase of a stop-and-copy semi-space collector.
Whippet stops at *safepoints*.  At a safepoint, all mutators must be
able to enumerate all of their edges to live objects.

Whippet has cooperative safepoints: mutators have to periodically call
into the collector to potentially synchronize with other mutators.
`gc_allocate_slow` is a safepoint, so if you a bunch of threads that are
all allocating, usually safepoints are reached in a more-or-less prompt
fashion.  But if a mutator isn't allocating, it either needs to
temporarily mark itself as inactive by trampolining through
`gc_call_without_gc`, or it should arrange to periodically call
`gc_safepoint`.  Marking a mutator as inactive is the right strategy
for, for example, system calls that might block.  Periodic safepoints is
better for code that is active but not allocating.

Also, the BDW collector actually uses pre-emptive safepoints: it stops
threads via POSIX signals.  `gc_safepoint` is a no-op with BDW.

Embedders can inline safepoint checks.  If
`gc_cooperative_safepoint_kind()` is `GC_COOPERATIVE_SAFEPOINT_NONE`,
then the collector doesn't need safepoints, as is the case for `bdw`
which uses signals and `semi` which is single-threaded.  If it is
`GC_COOPERATIVE_SAFEPOINT_HEAP_FLAG`, then calling
`gc_safepoint_flag_loc` on a mutator will return the address of an `int`
in memory, which if nonzero when loaded using relaxed atomics indicates
that the mutator should call `gc_safepoint_slow`.  Similarly for
`GC_COOPERATIVE_SAFEPOINT_MUTATOR_FLAG`, except that the address is
per-mutator rather than global.

### Pinning

Sometimes a mutator or embedder would like to tell the collector to not
move a particular object.  This can happen for example during a foreign
function call, or if the embedder allows programs to access the address
of an object, for example to compute an identity hash code.  To support
this use case, some Whippet collectors allow the embedder to *pin*
objects.  Call `gc_pin_object` to prevent the collector from relocating
an object.

Pinning is currently supported by the `bdw` collector, which never moves
objects, and also by the various `mmc` collectors, which can move
objects that have no inbound conservative references.

Pinning is not supported on `semi` or `pcc`.

Call `gc_can_pin_objects` to determine whether the current collector can
pin objects.

### Statistics

Sometimes a program would like some information from the GC: how many
bytes and objects have been allocated?  How much time has been spent in
the GC?  How many times has GC run, and how many of those were minor
collections?  What's the maximum pause time?  Stuff like that.

Instead of collecting a fixed set of information, Whippet emits
callbacks when the collector reaches specific states.  The embedder
provides a *listener* for these events when initializing the collector.

The listener interface is defined in
[`gc-event-listener.h`](../api/gc-event-listener.h).  Whippet ships with
two listener implementations,
[`GC_NULL_EVENT_LISTENER`](../api/gc-null-event-listener.h), and
[`GC_BASIC_STATS`](../api/gc-basic-stats.h).  Most embedders will want
their own listener, but starting with the basic stats listener is not a
bad option:

```
#include "gc-api.h"
#include "gc-basic-stats.h"
#include <stdio.h>

int main() {
  struct gc_options *options = NULL;
  struct gc_heap *heap;
  struct gc_mutator *mut;
  struct gc_basic_stats stats;
  gc_init(options, NULL, &heap, &mut, GC_BASIC_STATS, &stats);
  // ...
  gc_basic_stats_finish(&stats);
  gc_basic_stats_print(&stats, stdout);
}
```

As you can see, `GC_BASIC_STATS` expands to a `struct gc_event_listener`
definition.  We pass an associated pointer to a `struct gc_basic_stats`
instance which will be passed to the listener at every event.

The output of this program might be something like:

```
Completed 19 major collections (0 minor).
654.597 ms total time (385.235 stopped).
Heap size is 167.772 MB (max 167.772 MB); peak live data 55.925 MB.
```

There are currently three different sorts of events: heap events to
track heap growth, collector events to time different parts of
collection, and mutator events to indicate when specific mutators are
stopped.

There are three heap events:

 * `init(void* data, size_t heap_size)`: Called during `gc_init`, to
   allow the listener to initialize its associated state.
 * `heap_resized(void* data, size_t new_size)`: Called if the heap grows
   or shrinks.
 * `live_data_size(void* data, size_t size)`: Called periodically when
   the collector learns about live data size.
 
The collection events form a kind of state machine, and are called in
this order:

 * `requesting_stop(void* data)`: Called when the collector asks
   mutators to stop.
 * `waiting_for_stop(void* data)`: Called when the collector has done
   all the pre-stop work that it is able to and is just waiting on
   mutators to stop.
 * `mutators_stopped(void* data)`: Called when all mutators have
   stopped; the trace phase follows.
 * `prepare_gc(void* data, enum gc_collection_kind gc_kind)`: Called 
   to indicate which kind of collection is happening.
 * `roots_traced(void* data)`: Called when roots have been visited.
 * `heap_traced(void* data)`: Called when the whole heap has been
   traced.
 * `ephemerons_traced(void* data)`: Called when the [ephemeron
   fixpoint](https://wingolog.org/archives/2023/01/24/parallel-ephemeron-tracing)
   has been reached.
 * `restarting_mutators(void* data)`: Called right before the collector
   restarts mutators.

The collectors in Whippet will call all of these event handlers, but it
may be that they are called conservatively: for example, the
single-mutator, single-collector semi-space collector will never have to
wait for mutators to stop.  It will still call the functions, though!

Finally, there are the mutator events:
 * `mutator_added(void* data) -> void*`: The only event handler that
   returns a value, called when a new mutator is added.  The parameter
   is the overall event listener data, and the result is
   mutator-specific data.  The rest of the mutator events pass this
   mutator-specific data instead.
 * `mutator_cause_gc(void* mutator_data)`: Called when a mutator causes
   GC, either via allocation or an explicit `gc_collect` call.
 * `mutator_stopping(void* mutator_data)`: Called when a mutator has
   received the signal to stop.  It may perform some marking work before
   it stops.
 * `mutator_stopped(void* mutator_data)`: Called when a mutator parks
   itself.
 * `mutator_restarted(void* mutator_data)`: Called when a mutator
   restarts.
 * `mutator_removed(void* mutator_data)`: Called when a mutator goes
   away.

Note that these events handlers shouldn't really do much.  In
particular, they shouldn't call into the Whippet API, and they shouldn't
even access GC-managed objects.  Event listeners are really about
statistics and profiling and aren't a place to mutate the object graph.

### Ephemerons

Whippet supports ephemerons, first-class objects that weakly associate
keys with values.  If the an ephemeron's key ever becomes unreachable,
the ephemeron becomes dead and loses its value.

The user-facing API is in [`gc-ephemeron.h`](../api/gc-ephemeron.h).  To
allocate an ephemeron, call `gc_allocate_ephemeron`, then initialize its
key and value via `gc_ephemeron_init`.  Get the key and value via
`gc_ephemeron_key` and `gc_ephemeron_value`, respectively.

In Whippet, ephemerons can be linked together in a chain.  During GC, if
an ephemeron's chain points to a dead ephemeron, that link will be
elided, allowing the dead ephemeron itself to be collected.  In that
way, ephemerons can be used to build weak data structures such as weak
maps.

Weak data structures are often shared across multiple threads, so all
routines to access and modify chain links are atomic.  Use
`gc_ephemeron_chain_head` to access the head of a storage location that
points to an ephemeron; push a new ephemeron on a location with
`gc_ephemeron_chain_push`; and traverse a chain with
`gc_ephemeron_chain_next`.

An ephemeron association can be removed via `gc_ephemeron_mark_dead`.

### Finalizers

A finalizer allows the embedder to be notified when an object becomes
unreachable.

A finalizer has a priority.  When the heap is created, the embedder
should declare how many priorities there are.  Lower-numbered priorities
take precedence; if an object has a priority-0 finalizer outstanding,
that will prevent any finalizer at level 1 (or 2, ...)  from firing
until no priority-0 finalizer remains.

Call `gc_attach_finalizer`, from `gc-finalizer.h`, to attach a finalizer
to an object.

A finalizer also references an associated GC-managed closure object.
A finalizer's reference to the closure object is strong:  if a
finalizer's closure closure references its finalizable object,
directly or indirectly, the finalizer will never fire.

When an object with a finalizer becomes unreachable, it is added to a
queue.  The embedder can call `gc_pop_finalizable` to get the next
finalizable object and its associated closure.  At that point the
embedder can do anything with the object, including keeping it alive.
Ephemeron associations will still be present while the finalizable
object is live.  Note however that any objects referenced by the
finalizable object may themselves be already finalized; finalizers are
enqueued for objects when they become unreachable, which can concern
whole subgraphs of objects at once.

The usual way for an embedder to know when the queue of finalizable
object is non-empty is to call `gc_set_finalizer_callback` to
provide a function that will be invoked when there are pending
finalizers.

Arranging to call `gc_pop_finalizable` and doing something with the
finalizable object and closure is the responsibility of the embedder.
The embedder's finalization action can end up invoking arbitrary code,
so unless the embedder imposes some kind of restriction on what
finalizers can do, generally speaking finalizers should be run in a
dedicated thread instead of recursively from within whatever mutator
thread caused GC.  Setting up such a thread is the responsibility of the
mutator.  `gc_pop_finalizable` is thread-safe, allowing multiple
finalization threads if that is appropriate.

`gc_allocate_finalizer` returns a finalizer, which is a fresh GC-managed
heap object.  The mutator should then directly attach it to an object
using `gc_finalizer_attach`.  When the finalizer is fired, it becomes
available to the mutator via `gc_pop_finalizable`.
