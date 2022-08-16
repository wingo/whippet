struct handle { void *unused; };

#define HANDLE_TO(T) union { T* v; struct handle handle; }
#define HANDLE_REF(h) h.v
#define HANDLE_SET(h,val) do { h.v = val; } while (0)
#define PUSH_HANDLE(cx, h) do { (void) &h; } while (0)
#define POP_HANDLE(cx) do { } while (0)

static inline void visit_thread_roots(void *thread_roots,
                                      void (*trace_edge)(struct gc_edge edge,
                                                         void *trace_data),
                                      void *trace_data) {
  abort();
}

static inline void visit_roots(struct handle *roots,
                               void (*trace_edge)(struct gc_edge edge,
                                                  void *trace_data),
                               void *trace_data) {
  GC_ASSERT(!roots);
}
