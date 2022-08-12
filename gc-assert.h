#ifndef GC_ASSERT_H
#define GC_ASSERT_H

#include "gc-config.h"

#define GC_UNLIKELY(e) __builtin_expect(e, 0)
#define GC_LIKELY(e) __builtin_expect(e, 1)

#if GC_DEBUG
#define GC_ASSERT(x) do { if (GC_UNLIKELY(!(x))) __builtin_trap(); } while (0)
#else
#define GC_ASSERT(x) do { } while (0)
#endif

#endif // GC_ASSERT_H
