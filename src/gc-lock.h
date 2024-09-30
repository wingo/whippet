#ifndef GC_LOCK_H
#define GC_LOCK_H

#include <pthread.h>
#include "gc-assert.h"

struct gc_lock {
  pthread_mutex_t *lock;
};

static struct gc_lock
gc_lock_acquire(pthread_mutex_t *lock) {
  pthread_mutex_lock(lock);
  return (struct gc_lock){ lock };
}

static void
gc_lock_release(struct gc_lock *lock) {
  GC_ASSERT(lock->lock);
  pthread_mutex_unlock(lock->lock);
  lock->lock = NULL;
}

#endif // GC_LOCK_H
