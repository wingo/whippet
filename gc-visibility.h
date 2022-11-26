#ifndef GC_VISIBILITY_H_
#define GC_VISIBILITY_H_

#define GC_INTERNAL __attribute__((visibility("hidden")))
#define GC_PUBLIC __attribute__((visibility("default")))

// FIXME: Conflict with bdw-gc GC_API.  Switch prefix?
#ifndef GC_API_
#define GC_API_ GC_INTERNAL
#endif

#endif // GC_VISIBILITY_H
