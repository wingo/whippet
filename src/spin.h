#ifndef SPIN_H
#define SPIN_H

#include <sched.h>
#include <unistd.h>

static inline void yield_for_spin(size_t spin_count) {
  if (spin_count < 10)
    __builtin_ia32_pause();
  else if (spin_count < 20)
    sched_yield();
  else if (spin_count < 40)
    usleep(0);
  else
    usleep(1);
}  

#endif // SPIN_H
