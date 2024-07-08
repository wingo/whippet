#ifndef LOCAL_WORKLIST_H
#define LOCAL_WORKLIST_H

#include "assert.h"

#define LOCAL_WORKLIST_SIZE 1024
#define LOCAL_WORKLIST_MASK (LOCAL_WORKLIST_SIZE - 1)
#define LOCAL_WORKLIST_SHARE_AMOUNT (LOCAL_WORKLIST_SIZE * 3 / 4)
struct local_worklist {
  size_t read;
  size_t write;
  struct gc_ref data[LOCAL_WORKLIST_SIZE];
};

static inline void
local_worklist_init(struct local_worklist *q) {
  q->read = q->write = 0;
}
static inline void
local_worklist_poison(struct local_worklist *q) {
  q->read = 0; q->write = LOCAL_WORKLIST_SIZE;
}
static inline size_t
local_worklist_size(struct local_worklist *q) {
  return q->write - q->read;
}
static inline int
local_worklist_empty(struct local_worklist *q) {
  return local_worklist_size(q) == 0;
}
static inline int
local_worklist_full(struct local_worklist *q) {
  return local_worklist_size(q) >= LOCAL_WORKLIST_SIZE;
}
static inline void
local_worklist_push(struct local_worklist *q, struct gc_ref v) {
  ASSERT(!local_worklist_full(q));
  q->data[q->write++ & LOCAL_WORKLIST_MASK] = v;
}
static inline struct gc_ref
local_worklist_pop(struct local_worklist *q) {
  ASSERT(!local_worklist_empty(q));
  return q->data[q->read++ & LOCAL_WORKLIST_MASK];
}

static inline size_t
local_worklist_pop_many(struct local_worklist *q, struct gc_ref **objv,
                        size_t limit) {
  size_t avail = local_worklist_size(q);
  size_t read = q->read & LOCAL_WORKLIST_MASK;
  size_t contig = LOCAL_WORKLIST_SIZE - read;
  if (contig < avail) avail = contig;
  if (limit < avail) avail = limit;
  *objv = q->data + read;
  q->read += avail;
  return avail;
}

#endif // LOCAL_WORKLIST_H
