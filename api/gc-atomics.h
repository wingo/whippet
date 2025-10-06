#ifndef GC_ATOMICS_H
#define GC_ATOMICS_H

#include <stdatomic.h>

#define gc_atomic_cast(loc) \
  _Generic((loc),                                                       \
           uint8_t*   : ((_Atomic uint8_t*) (loc)),                     \
           uint16_t*  : ((_Atomic uint16_t*) (loc)),                    \
           uint32_t*  : ((_Atomic uint32_t*) (loc)),                    \
           uint64_t*  : ((_Atomic uint64_t*) (loc)),                    \
           int8_t*    : ((_Atomic int8_t*) (loc)),                      \
           int16_t*   : ((_Atomic int16_t*) (loc)),                     \
           int32_t*   : ((_Atomic int32_t*) (loc)),                     \
           int64_t*   : ((_Atomic int64_t*) (loc)),                     \
           void**     : ((_Atomic (void*)*) (loc)),                     \
           default    : (loc))

#define gc_atomic(verb, loc, ...) \
  atomic_##verb(gc_atomic_cast(loc), ##__VA_ARGS__)

#define gc_atomic_load(loc)                                             \
  gc_atomic(load_explicit, loc, memory_order_acquire)
#define gc_atomic_load_relaxed(loc)                                     \
  gc_atomic(load_explicit, loc, memory_order_relaxed)

#define gc_atomic_store(loc, val)                                       \
  gc_atomic(store_explicit, loc, val, memory_order_release)
#define gc_atomic_store_relaxed(loc, val)                               \
  gc_atomic(store_explicit, loc, val, memory_order_relaxed)

#define gc_atomic_cmpxchg_weak(loc, expected, val)                      \
  gc_atomic(compare_exchange_weak_explicit, loc, expected, val,         \
            memory_order_acq_rel, memory_order_acquire)

#define gc_atomic_cmpxchg_weak_seq_cst(loc, expected, val)              \
  gc_atomic(compare_exchange_weak_explicit, loc, expected, val,         \
            memory_order_seq_cst, memory_order_seq_cst)

#define gc_atomic_cmpxchg_strong(loc, expected, val)                    \
  gc_atomic(compare_exchange_strong_explicit, loc, expected, val,       \
            memory_order_acq_rel, memory_order_acquire)

#define gc_atomic_cmpxchg_strong_seq_cst(loc, expected, val)            \
  gc_atomic(compare_exchange_strong_explicit, loc, expected, val,       \
            memory_order_seq_cst, memory_order_seq_cst)

#define gc_atomic_swap(loc, val)                                        \
  gc_atomic(exchange_explicit, loc, val, memory_order_acq_rel)

#define gc_atomic_swap_relaxed(loc, val)                                \
  gc_atomic(exchange_explicit, loc, val, memory_order_relaxed)

#define gc_atomic_fetch_add(loc, val)                                   \
  gc_atomic(fetch_add_explicit, loc, val, memory_order_acq_rel)

#define gc_atomic_fetch_add_relaxed(loc, val)                           \
  gc_atomic(fetch_add_explicit, loc, val, memory_order_relaxed)

#define gc_atomic_fetch_sub(loc, val)                                   \
  gc_atomic(fetch_sub_explicit, loc, val, memory_order_acq_rel)

#define gc_atomic_fetch_sub_relaxed(loc, val)                           \
  gc_atomic(fetch_sub_explicit, loc, val, memory_order_relaxed)

#endif // GC_ATOMICS_H
