#ifndef GC_FINALIZER_H_
#define GC_FINALIZER_H_

#include "gc-edge.h"
#include "gc-ref.h"
#include "gc-visibility.h"

// A finalizer allows the embedder to be notified when an object becomes
// unreachable.
//
// A finalizer has a priority.  When the heap is created, the embedder
// should declare how many priorities there are.  Lower-numbered
// priorities take precedence; if an object has a priority-0 finalizer
// outstanding, that will prevent any finalizer at level 1 (or 2, ...)
// from firing until no priority-0 finalizer remains.
//
// Call gc_attach_finalizer to attach a finalizer to an object.
//
// A finalizer also references an associated GC-managed closure object.
// A finalizer's reference to the closure object is strong:  if a
// finalizer's closure closure references its finalizable object,
// directly or indirectly, the finalizer will never fire.
//
// When an object with a finalizer becomes unreachable, it is added to a
// queue.  The embedder can call gc_pop_finalizable to get the next
// finalizable object and its associated closure.  At that point the
// embedder can do anything with the object, including keeping it alive.
// Ephemeron associations will still be present while the finalizable
// object is live.  Note however that any objects referenced by the
// finalizable object may themselves be already finalized; finalizers
// are enqueued for objects when they become unreachable, which can
// concern whole subgraphs of objects at once.
//
// The usual way for an embedder to know when the queue of finalizable
// object is non-empty is to call gc_set_finalizer_callback to
// provide a function that will be invoked when there are pending
// finalizers.
//
// Arranging to call gc_pop_finalizable and doing something with the
// finalizable object and closure is the responsibility of the embedder.
// The embedder's finalization action can end up invoking arbitrary
// code, so unless the embedder imposes some kind of restriction on what
// finalizers can do, generally speaking finalizers should be run in a
// dedicated thread instead of recursively from within whatever mutator
// thread caused GC.  Setting up such a thread is the responsibility of
// the mutator.  gc_pop_finalizable is thread-safe, allowing multiple
// finalization threads if that is appropriate.
//
// gc_allocate_finalizer returns a finalizer, which is a fresh
// GC-managed heap object.  The mutator should then directly attach it
// to an object using gc_finalizer_attach.  When the finalizer is fired,
// it becomes available to the mutator via gc_pop_finalizable.

struct gc_heap;
struct gc_mutator;
struct gc_finalizer;

GC_API_ size_t gc_finalizer_size(void);
GC_API_ struct gc_finalizer* gc_allocate_finalizer(struct gc_mutator *mut);
GC_API_ void gc_finalizer_attach(struct gc_mutator *mut,
                                 struct gc_finalizer *finalizer,
                                 unsigned priority,
                                 struct gc_ref object, struct gc_ref closure);

GC_API_ struct gc_ref gc_finalizer_object(struct gc_finalizer *finalizer);
GC_API_ struct gc_ref gc_finalizer_closure(struct gc_finalizer *finalizer);

GC_API_ struct gc_finalizer* gc_pop_finalizable(struct gc_mutator *mut);

typedef void (*gc_finalizer_callback)(struct gc_heap *heap, size_t count);
GC_API_ void gc_set_finalizer_callback(struct gc_heap *heap,
                                       gc_finalizer_callback callback);

GC_API_ void gc_trace_finalizer(struct gc_finalizer *finalizer,
                                void (*visit)(struct gc_edge edge,
                                              struct gc_heap *heap,
                                              void *visit_data),
                                struct gc_heap *heap,
                                void *trace_data);

#endif // GC_FINALIZER_H_
