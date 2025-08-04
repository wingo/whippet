#ifndef NOFL_HOLESET_H
#define NOFL_HOLESET_H

#include <math.h>
#include <pthread.h>

#include "gc-attrs.h"
#include "gc-align.h"
#include "gc-ref.h"
#include "gc-inline.h"
#include "gc-lock.h"

struct nofl_hole {
  struct nofl_hole *next;
  struct nofl_hole *next_chain;
};

struct nofl_hole_with_size {
  struct nofl_hole entry;
  size_t granules;
};

struct nofl_holeset {
  uint64_t nonempty;
  struct nofl_hole *buckets[64];
};

#define NOFL_HOLESET_PACKED_GRANULES 50
#define NOFL_HOLESET_SPARSE_COUNT (64 - NOFL_HOLESET_PACKED_GRANULES - 1)
#define NOFL_HOLESET_MAX_GRANULES 512

static inline void
nofl_hole_push(struct nofl_hole **loc, struct nofl_hole *hole) {
  GC_ASSERT_EQ(hole->next, NULL);
  GC_ASSERT_EQ(hole->next_chain, NULL);
  hole->next = *loc;
  *loc = hole;
}

static inline void*
nofl_hole_pop(struct nofl_hole **loc) {
  GC_ASSERT(*loc);
  struct nofl_hole *hole = *loc;
  GC_ASSERT_EQ(hole->next_chain, NULL);
  *loc = hole->next;
  hole->next = NULL;
  return hole;
}

static inline size_t
nofl_holeset_sparse_bucket(size_t granules, int for_insert) {
  GC_ASSERT(granules >= NOFL_HOLESET_PACKED_GRANULES);
  size_t idx = NOFL_HOLESET_PACKED_GRANULES;
  size_t num = granules - NOFL_HOLESET_PACKED_GRANULES;
  size_t denom = NOFL_HOLESET_MAX_GRANULES - NOFL_HOLESET_PACKED_GRANULES;
  double frac = (double) (num * NOFL_HOLESET_SPARSE_COUNT) / denom;
  idx += fmin(for_insert ? floor(frac) : ceil(frac),
              NOFL_HOLESET_SPARSE_COUNT);
  GC_ASSERT(idx < 64);
  return idx;
}

static inline size_t
nofl_holeset_bucket_for_lookup(size_t granules) {
  return granules <= NOFL_HOLESET_PACKED_GRANULES
    ? granules - 1
    : nofl_holeset_sparse_bucket(granules, 0);
}

static inline void
nofl_holeset_push_local(struct nofl_holeset *local, uintptr_t addr,
                        size_t granules) {
  size_t idx;
  struct nofl_hole *hole = (struct nofl_hole *) addr;
  if (granules <= NOFL_HOLESET_PACKED_GRANULES) {
    GC_ASSERT(granules > 0);
    idx = granules - 1;
  } else {
    struct nofl_hole_with_size *hole_with_size =
      (struct nofl_hole_with_size *) hole;
    GC_ASSERT_EQ(hole_with_size->granules, 0);
    hole_with_size->granules = granules;
    idx = nofl_holeset_sparse_bucket(granules, 1);
  }

  nofl_hole_push(&local->buckets[idx], hole);
  local->nonempty |= ((uint64_t) 1) << idx;
}

static inline uintptr_t
nofl_hole_tail_address(void *hole, size_t granules)
{
  return ((uintptr_t) hole) + granules * NOFL_GRANULE_SIZE;
}

static inline struct gc_ref
nofl_holeset_try_pop(struct nofl_holeset *holes, size_t granules) {
  GC_ASSERT(granules <= NOFL_HOLESET_MAX_GRANULES);
  size_t min_idx = nofl_holeset_bucket_for_lookup(granules);

  uint64_t candidates = holes->nonempty >> min_idx;
  if (!candidates)
    return gc_ref_null();

  size_t idx = min_idx + __builtin_ctzll(candidates);
  void *ret = nofl_hole_pop(&holes->buckets[idx]);
  
  GC_ASSERT(holes->nonempty & (((uint64_t)1) << idx));
  if (!holes->buckets[idx])
    holes->nonempty ^= ((uint64_t)1) << idx;

  size_t hole_granules;
  if (idx < NOFL_HOLESET_PACKED_GRANULES) {
    hole_granules = idx + 1;
  } else {
    struct nofl_hole_with_size *hole_with_size = ret;
    hole_granules = hole_with_size->granules;
    hole_with_size->granules = 0;
  }

  GC_ASSERT(hole_granules >= granules);
  size_t tail_granules = hole_granules - granules;
  if (tail_granules)
    nofl_holeset_push_local(holes, nofl_hole_tail_address(ret, granules),
                            tail_granules);

  return gc_ref_from_heap_object(ret);
}

static inline void
nofl_holeset_release(struct nofl_holeset *local,
                     struct nofl_holeset *remote,
                     pthread_mutex_t *lock) {
  uint64_t nonempty = local->nonempty;
  if (!nonempty)
    return;
  
  local->nonempty = 0;

  pthread_mutex_lock(lock);
  remote->nonempty |= nonempty;
  do {
    uint64_t idx = __builtin_ctzll(nonempty);
    struct nofl_hole *hole = local->buckets[idx];
    local->buckets[idx] = NULL;
    hole->next_chain = remote->buckets[idx];
    remote->buckets[idx] = hole;
    nonempty ^= ((uint64_t)1) << idx;
  } while (nonempty);
  pthread_mutex_unlock(lock);
}

static inline int
nofl_holeset_acquire(struct nofl_holeset *local,
                     struct nofl_holeset *remote,
                     pthread_mutex_t *lock,
                     size_t granules) {
  size_t min_idx = nofl_holeset_bucket_for_lookup(granules);
  
  uint64_t sloppy_nonempty =
    atomic_load_explicit(&remote->nonempty, memory_order_relaxed);
  if (!(sloppy_nonempty >> min_idx))
    return 0;

  pthread_mutex_lock(lock);
  uint64_t nonempty = remote->nonempty >> min_idx;
  if (!nonempty) {
    pthread_mutex_unlock(lock);
    return 0;
  }
  uint64_t idx = min_idx + __builtin_ctzll(nonempty);
  GC_ASSERT(!local->buckets[idx]);
  struct nofl_hole *chain = remote->buckets[idx];
  remote->buckets[idx] = chain->next_chain;
  if (!chain->next_chain)
    remote->nonempty ^= ((uint64_t)1) << idx;
  pthread_mutex_unlock(lock);

  local->buckets[idx] = chain;
  local->nonempty |= ((uint64_t)1) << idx;
  chain->next_chain = NULL;
  return 1;
}

static inline void
nofl_holeset_clear(struct nofl_holeset *holes)
{
  memset(holes, 0, sizeof (*holes));
}

#endif // NOFL_HOLESET_H
