#ifndef GC_FORWARDING_H
#define GC_FORWARDING_H

#include <stdint.h>
#include "gc-ref.h"

enum gc_forwarding_state {
  GC_FORWARDING_STATE_FORWARDED,
  GC_FORWARDING_STATE_BUSY,
  GC_FORWARDING_STATE_ACQUIRED,
  GC_FORWARDING_STATE_NOT_FORWARDED
};

struct gc_atomic_forward {
  struct gc_ref ref;
  uintptr_t data;
  enum gc_forwarding_state state;
};

#endif // GC_FORWARDING_H
