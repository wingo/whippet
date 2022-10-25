#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GC_API_ 
#include "gc-api.h"

#include "semi-attrs.h"
#include "large-object-space.h"

#if GC_PRECISE
#include "precise-roots-embedder.h"
#else
#error semi is a precise collector
#endif

struct semi_space {
  uintptr_t hp;
  uintptr_t limit;
  uintptr_t from_space;
  uintptr_t to_space;
  size_t page_size;
  size_t stolen_pages;
  uintptr_t base;
  size_t size;
  long count;
};
struct gc_heap {
  struct semi_space semi_space;
  struct large_object_space large_object_space;
};
// One mutator per space, can just store the heap in the mutator.
struct gc_mutator {
  struct gc_heap heap;
  struct gc_mutator_roots *roots;
};


static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static inline struct gc_heap* mutator_heap(struct gc_mutator *mut) {
  return &mut->heap;
}
static inline struct semi_space* heap_semi_space(struct gc_heap *heap) {
  return &heap->semi_space;
}
static inline struct large_object_space* heap_large_object_space(struct gc_heap *heap) {
  return &heap->large_object_space;
}
static inline struct semi_space* mutator_semi_space(struct gc_mutator *mut) {
  return heap_semi_space(mutator_heap(mut));
}

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}

static void collect(struct gc_mutator *mut) GC_NEVER_INLINE;
static void collect_for_alloc(struct gc_mutator *mut, size_t bytes) GC_NEVER_INLINE;

static void trace(struct gc_edge edge, struct gc_heap *heap, void *visit_data);

static int semi_space_steal_pages(struct semi_space *space, size_t npages) {
  size_t stolen_pages = space->stolen_pages + npages;
  size_t old_limit_size = space->limit - space->to_space;
  size_t new_limit_size =
    (space->size - align_up(stolen_pages, 2) * space->page_size) / 2;

  if (space->to_space + new_limit_size < space->hp)
    return 0;

  space->limit = space->to_space + new_limit_size;
  space->stolen_pages = stolen_pages;

  madvise((void*)(space->to_space + new_limit_size),
          old_limit_size - new_limit_size,
          MADV_DONTNEED);
  madvise((void*)(space->from_space + new_limit_size),
          old_limit_size - new_limit_size,
          MADV_DONTNEED);
  return 1;
}

static void semi_space_set_stolen_pages(struct semi_space *space, size_t npages) {
  space->stolen_pages = npages;
  size_t limit_size =
    (space->size - align_up(npages, 2) * space->page_size) / 2;
  space->limit = space->to_space + limit_size;
}

static void flip(struct semi_space *space) {
  space->hp = space->from_space;
  space->from_space = space->to_space;
  space->to_space = space->hp;
  space->limit = space->hp + space->size / 2;
  space->count++;
}  

static struct gc_ref copy(struct semi_space *space, struct gc_ref ref) {
  size_t size;
  gc_trace_object(ref, NULL, NULL, NULL, &size);
  struct gc_ref new_ref = gc_ref(space->hp);
  memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(ref), size);
  gc_object_forward_nonatomic(ref, new_ref);
  space->hp += align_up(size, GC_ALIGNMENT);
  return new_ref;
}

static uintptr_t scan(struct gc_heap *heap, struct gc_ref grey) {
  size_t size;
  gc_trace_object(grey, trace, heap, NULL, &size);
  return gc_ref_value(grey) + align_up(size, GC_ALIGNMENT);
}

static struct gc_ref forward(struct semi_space *space, struct gc_ref obj) {
  uintptr_t forwarded = gc_object_forwarded_nonatomic(obj);
  return forwarded ? gc_ref(forwarded) : copy(space, obj);
}  

static void visit_semi_space(struct gc_heap *heap, struct semi_space *space,
                             struct gc_edge edge, struct gc_ref ref) {
  gc_edge_update(edge, forward(space, ref));
}

static void visit_large_object_space(struct gc_heap *heap,
                                     struct large_object_space *space,
                                     struct gc_ref ref) {
  if (large_object_space_copy(space, ref))
    gc_trace_object(ref, trace, heap, NULL, NULL);
}

static int semi_space_contains(struct semi_space *space, struct gc_ref ref) {
  uintptr_t addr = gc_ref_value(ref);
  return addr - space->base < space->size;
}

static void visit(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  if (!gc_ref_is_heap_object(ref))
    return;
  if (semi_space_contains(heap_semi_space(heap), ref))
    visit_semi_space(heap, heap_semi_space(heap), edge, ref);
  else if (large_object_space_contains(heap_large_object_space(heap), ref))
    visit_large_object_space(heap, heap_large_object_space(heap), ref);
  else
    GC_CRASH();
}

static void trace(struct gc_edge edge, struct gc_heap *heap, void *visit_data) {
  return visit(edge, heap);
}

static void collect(struct gc_mutator *mut) {
  struct gc_heap *heap = mutator_heap(mut);
  struct semi_space *semi = heap_semi_space(heap);
  struct large_object_space *large = heap_large_object_space(heap);
  // fprintf(stderr, "start collect #%ld:\n", space->count);
  large_object_space_start_gc(large, 0);
  flip(semi);
  uintptr_t grey = semi->hp;
  if (mut->roots)
    gc_trace_mutator_roots(mut->roots, trace, heap, NULL);
  // fprintf(stderr, "pushed %zd bytes in roots\n", space->hp - grey);
  while(grey < semi->hp)
    grey = scan(heap, gc_ref(grey));
  large_object_space_finish_gc(large, 0);
  semi_space_set_stolen_pages(semi, large->live_pages_at_last_collection);
  // fprintf(stderr, "%zd bytes copied\n", (space->size>>1)-(space->limit-space->hp));
}

