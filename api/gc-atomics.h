#ifndef GC_ATOMICS_H
#define GC_ATOMICS_H

#include <stdatomic.h>

// This file defines some wrappers around C11 atomic operations.  There
// are two goals:
//
//  1. Default to acquire/release memory ordering, and to make other
//     memory orderings explicit.
//
//  2. Denote the access as atomic, not the object being accessed.  In
//     practice this means that users of this interface don't often use
//     the _Atomic type qualifier or the atomic_int types for the
//     accessed objects.
//
// The second goal is gnarly.  GCC doesn't require _Atomic qualifiers,
// which means gc_atomic_load(LOC) can directly translate to
// atomic_load_explicit(LOC, memory_order_acquire), without regard to
// the type of LOC.  But this is nonstandard.  To do things in a
// standards-compliant way, we'd need to use _Generic maybe, but as far
// as I can tell, there is no way to turn e.g. struct my_foo ** into
// _Atomic(struct my_foo*)*.  (My kingdom for generics!)
//
// So instead we are going to bottom out to the __atomic intrinsics,
// which are supported on GCC but also Clang.

#define gc_atomic(verb, loc, ...) \
  __atomic_##verb(loc, ##__VA_ARGS__)

#define gc_atomic_load(loc)                                             \
  gc_atomic(load_n, loc, __ATOMIC_ACQUIRE)
#define gc_atomic_load_relaxed(loc)                                     \
  gc_atomic(load_n, loc, __ATOMIC_RELAXED)

#define gc_atomic_store(loc, val)                                       \
  gc_atomic(store_n, loc, val, __ATOMIC_RELEASE)
#define gc_atomic_store_relaxed(loc, val)                               \
  gc_atomic(store_n, loc, val, __ATOMIC_RELAXED)

#define gc_atomic_cmpxchg_weak(loc, expected, val)                      \
  gc_atomic(compare_exchange_n, loc, expected, val, 1,                  \
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#define gc_atomic_cmpxchg_weak_seq_cst(loc, expected, val)              \
  gc_atomic(compare_exchange_n, loc, expected, val, 1,                  \
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define gc_atomic_cmpxchg_strong(loc, expected, val)                    \
  gc_atomic(compare_exchange_n, loc, expected, val, 0,                  \
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#define gc_atomic_cmpxchg_strong_seq_cst(loc, expected, val)            \
  gc_atomic(compare_exchange_n, loc, expected, val, 0,                  \
            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define gc_atomic_swap(loc, val)                                        \
  gc_atomic(exchange_n, loc, val, __ATOMIC_ACQ_REL)

#define gc_atomic_swap_relaxed(loc, val)                                \
  gc_atomic(exchange_n, loc, val, __ATOMIC_RELAXED)

#define gc_atomic_fetch_add(loc, val)                                   \
  gc_atomic(fetch_add, loc, val, __ATOMIC_ACQ_REL)

#define gc_atomic_fetch_add_relaxed(loc, val)                           \
  gc_atomic(fetch_add, loc, val, __ATOMIC_RELAXED)

#define gc_atomic_fetch_sub(loc, val)                                   \
  gc_atomic(fetch_sub, loc, val, __ATOMIC_ACQ_REL)

#define gc_atomic_fetch_sub_relaxed(loc, val)                           \
  gc_atomic(fetch_sub, loc, val, __ATOMIC_RELAXED)

#endif // GC_ATOMICS_H
