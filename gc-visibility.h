#ifndef GC_VISIBILITY_H_
#define GC_VISIBILITY_H_

#define GC_INTERNAL __attribute__((visibility("hidden")))
#define GC_PUBLIC __attribute__((visibility("default")))

#endif // GC_VISIBILITY_H
