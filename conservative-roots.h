struct handle { void *unused; };

#define HANDLE_TO(T) union { T* v; struct handle handle; }
#define HANDLE_REF(h) h.v
#define HANDLE_SET(h,val) do { h.v = val; } while (0)
#define PUSH_HANDLE(cx, h) do { (void) &h; } while (0)
#define POP_HANDLE(cx) do { } while (0)
