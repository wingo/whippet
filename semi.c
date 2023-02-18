#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "semi-attrs.h"
#include "large-object-space.h"

#if GC_CONSERVATIVE_ROOTS
#error semi is a precise collector
#endif

struct region {
  uintptr_t base;
  size_t size;
  size_t unavailable;
};
struct semi_space {
  uintptr_t hp;
  uintptr_t limit;
  struct region from_space;
  struct region to_space;
  size_t page_size;
  size_t stolen_pages;
  size_t reserve_pages;
};
struct gc_heap {
  struct semi_space semi_space;
  struct large_object_space large_object_space;
  struct gc_pending_ephemerons *pending_ephemerons;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
  size_t size;
  long count;
  int check_pending_ephemerons;
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

static void region_trim_by(struct region *region, size_t newly_unavailable) {
  size_t old_available = region->size - region->unavailable;
  GC_ASSERT(newly_unavailable <= old_available);

  madvise((void*)(region->base + old_available - newly_unavailable),
          newly_unavailable,
          MADV_DONTNEED);
  region->unavailable += newly_unavailable;
}

static void region_reset_unavailable(struct region *region,
                                     size_t unavailable) {
  GC_ASSERT(unavailable <= region->unavailable);
  region->unavailable = unavailable;
}

static int semi_space_steal_pages(struct semi_space *space, size_t npages) {
  size_t old_unavailable_pages = space->stolen_pages + space->reserve_pages;
  size_t old_region_unavailable_pages = align_up(old_unavailable_pages, 2) / 2;
  size_t new_unavailable_pages = old_unavailable_pages + npages;
  size_t new_region_unavailable_pages = align_up(new_unavailable_pages, 2) / 2;
  size_t region_newly_unavailable_pages =
    new_region_unavailable_pages - old_region_unavailable_pages;
  size_t region_newly_unavailable_bytes =
    region_newly_unavailable_pages * space->page_size;

  if (space->limit - space->hp < region_newly_unavailable_bytes)
    return 0;

  space->stolen_pages += npages;

  if (region_newly_unavailable_bytes == 0)
    return 1;

  space->limit -= region_newly_unavailable_bytes;
  region_trim_by(&space->to_space, region_newly_unavailable_bytes);
  region_trim_by(&space->from_space, region_newly_unavailable_bytes);
  return 1;
}

static void semi_space_set_stolen_pages(struct semi_space *space,
                                        size_t npages) {
  space->stolen_pages = npages;
  size_t unavailable_pages = space->stolen_pages + space->reserve_pages;
  size_t region_unavailable_pages = align_up(unavailable_pages, 2) / 2;
  size_t region_unavailable_bytes = region_unavailable_pages * space->page_size;

  region_reset_unavailable(&space->to_space, region_unavailable_bytes);
  region_reset_unavailable(&space->from_space, region_unavailable_bytes);

  space->limit =
    space->to_space.base + space->to_space.size - space->to_space.unavailable;
}

static void flip(struct semi_space *space) {
  struct region tmp;
  memcpy(&tmp, &space->from_space, sizeof(tmp));
  memcpy(&space->from_space, &space->to_space, sizeof(tmp));
  memcpy(&space->to_space, &tmp, sizeof(tmp));

  space->hp = space->to_space.base;
  space->limit = space->hp + space->to_space.size;
}  

static struct gc_ref copy(struct gc_heap *heap, struct semi_space *space,
                          struct gc_ref ref) {
  size_t size;
  gc_trace_object(ref, NULL, NULL, NULL, &size);
  struct gc_ref new_ref = gc_ref(space->hp);
  memcpy(gc_ref_heap_object(new_ref), gc_ref_heap_object(ref), size);
  gc_object_forward_nonatomic(ref, new_ref);
  space->hp += align_up(size, GC_ALIGNMENT);

  if (GC_UNLIKELY(heap->check_pending_ephemerons))
    gc_resolve_pending_ephemerons(ref, heap);

