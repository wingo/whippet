#ifndef SPIN_H
#define SPIN_H

#include <sched.h>
#include <unistd.h>

static inline void yield_for_spin(size_t spin_count) {
  if (spin_count < 10)
    __builtin_ia32_pause();
  else if (spin_count < 20)
    sched_yield();
  else
    // initially 0 usec, then 2 usec after 32 spins, 4 usec after 64 spins, 6
    // usec after 128 spins, and so on.
    usleep((__builtin_clzll(20) - __builtin_clzll(spin_count)) << 1);
}  

#endif // SPIN_H
