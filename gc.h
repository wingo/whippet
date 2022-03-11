#ifndef GC_H_
#define GC_H_

#if defined(GC_BDW)
#include "bdw.h"
#elif defined(GC_SEMI)
#include "semi.h"
#elif defined(GC_MARK_SWEEP)
#include "mark-sweep.h"
#elif defined(GC_PARALLEL_MARK_SWEEP)
#define GC_PARALLEL_MARK 1
#include "mark-sweep.h"
#else
#error unknown gc
#endif

#endif // GC_H_
