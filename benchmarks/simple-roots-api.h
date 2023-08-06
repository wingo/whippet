#ifndef SIMPLE_ROOTS_API_H
#define SIMPLE_ROOTS_API_H

#include "gc-config.h"
#include "simple-roots-types.h"

#define HANDLE_TO(T) union { T* v; struct handle handle; }
#define HANDLE_LOC(h) &(h).v
#define HANDLE_REF(h) (h).v
#define HANDLE_SET(h,val) do { (h).v = val; } while (0)
#define PUSH_HANDLE(cx, h) push_handle(&(cx)->roots.roots, &h.handle)
#define POP_HANDLE(cx) pop_handle(&(cx)->roots.roots)

static inline void push_handle(struct handle **roots, struct handle *handle) {
  if (GC_PRECISE_ROOTS) {
    handle->next = *roots;
    *roots = handle;
  }
}

static inline void pop_handle(struct handle **roots) {
  if (GC_PRECISE_ROOTS)
    *roots = (*roots)->next;
}

#endif // SIMPLE_ROOTS_API_H
