#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "precise-roots.h"

struct context {
  uintptr_t hp;
  uintptr_t limit;
  uintptr_t heap_base;
  size_t heap_size;
  struct handle *roots;
  void *mem;
  size_t mem_size;
  long count;
};

static const uintptr_t ALIGNMENT = 8;

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct context *cx) NEVER_INLINE;
static void collect_for_alloc(struct context *cx, size_t bytes) NEVER_INLINE;

static void visit(void **loc, void *visit_data);

static void flip(struct context *cx) {
  uintptr_t split = cx->heap_base + (cx->heap_size >> 1);
  if (cx->hp <= split) {
    cx->hp = split;
    cx->limit = cx->heap_base + cx->heap_size;
  } else {
    cx->hp = cx->heap_base;
    cx->limit = split;
  }
  cx->count++;
}  

static void* copy(struct context *cx, uintptr_t kind, void *obj) {
  size_t size;
  switch (kind) {
#define COMPUTE_SIZE(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      size = name##_size(obj); \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(COMPUTE_SIZE)
#undef COMPUTE_SIZE
  default:
    abort ();
  }
  void *new_obj = (void*)cx->hp;
  memcpy(new_obj, obj, size);
  *(uintptr_t*) obj = cx->hp;
  cx->hp += align_up (size, ALIGNMENT);
  return new_obj;
}

static uintptr_t scan(struct context *cx, uintptr_t grey) {
  void *obj = (void*)grey;
  uintptr_t kind = *(uintptr_t*) obj;
  switch (kind) {
#define SCAN_OBJECT(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      visit_##name##_fields((Name*)obj, visit, cx); \
      return grey + align_up(name##_size((Name*)obj), ALIGNMENT);
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    abort ();
  }
}

static void* forward(struct context *cx, void *obj) {
  uintptr_t header_word = *(uintptr_t*)obj;
  switch (header_word) {
#define CASE_ALLOC_KIND(name, Name, NAME) \
    case ALLOC_KIND_##NAME:
    FOR_EACH_HEAP_OBJECT_KIND(CASE_ALLOC_KIND)
#undef CASE_ALLOC_KIND
    return copy(cx, header_word, obj);
  default:
    return (void*)header_word;
  }
}  

static void visit(void **loc, void *visit_data) {
  struct context *cx = visit_data;
  void *obj = *loc;
  if (obj != NULL)
    *loc = forward(cx, obj);
}
static void collect(struct context *cx) {
  // fprintf(stderr, "start collect #%ld:\n", cx->count);
  flip(cx);
  uintptr_t grey = cx->hp;
  for (struct handle *h = cx->roots; h; h = h->next)
    visit(&h->v, cx);
  // fprintf(stderr, "pushed %zd bytes in roots\n", cx->hp - grey);
  while(grey < cx->hp)
    grey = scan(cx, grey);
  // fprintf(stderr, "%zd bytes copied\n", (cx->heap_size>>1)-(cx->limit-cx->hp));

}
static void collect_for_alloc(struct context *cx, size_t bytes) {
  collect(cx);
  if (cx->limit - cx->hp < bytes) {
    fprintf(stderr, "ran out of space, heap size %zu\n", cx->mem_size);
    abort();
  }
}

static inline void* allocate(struct context *cx, enum alloc_kind kind,
                             size_t size) {
  while (1) {
    uintptr_t addr = cx->hp;
    uintptr_t new_hp = align_up (addr + size, ALIGNMENT);
    if (cx->limit < new_hp) {
      collect_for_alloc(cx, size);
      continue;
    }
    cx->hp = new_hp;
    void *ret = (void *)addr;
    uintptr_t *header_word = ret;
    *header_word = kind;
    // FIXME: Allow allocator to avoid initializing pointerless memory?
    // if (kind == NODE)
    clear_memory(addr + sizeof(uintptr_t), size - sizeof(uintptr_t));
    return ret;
  }
}
static inline void* allocate_pointerless(struct context *cx,
                                         enum alloc_kind kind, size_t size) {
  return allocate(cx, kind, size);
}

static inline void init_field(void **addr, void *val) {
  *addr = val;
}
static inline void set_field(void **addr, void *val) {
  *addr = val;
}
static inline void* get_field(void **addr) {
  return *addr;
}

static struct context* initialize_gc(size_t size) {
  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    abort();
  }
  struct context *cx = mem;
  cx->mem = mem;
  cx->mem_size = size;
  // Round up to twice ALIGNMENT so that both spaces will be aligned.
  size_t overhead = align_up(sizeof(*cx), ALIGNMENT * 2);
  cx->hp = cx->heap_base = ((uintptr_t) mem) + overhead;
  cx->heap_size = size - overhead;
  cx->count = -1;
  flip(cx);
  cx->roots = NULL;
  return cx;
}

static struct context* initialize_gc_for_thread(uintptr_t *stack_base,
                                                struct context *parent) {
  fprintf(stderr,
          "Semispace copying collector not appropriate for multithreaded use.\n");
  exit(1);
}
static void finish_gc_for_thread(struct context *cx) {
}

static inline void print_start_gc_stats(struct context *cx) {
}

static inline void print_end_gc_stats(struct context *cx) {
  printf("Completed %ld collections\n", cx->count);
  printf("Heap size is %zd\n", cx->mem_size);
}
