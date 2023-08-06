#ifndef ASSERT_H
#define ASSERT_H

#define STATIC_ASSERT_EQ(a, b) _Static_assert((a) == (b), "eq")

#define UNLIKELY(e) __builtin_expect(e, 0)
#define LIKELY(e) __builtin_expect(e, 1)

#ifndef NDEBUG
#define ASSERT(x) do { if (UNLIKELY(!(x))) __builtin_trap(); } while (0)
#else
#define ASSERT(x) do { } while (0)
#endif
#define ASSERT_EQ(a,b) ASSERT((a) == (b))

#endif // ASSERT_H