static void collect_for_alloc(struct gc_mutator *mut, size_t bytes) {
  collect(mut);
  struct semi_space *space = mutator_semi_space(mut);
  if (space->limit - space->hp < bytes) {
    fprintf(stderr, "ran out of space, heap size %zu\n", space->size);
    GC_CRASH();
  }
}

void* gc_allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);
  struct semi_space *semi_space = heap_semi_space(heap);

  size_t npages = large_object_space_npages(space, size);
  if (!semi_space_steal_pages(semi_space, npages)) {
    collect(mut);
    if (!semi_space_steal_pages(semi_space, npages)) {
      fprintf(stderr, "ran out of space, heap size %zu\n", semi_space->size);
      GC_CRASH();
    }
  }

  void *ret = large_object_space_alloc(space, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(space, npages);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    GC_CRASH();
  }

  return ret;
}

void* gc_allocate_small(struct gc_mutator *mut, size_t size) {
  struct semi_space *space = mutator_semi_space(mut);
  while (1) {
    uintptr_t addr = space->hp;
    uintptr_t new_hp = align_up (addr + size, GC_ALIGNMENT);
    if (space->limit < new_hp) {
      collect_for_alloc(mut, size);
      continue;
    }
    space->hp = new_hp;
    // FIXME: Allow allocator to avoid clearing memory?
    clear_memory(addr, size);
    return (void *)addr;
  }
}
void* gc_allocate_pointerless(struct gc_mutator *mut, size_t size) {
  return gc_allocate(mut, size);
}

static int initialize_semi_space(struct semi_space *space, size_t size) {
  // Allocate even numbers of pages.
  size_t page_size = getpagesize();
  size = align_up(size, page_size * 2);

  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return 0;
  }

  space->to_space = space->hp = space->base = (uintptr_t) mem;
  space->from_space = space->base + size / 2;
  space->page_size = page_size;
  space->stolen_pages = 0;
  space->size = size;
  space->count = 0;

  return 1;
}
  
#define FOR_EACH_GC_OPTION(M) \
  M(GC_OPTION_FIXED_HEAP_SIZE, "fixed-heap-size") \
  M(GC_OPTION_PARALLELISM, "parallelism")

static void dump_available_gc_options(void) {
  fprintf(stderr, "available gc options:");
#define PRINT_OPTION(option, name) fprintf(stderr, " %s", name);
  FOR_EACH_GC_OPTION(PRINT_OPTION)
#undef PRINT_OPTION
  fprintf(stderr, "\n");
}

int gc_option_from_string(const char *str) {
#define PARSE_OPTION(option, name) if (strcmp(str, name) == 0) return option;
  FOR_EACH_GC_OPTION(PARSE_OPTION)
#undef PARSE_OPTION
  if (strcmp(str, "fixed-heap-size") == 0)
    return GC_OPTION_FIXED_HEAP_SIZE;
  if (strcmp(str, "parallelism") == 0)
    return GC_OPTION_PARALLELISM;
  fprintf(stderr, "bad gc option: '%s'\n", str);
  dump_available_gc_options();
  return -1;
}

struct options {
  size_t fixed_heap_size;
  size_t parallelism;
};

static size_t parse_size_t(double value) {
  GC_ASSERT(value >= 0);
  GC_ASSERT(value <= (size_t) -1);
  return value;
}

static int parse_options(int argc, struct gc_option argv[],
                         struct options *options) {
  options->parallelism = 1;
  for (int i = 0; i < argc; i++) {
    switch (argv[i].option) {
    case GC_OPTION_FIXED_HEAP_SIZE:
      options->fixed_heap_size = parse_size_t(argv[i].value);
      break;
    case GC_OPTION_PARALLELISM:
      options->parallelism = parse_size_t(argv[i].value);
      break;
    default:
      GC_CRASH();
    }
  }

  if (!options->fixed_heap_size) {
    fprintf(stderr, "fixed heap size is currently required\n");
    return 0;
  }
  if (options->parallelism != 1) {
    fprintf(stderr, "parallelism unimplemented in semispace copying collector\n");
    return 0;
  }

  return 1;
}

int gc_init(int argc, struct gc_option argv[],
            struct gc_stack_addr *stack_base, struct gc_heap **heap,
            struct gc_mutator **mut) {
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct semi_space, hp));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct semi_space, limit));

  struct options options = { 0, };
  if (!parse_options(argc, argv, &options))
    return 0;

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  *heap = mutator_heap(*mut);

  struct semi_space *space = mutator_semi_space(*mut);
  if (!initialize_semi_space(space, options.fixed_heap_size))
    return 0;
  if (!large_object_space_init(heap_large_object_space(*heap), *heap))
    return 0;
  
  // Ignore stack base, as we are precise.
  (*mut)->roots = NULL;

  return 1;
}

void gc_mutator_set_roots(struct gc_mutator *mut,
                          struct gc_mutator_roots *roots) {
  mut->roots = roots;
}
void gc_heap_set_roots(struct gc_heap *heap, struct gc_heap_roots *roots) {
  GC_CRASH();
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr *base,
                                      struct gc_heap *heap) {
  fprintf(stderr,
          "Semispace copying collector not appropriate for multithreaded use.\n");
  GC_CRASH();
}
void gc_finish_for_thread(struct gc_mutator *space) {
}

void* gc_call_without_gc(struct gc_mutator *mut, void* (*f)(void*),
                         void *data) {
  // Can't be threads, then there won't be collection.
  return f(data);
}

void gc_print_stats(struct gc_heap *heap) {
  struct semi_space *space = heap_semi_space(heap);
  printf("Completed %ld collections\n", space->count);
  printf("Heap size is %zd\n", space->size);
}
