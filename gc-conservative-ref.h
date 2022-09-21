#ifndef GC_CONSERVATIVE_REF_H
#define GC_CONSERVATIVE_REF_H

#include <stdint.h>

struct gc_conservative_ref {
  uintptr_t value;
};

static inline struct gc_conservative_ref gc_conservative_ref(uintptr_t value) {
  return (struct gc_conservative_ref){value};
}
static inline uintptr_t gc_conservative_ref_value(struct gc_conservative_ref ref) {
  return ref.value;
}

#endif // GC_CONSERVATIVE_REF_H