  return new_ref;
}

static uintptr_t scan(struct gc_heap *heap, struct gc_ref grey) {
  size_t size;
  gc_trace_object(grey, trace, heap, NULL, &size);
  return gc_ref_value(grey) + align_up(size, GC_ALIGNMENT);
}

static struct gc_ref forward(struct gc_heap *heap, struct semi_space *space,
                             struct gc_ref obj) {
  uintptr_t forwarded = gc_object_forwarded_nonatomic(obj);
  return forwarded ? gc_ref(forwarded) : copy(heap, space, obj);
}  

static void visit_semi_space(struct gc_heap *heap, struct semi_space *space,
                             struct gc_edge edge, struct gc_ref ref) {
  gc_edge_update(edge, forward(heap, space, ref));
}

static void visit_large_object_space(struct gc_heap *heap,
                                     struct large_object_space *space,
                                     struct gc_ref ref) {
  if (large_object_space_copy(space, ref)) {
    if (GC_UNLIKELY(heap->check_pending_ephemerons))
      gc_resolve_pending_ephemerons(ref, heap);

    gc_trace_object(ref, trace, heap, NULL, NULL);
  }
}

static int region_contains(struct region *region, uintptr_t addr) {
  return addr - region->base < region->size;
}

static int semi_space_contains(struct semi_space *space, struct gc_ref ref) {
  // As each live object is traced exactly once, its edges have not been
  // visited, so its refs are to fromspace and not tospace.
  uintptr_t addr = gc_ref_value(ref);
  GC_ASSERT(!region_contains(&space->to_space, addr));
  return region_contains(&space->from_space, addr);
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

struct gc_pending_ephemerons *
gc_heap_pending_ephemerons(struct gc_heap *heap) {
  return heap->pending_ephemerons;
}

int gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  GC_ASSERT(gc_ref_is_heap_object(ref));
  if (semi_space_contains(heap_semi_space(heap), ref)) {
    uintptr_t forwarded = gc_object_forwarded_nonatomic(ref);
    if (!forwarded)
      return 0;
    gc_edge_update(edge, gc_ref(forwarded));
    return 1;
  } else if (large_object_space_contains(heap_large_object_space(heap), ref)) {
    return large_object_space_is_copied(heap_large_object_space(heap), ref);
  }
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
  heap->count++;
  heap->check_pending_ephemerons = 0;
  uintptr_t grey = semi->hp;
  if (mut->roots)
    gc_trace_mutator_roots(mut->roots, trace, heap, NULL);
  // fprintf(stderr, "pushed %zd bytes in roots\n", space->hp - grey);
  while(grey < semi->hp)
    grey = scan(heap, gc_ref(grey));
  gc_scan_pending_ephemerons(heap->pending_ephemerons, heap, 0, 1);
  heap->check_pending_ephemerons = 1;
  while (gc_pop_resolved_ephemerons(heap, trace, NULL))
    while(grey < semi->hp)
      grey = scan(heap, gc_ref(grey));
  large_object_space_finish_gc(large, 0);
  semi_space_set_stolen_pages(semi, large->live_pages_at_last_collection);
  gc_sweep_pending_ephemerons(heap->pending_ephemerons, 0, 1);
  // fprintf(stderr, "%zd bytes copied\n", (space->size>>1)-(space->limit-space->hp));
}

void gc_collect(struct gc_mutator *mut) {
  collect(mut);
}

static void collect_for_alloc(struct gc_mutator *mut, size_t bytes) {
  collect(mut);
  struct semi_space *space = mutator_semi_space(mut);
  if (space->limit - space->hp < bytes) {
    fprintf(stderr, "ran out of space, heap size %zu\n",
            mutator_heap(mut)->size);
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
      fprintf(stderr, "ran out of space, heap size %zu\n",
              mutator_heap(mut)->size);
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

struct gc_ref gc_allocate_ephemeron(struct gc_mutator *mut) {
  return gc_ref_from_heap_object(gc_allocate(mut, gc_ephemeron_size()));
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
}

static int initialize_region(struct region *region, size_t size) {
  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return 0;
  }

  region->base = (uintptr_t)mem;
  region->size = size;
  region->unavailable = 0;
  return 1;
}

static int initialize_semi_space(struct semi_space *space, size_t size) {
  // Allocate even numbers of pages.
  size_t page_size = getpagesize();
  size = align_up(size, page_size * 2);

  if (!initialize_region(&space->from_space, size / 2))
    return 0;
  if (!initialize_region(&space->to_space, size / 2))
    return 0;

  space->hp = space->to_space.base;
  space->limit = space->hp + space->to_space.size;

  space->page_size = page_size;
  space->stolen_pages = 0;
  space->reserve_pages = 0;

  return 1;
}
  
static int heap_prepare_pending_ephemerons(struct gc_heap *heap) {
  struct gc_pending_ephemerons *cur = heap->pending_ephemerons;
  size_t target = heap->size * heap->pending_ephemerons_size_factor;
  double slop = heap->pending_ephemerons_size_slop;

  heap->pending_ephemerons = gc_prepare_pending_ephemerons(cur, target, slop);

  return !!heap->pending_ephemerons;
}

unsigned gc_heap_ephemeron_trace_epoch(struct gc_heap *heap) {
  return heap->count;
}

static int heap_init(struct gc_heap *heap, size_t size) {
  heap->pending_ephemerons_size_factor = 0.01;
  heap->pending_ephemerons_size_slop = 0.5;
  heap->count = 0;
  heap->size = size;

  return heap_prepare_pending_ephemerons(heap);
}

struct gc_options {
  struct gc_common_options common;
};
int gc_option_from_string(const char *str) {
  return gc_common_option_from_string(str);
}
struct gc_options* gc_allocate_options(void) {
  struct gc_options *ret = malloc(sizeof(struct gc_options));
  gc_init_common_options(&ret->common);
  return ret;
}
int gc_options_set_int(struct gc_options *options, int option, int value) {
  return gc_common_options_set_int(&options->common, option, value);
}
int gc_options_set_size(struct gc_options *options, int option,
                        size_t value) {
  return gc_common_options_set_size(&options->common, option, value);
}
int gc_options_set_double(struct gc_options *options, int option,
                          double value) {
  return gc_common_options_set_double(&options->common, option, value);
}
int gc_options_parse_and_set(struct gc_options *options, int option,
                             const char *value) {
  return gc_common_options_parse_and_set(&options->common, option, value);
}

int gc_init(struct gc_options *options, struct gc_stack_addr *stack_base,
            struct gc_heap **heap, struct gc_mutator **mut) {
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct semi_space, hp));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct semi_space, limit));

  if (!options) options = gc_allocate_options();

  if (options->common.heap_size_policy != GC_HEAP_SIZE_FIXED) {
    fprintf(stderr, "fixed heap size is currently required\n");
    return 0;
  }
  if (options->common.parallelism != 1) {
    fprintf(stderr, "parallelism unimplemented in semispace copying collector\n");
    return 0;
  }

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  *heap = mutator_heap(*mut);

  if (!heap_init(*heap, options->common.heap_size))
    return 0;

  struct semi_space *space = mutator_semi_space(*mut);
  if (!initialize_semi_space(space, options->common.heap_size))
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
  printf("Completed %ld collections\n", heap->count);
  printf("Heap size is %zd\n", heap->size);
}
