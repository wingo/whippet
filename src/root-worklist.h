#ifndef ROOT_WORKLIST_H
#define ROOT_WORKLIST_H

#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "assert.h"
#include "debug.h"
#include "gc-inline.h"
#include "gc-ref.h"
#include "root.h"

// A single-producer, multiple-consumer worklist that has two phases:
// one in which roots are added by the producer, then one in which roots
// are consumed from the worklist.  Roots are never added once the
// consumer phase starts.
struct root_worklist {
  size_t size;
  size_t read;
  size_t write;
  struct gc_root *buf;
};

void
root_worklist_alloc(struct root_worklist *q) {
  q->buf = realloc(q->buf, q->size * sizeof(struct gc_root));
  if (!q->buf) {
    perror("Failed to grow root worklist");
    GC_CRASH();
  }
}

static void
root_worklist_init(struct root_worklist *q) {
  q->size = 16;
  q->read = 0;
  q->write = 0;
  q->buf = NULL;
  root_worklist_alloc(q);
}

static inline void
root_worklist_push(struct root_worklist *q, struct gc_root root) {
  if (UNLIKELY(q->write == q->size)) {
    q->size *= 2;
    root_worklist_alloc(q);
  }
  q->buf[q->write++] = root;
}

// Not atomic.
static inline size_t
root_worklist_size(struct root_worklist *q) {
  return q->write - q->read;
}

static inline struct gc_root
root_worklist_pop(struct root_worklist *q) {
  size_t idx = atomic_fetch_add(&q->read, 1);
  if (idx < q->write)
    return q->buf[idx];
  return (struct gc_root){ GC_ROOT_KIND_NONE, };
}

static void
root_worklist_reset(struct root_worklist *q) {
  q->read = q->write = 0;
}

static void
root_worklist_destroy(struct root_worklist *q) {
  free(q->buf);
}

#endif // ROOT_WORKLIST_H
