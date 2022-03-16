struct handle {
  void *v;
  struct handle *next;
};

#define HANDLE_TO(T) union { T* v; struct handle handle; }
#define HANDLE_REF(h) h.v
#define HANDLE_SET(h,val) do { h.v = val; } while (0)
#define PUSH_HANDLE(cx, h) push_handle(&cx->roots, &h.handle)
#define POP_HANDLE(cx) pop_handle(&cx->roots)

static inline void push_handle(struct handle **roots, struct handle *handle) {
  handle->next = *roots;
  *roots = handle;
}

static inline void pop_handle(struct handle **roots) {
  *roots = (*roots)->next;
}
