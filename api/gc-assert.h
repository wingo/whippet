#ifndef GC_ASSERT_H
#define GC_ASSERT_H

#include "gc-config.h"

#define GC_UNLIKELY(e) __builtin_expect(e, 0)
#define GC_LIKELY(e) __builtin_expect(e, 1)

#define GC_CRASH() __builtin_trap()

#if GC_DEBUG
#define GC_ASSERT(x) do { if (GC_UNLIKELY(!(x))) GC_CRASH(); } while (0)
#define GC_UNREACHABLE() GC_CRASH()
#else
#define GC_ASSERT(x) do { } while (0)
#define GC_UNREACHABLE() __builtin_unreachable()
#endif

#define GC_ASSERT_EQ(a, b) GC_ASSERT((a) == (b))

#endif // GC_ASSERT_H
