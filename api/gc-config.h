#ifndef GC_CONFIG_H
#define GC_CONFIG_H

#ifndef GC_DEBUG
#define GC_DEBUG 0
#endif

#ifndef GC_HAS_IMMEDIATES
#define GC_HAS_IMMEDIATES 1
#endif

#ifndef GC_PARALLEL
#define GC_PARALLEL 0
#endif

#ifndef GC_GENERATIONAL
#define GC_GENERATIONAL 0
#endif

// Though you normally wouldn't configure things this way, it's possible
// to have both precise and conservative roots.  However we have to
// either have precise or conservative tracing; not a mix.

#ifndef GC_PRECISE_ROOTS
#define GC_PRECISE_ROOTS 0
#endif

#ifndef GC_CONSERVATIVE_ROOTS
#define GC_CONSERVATIVE_ROOTS 0
#endif

#ifndef GC_CONSERVATIVE_TRACE
#define GC_CONSERVATIVE_TRACE 0
#endif

#ifndef GC_CONCURRENT_TRACE
#define GC_CONCURRENT_TRACE 0
#endif

#endif // GC_CONFIG_H
