#ifndef GC_API_H_
#define GC_API_H_

#include "gc-config.h"
#include "gc-assert.h"
#include "gc-ref.h"
#include "gc-edge.h"

#include <stdint.h>

// FIXME: prefix with gc_
struct heap;
struct mutator;

enum {
  GC_OPTION_FIXED_HEAP_SIZE,
  GC_OPTION_PARALLELISM
};

struct gc_option {
  int option;
  double value;
};

// FIXME: Conflict with bdw-gc GC_API.  Switch prefix?
#ifndef GC_API_
#define GC_API_ static
#endif

GC_API_ int gc_option_from_string(const char *str);
GC_API_ int gc_init(int argc, struct gc_option argv[],
                    struct heap **heap, struct mutator **mutator);

GC_API_ struct mutator* gc_init_for_thread(uintptr_t *stack_base,
                                           struct heap *heap);
GC_API_ void gc_finish_for_thread(struct mutator *mut);
GC_API_ void* gc_call_without_gc(struct mutator *mut, void* (*f)(void*),
                                 void *data) GC_NEVER_INLINE;

GC_API_ inline void* gc_allocate(struct mutator *mut, size_t bytes);
// FIXME: remove :P
GC_API_ inline void* gc_allocate_pointerless(struct mutator *mut, size_t bytes);

#endif // GC_API_H_
