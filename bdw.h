// When pthreads are used, let `libgc' know about it and redirect
// allocation calls such as `GC_MALLOC ()' to (contention-free, faster)
// thread-local allocation.

#define GC_THREADS 1
#define GC_REDIRECT_TO_LOCAL 1

// Don't #define pthread routines to their GC_pthread counterparts.
// Instead we will be careful inside the benchmarks to use API to
// register threads with libgc.
#define GC_NO_THREAD_REDIRECTS 1

#include <gc/gc.h>
