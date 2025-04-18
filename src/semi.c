#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gc-api.h"

#define GC_IMPL 1
#include "gc-internal.h"

#include "gc-platform.h"
#include "gc-tracepoint.h"
#include "heap-sizer.h"
#include "semi-attrs.h"
#include "large-object-space.h"

#if GC_CONSERVATIVE_ROOTS
#error semi is a precise collector
#endif

struct gc_options {
  struct gc_common_options common;
};
struct region {
  uintptr_t base;
  size_t active_size;
  size_t mapped_size;
};
struct semi_space {
  uintptr_t hp;
  uintptr_t limit;
  struct region from_space;
  struct region to_space;
  size_t page_size;
  size_t stolen_pages;
  size_t live_bytes_at_last_gc;
};
struct gc_heap {
  struct semi_space semi_space;
  struct large_object_space large_object_space;
  struct gc_pending_ephemerons *pending_ephemerons;
  struct gc_finalizer_state *finalizer_state;
  struct gc_extern_space *extern_space;
  double pending_ephemerons_size_factor;
  double pending_ephemerons_size_slop;
  size_t size;
  size_t total_allocated_bytes_at_last_gc;
  long count;
  int check_pending_ephemerons;
  const struct gc_options *options;
  struct gc_heap_roots *roots;
  struct gc_heap_sizer sizer;
  struct gc_event_listener event_listener;
  void *event_listener_data;
};
// One mutator per space, can just store the heap in the mutator.
struct gc_mutator {
  struct gc_heap heap;
  struct gc_mutator_roots *roots;
  void *event_listener_data;
};

#define HEAP_EVENT(heap, event, ...) do {                               \
    (heap)->event_listener.event((heap)->event_listener_data, ##__VA_ARGS__); \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)
#define MUTATOR_EVENT(mut, event, ...) do {                             \
    (mut)->heap->event_listener.event((mut)->event_listener_data,       \
                                      ##__VA_ARGS__);                   \
    GC_TRACEPOINT(event, ##__VA_ARGS__);                                \
  } while (0)

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

struct gc_heap* gc_mutator_heap(struct gc_mutator *mutator) {
  return mutator_heap(mutator);
}
uintptr_t gc_small_object_nursery_low_address(struct gc_heap *heap) {
  GC_CRASH();
}
uintptr_t gc_small_object_nursery_high_address(struct gc_heap *heap) {
  GC_CRASH();
}

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
static size_t max_size(size_t a, size_t b) { return a < b ? b : a; }

static void collect(struct gc_mutator *mut, size_t for_alloc) GC_NEVER_INLINE;
static void collect_for_alloc(struct gc_mutator *mut,
                              size_t bytes) GC_NEVER_INLINE;

static void trace(struct gc_edge edge, struct gc_heap *heap, void *visit_data);

static void region_trim_by(struct region *region, size_t newly_unavailable) {
  size_t old_available = region->active_size;
  GC_ASSERT(newly_unavailable <= region->active_size);

  region->active_size -= newly_unavailable;
  gc_platform_discard_memory((void*)(region->base + region->active_size),
                             newly_unavailable);
}

static void region_set_active_size(struct region *region, size_t size) {
  GC_ASSERT(size <= region->mapped_size);
  GC_ASSERT(size == align_up(size, gc_platform_page_size()));
  if (size < region->active_size)
    region_trim_by(region, region->active_size - size);
  else
    region->active_size = size;
}

static int semi_space_steal_pages(struct semi_space *space, size_t npages) {
  size_t old_stolen_pages = space->stolen_pages;
  size_t old_region_stolen_pages = align_up(old_stolen_pages,2)/2;
  size_t new_stolen_pages = old_stolen_pages + npages;
  size_t new_region_stolen_pages = align_up(new_stolen_pages,2)/2;
  size_t region_newly_stolen_pages =
    new_region_stolen_pages - old_region_stolen_pages;
  size_t region_newly_unavailable_bytes =
    region_newly_stolen_pages * space->page_size;

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

static void semi_space_finish_gc(struct semi_space *space,
                                 size_t large_object_pages) {
  space->live_bytes_at_last_gc = space->hp - space->to_space.base;
  space->stolen_pages = large_object_pages;
  space->limit = 0; // set in adjust_heap_size_and_limits
}

static void
semi_space_add_to_allocation_counter(struct semi_space *space,
                                     uint64_t *counter) {
  size_t base = space->to_space.base + space->live_bytes_at_last_gc;
  *counter += space->hp - base;
}

static void flip(struct semi_space *space) {
  struct region tmp;
  GC_ASSERT(space->hp <= space->limit);
  GC_ASSERT(space->limit - space->to_space.base <= space->to_space.active_size);
  GC_ASSERT(space->to_space.active_size <= space->from_space.mapped_size);
  memcpy(&tmp, &space->from_space, sizeof(tmp));
  memcpy(&space->from_space, &space->to_space, sizeof(tmp));
  memcpy(&space->to_space, &tmp, sizeof(tmp));

  space->hp = space->to_space.base;
  space->limit = space->hp + space->to_space.active_size;
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
  if (large_object_space_mark(space, ref)) {
    if (GC_UNLIKELY(heap->check_pending_ephemerons))
      gc_resolve_pending_ephemerons(ref, heap);

    gc_trace_object(ref, trace, heap, NULL, NULL);
  }
}

static int region_contains(struct region *region, uintptr_t addr) {
  return addr - region->base < region->active_size;
}

static int semi_space_contains(struct semi_space *space, struct gc_ref ref) {
  // As each live object is traced exactly once, its edges have not been
  // visited, so its refs are to fromspace and not tospace.
  uintptr_t addr = gc_ref_value(ref);
  GC_ASSERT(!region_contains(&space->to_space, addr));
  return region_contains(&space->from_space, addr);
}

static void visit_external_object(struct gc_heap *heap,
                                  struct gc_extern_space *space,
                                  struct gc_edge edge,
                                  struct gc_ref old_ref) {
  if (gc_extern_space_visit(space, edge, old_ref)) {
    if (GC_UNLIKELY(heap->check_pending_ephemerons))
      gc_resolve_pending_ephemerons(old_ref, heap);

    gc_trace_object(gc_edge_ref(edge), trace, heap, NULL, NULL);
  }
}

static void visit(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  if (gc_ref_is_null(ref) || gc_ref_is_immediate(ref))
    return;
  if (semi_space_contains(heap_semi_space(heap), ref))
    visit_semi_space(heap, heap_semi_space(heap), edge, ref);
  else if (large_object_space_contains_with_lock(heap_large_object_space(heap),
                                                 ref))
    visit_large_object_space(heap, heap_large_object_space(heap), ref);
  else
    visit_external_object(heap, heap->extern_space, edge, ref);
}

struct gc_pending_ephemerons *
gc_heap_pending_ephemerons(struct gc_heap *heap) {
  return heap->pending_ephemerons;
}

int gc_visit_ephemeron_key(struct gc_edge edge, struct gc_heap *heap) {
  struct gc_ref ref = gc_edge_ref(edge);
  GC_ASSERT(!gc_ref_is_null(ref));
  if (gc_ref_is_immediate(ref))
    return 1;
  GC_ASSERT(gc_ref_is_heap_object(ref));
  if (semi_space_contains(heap_semi_space(heap), ref)) {
    uintptr_t forwarded = gc_object_forwarded_nonatomic(ref);
    if (!forwarded)
      return 0;
    gc_edge_update(edge, gc_ref(forwarded));
    return 1;
  } else if (large_object_space_contains_with_lock(heap_large_object_space(heap), ref)) {
    return large_object_space_is_marked(heap_large_object_space(heap), ref);
  }
  GC_CRASH();
}

static void trace(struct gc_edge edge, struct gc_heap *heap, void *visit_data) {
  return visit(edge, heap);
}

static int grow_region_if_needed(struct region *region, size_t new_size) {
  if (new_size <= region->mapped_size)
    return 1;

  void *mem = gc_platform_acquire_memory(new_size, 0);
  DEBUG("new size %zx\n", new_size);
  if (!mem)
    return 0;
  if (region->mapped_size)
    gc_platform_release_memory((void*)region->base, region->mapped_size);
  region->base = (uintptr_t)mem;
  region->active_size = 0;
  region->mapped_size = new_size;
  return 1;
}

static void truncate_region(struct region *region, size_t new_size) {
  GC_ASSERT(new_size <= region->mapped_size);

  size_t bytes = region->mapped_size - new_size;
  if (bytes) {
    gc_platform_release_memory((void*)(region->base + new_size), bytes);
    region->mapped_size = new_size;
    if (region->active_size > new_size)
      region->active_size = new_size;
  }
}

static void resize_heap(struct gc_heap *heap, size_t new_heap_size) {
  struct semi_space *semi = heap_semi_space(heap);
  new_heap_size = align_up(new_heap_size, semi->page_size * 2);
  size_t new_region_size = new_heap_size / 2;

  // Note that there is an asymmetry in how heap size is adjusted: we
  // grow in two cycles (first the fromspace, then the tospace after it
  // becomes the fromspace in the next collection) but shrink in one (by
  // returning pages to the OS).

  // If we are growing the heap now, grow the fromspace mapping.  Also,
  // always try to grow the fromspace if it is smaller than the tospace.
  grow_region_if_needed(&semi->from_space,
                        max_size(new_region_size, semi->to_space.mapped_size));

  // We may have grown fromspace.  Find out what our actual new region
  // size will be.
  new_region_size = min_size(new_region_size,
                             min_size(semi->to_space.mapped_size,
                                      semi->from_space.mapped_size));
  size_t old_heap_size = heap->size;
  heap->size = new_region_size * 2;
  if (heap->size != old_heap_size)
    HEAP_EVENT(heap, heap_resized, heap->size);
}

static void reset_heap_limits(struct gc_heap *heap) {
  struct semi_space *semi = heap_semi_space(heap);
  size_t new_region_size = align_up(heap->size, semi->page_size * 2) / 2;
  size_t stolen = align_up(semi->stolen_pages, 2) * semi->page_size;
  GC_ASSERT(new_region_size > stolen/2);
  size_t new_active_region_size = new_region_size - stolen/2;

  region_set_active_size(&semi->from_space, new_active_region_size);
  region_set_active_size(&semi->to_space, new_active_region_size);

  size_t new_limit = semi->to_space.base + new_active_region_size;
  GC_ASSERT(semi->hp <= new_limit);
  semi->limit = new_limit;
}

static uintptr_t trace_closure(struct gc_heap *heap, struct semi_space *semi,
                               uintptr_t grey) {
  while(grey < semi->hp)
    grey = scan(heap, gc_ref(grey));
  return grey;
}

static uintptr_t resolve_ephemerons(struct gc_heap *heap, uintptr_t grey) {
  for (struct gc_ephemeron *resolved = gc_pop_resolved_ephemerons(heap);
       resolved;
       resolved = gc_pop_resolved_ephemerons(heap)) {
    gc_trace_resolved_ephemerons(resolved, trace, heap, NULL);
    grey = trace_closure(heap, heap_semi_space(heap), grey);
  }
  return grey;
}

static uintptr_t resolve_finalizers(struct gc_heap *heap, uintptr_t grey) {
  for (size_t priority = 0;
       priority < gc_finalizer_priority_count();
       priority++) {
    if (gc_resolve_finalizers(heap->finalizer_state, priority,
                              trace, heap, NULL)) {
      grey = trace_closure(heap, heap_semi_space(heap), grey);
      grey = resolve_ephemerons(heap, grey);
    }
  }
  gc_notify_finalizers(heap->finalizer_state, heap);
  return grey;
}

static void collect(struct gc_mutator *mut, size_t for_alloc) {
  struct gc_heap *heap = mutator_heap(mut);
  int is_minor = 0;
  int is_compacting = 1;
  uint64_t start_ns = gc_platform_monotonic_nanoseconds();

  HEAP_EVENT(heap, requesting_stop);
  HEAP_EVENT(heap, waiting_for_stop);
  HEAP_EVENT(heap, mutators_stopped);
  HEAP_EVENT(heap, prepare_gc, GC_COLLECTION_COMPACTING);

  struct semi_space *semi = heap_semi_space(heap);
  struct large_object_space *large = heap_large_object_space(heap);
  // fprintf(stderr, "start collect #%ld:\n", space->count);
  uint64_t *counter_loc = &heap->total_allocated_bytes_at_last_gc;
  semi_space_add_to_allocation_counter(semi, counter_loc);
  large_object_space_add_to_allocation_counter(large, counter_loc);
  large_object_space_start_gc(large, 0);
  gc_extern_space_start_gc(heap->extern_space, 0);
  flip(semi);
  heap->count++;
  heap->check_pending_ephemerons = 0;
  uintptr_t grey = semi->hp;
  if (heap->roots)
    gc_trace_heap_roots(heap->roots, trace, heap, NULL);
  if (mut->roots)
    gc_trace_mutator_roots(mut->roots, trace, heap, NULL);
  gc_visit_finalizer_roots(heap->finalizer_state, trace, heap, NULL);
  HEAP_EVENT(heap, roots_traced);
  // fprintf(stderr, "pushed %zd bytes in roots\n", space->hp - grey);
  grey = trace_closure(heap, semi, grey);
  HEAP_EVENT(heap, heap_traced);
  gc_scan_pending_ephemerons(heap->pending_ephemerons, heap, 0, 1);
  heap->check_pending_ephemerons = 1;
  grey = resolve_ephemerons(heap, grey);
  HEAP_EVENT(heap, ephemerons_traced);
  grey = resolve_finalizers(heap, grey);
  HEAP_EVENT(heap, finalizers_traced);
  large_object_space_finish_gc(large, 0);
  gc_extern_space_finish_gc(heap->extern_space, 0);
  semi_space_finish_gc(semi, large->live_pages_at_last_collection);
  gc_sweep_pending_ephemerons(heap->pending_ephemerons, 0, 1);
  size_t live_size = semi->live_bytes_at_last_gc;
  live_size += large_object_space_size_at_last_collection(large);
  live_size += for_alloc;
  uint64_t pause_ns = gc_platform_monotonic_nanoseconds() - start_ns;
  HEAP_EVENT(heap, live_data_size, live_size);
  DEBUG("gc %zu: live size %zu, heap size %zu\n", heap->count, live_size,
        heap->size);
  gc_heap_sizer_on_gc(heap->sizer, heap->size, live_size, pause_ns,
                      resize_heap);
  reset_heap_limits(heap);  
  clear_memory(semi->hp, semi->limit - semi->hp);

  HEAP_EVENT(heap, restarting_mutators);
  // fprintf(stderr, "%zd bytes copied\n", (space->size>>1)-(space->limit-space->hp));
}

static void collect_for_alloc(struct gc_mutator *mut, size_t bytes) {
  collect(mut, bytes);

  struct semi_space *space = mutator_semi_space(mut);
  if (bytes < space->limit - space->hp)
    return;

  struct gc_heap *heap = mutator_heap(mut);
  if (heap->options->common.heap_size_policy != GC_HEAP_SIZE_FIXED) {
    // Each collection can potentially resize only the inactive
    // fromspace, so if we really run out of space we will need to
    // collect again in order to resize the other half.
    collect(mut, bytes);
    if (bytes < space->limit - space->hp)
      return;
  }
  fprintf(stderr, "ran out of space, heap size %zu\n", heap->size);
  GC_CRASH();
}

void gc_collect(struct gc_mutator *mut,
                enum gc_collection_kind requested_kind) {
  // Ignore requested kind, because we always compact.
  collect(mut, 0);
}

int gc_object_is_old_generation_slow(struct gc_mutator *mut,
                                     struct gc_ref obj) {
  return 0;
}

void gc_write_barrier_slow(struct gc_mutator *mut, struct gc_ref obj,
                           size_t obj_size, struct gc_edge edge,
                           struct gc_ref new_val) {
}

int* gc_safepoint_flag_loc(struct gc_mutator *mut) { GC_CRASH(); }
void gc_safepoint_slow(struct gc_mutator *mut) { GC_CRASH(); }
  
static void collect_for_large_alloc(struct gc_mutator *mut, size_t npages) {
  collect_for_alloc(mut, npages * mutator_semi_space(mut)->page_size);
}

static void* allocate_large(struct gc_mutator *mut, size_t size) {
  struct gc_heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);
  struct semi_space *semi_space = heap_semi_space(heap);

  size_t npages = large_object_space_npages(space, size);
  while (!semi_space_steal_pages(semi_space, npages))
    collect_for_large_alloc(mut, npages);

  void *ret = large_object_space_alloc(space, npages, GC_TRACE_PRECISELY);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    GC_CRASH();
  }

  return ret;
}

void* gc_allocate_slow(struct gc_mutator *mut, size_t size,
                       enum gc_allocation_kind kind) {
  if (GC_UNLIKELY(kind != GC_ALLOCATION_TAGGED
                  && kind != GC_ALLOCATION_TAGGED_POINTERLESS)) {
    fprintf(stderr, "semispace collector cannot make allocations of kind %d\n",
            (int)kind);
    GC_CRASH();
  }

  if (size > gc_allocator_large_threshold())
    return allocate_large(mut, size);

  struct semi_space *space = mutator_semi_space(mut);
  while (1) {
    uintptr_t addr = space->hp;
    uintptr_t new_hp = align_up (addr + size, GC_ALIGNMENT);
    if (space->limit < new_hp) {
      // The factor of 2 is for both regions.
      collect_for_alloc(mut, size * 2);
      continue;
    }
    space->hp = new_hp;
    return (void *)addr;
  }
}

void gc_pin_object(struct gc_mutator *mut, struct gc_ref ref) {
  GC_CRASH();
}

struct gc_ephemeron* gc_allocate_ephemeron(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_ephemeron_size(), GC_ALLOCATION_TAGGED);
}

void gc_ephemeron_init(struct gc_mutator *mut, struct gc_ephemeron *ephemeron,
                       struct gc_ref key, struct gc_ref value) {
  gc_ephemeron_init_internal(mutator_heap(mut), ephemeron, key, value);
}

struct gc_finalizer* gc_allocate_finalizer(struct gc_mutator *mut) {
  return gc_allocate(mut, gc_finalizer_size(), GC_ALLOCATION_TAGGED);
}

void gc_finalizer_attach(struct gc_mutator *mut, struct gc_finalizer *finalizer,
                         unsigned priority, struct gc_ref object,
                         struct gc_ref closure) {
  gc_finalizer_init_internal(finalizer, object, closure);
  gc_finalizer_attach_internal(mutator_heap(mut)->finalizer_state,
                               finalizer, priority);
  // No write barrier.
}

struct gc_finalizer* gc_pop_finalizable(struct gc_mutator *mut) {
  return gc_finalizer_state_pop(mutator_heap(mut)->finalizer_state);
}

void gc_set_finalizer_callback(struct gc_heap *heap,
                               gc_finalizer_callback callback) {
  gc_finalizer_state_set_callback(heap->finalizer_state, callback);
}

static int region_init(struct region *region, size_t size) {
  region->base = 0;
  region->active_size = 0;
  region->mapped_size = 0;

  if (!grow_region_if_needed(region, size)) {
    fprintf(stderr, "failed to allocated %zu bytes\n", size);
    return 0;
  }

  region->active_size = size;

  return 1;
}

static int semi_space_init(struct semi_space *space, struct gc_heap *heap) {
  // Allocate even numbers of pages.
  size_t page_size = gc_platform_page_size();
  size_t size = align_up(heap->size, page_size * 2);

  space->page_size = page_size;
  space->stolen_pages = 0;

  if (!region_init(&space->from_space, size / 2))
    return 0;
  if (!region_init(&space->to_space, size / 2))
    return 0;

  space->hp = space->to_space.base;
  space->limit = space->hp + space->to_space.active_size;

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

static uint64_t get_allocation_counter(struct gc_heap *heap) {
  return heap->total_allocated_bytes_at_last_gc;
}

uint64_t gc_allocation_counter(struct gc_heap *heap) {
  return get_allocation_counter(heap);
}

static void ignore_async_heap_size_adjustment(struct gc_heap *heap,
                                              size_t size) {
}

static int heap_init(struct gc_heap *heap, const struct gc_options *options) {
  heap->extern_space = NULL;
  heap->pending_ephemerons_size_factor = 0.01;
  heap->pending_ephemerons_size_slop = 0.5;
  heap->count = 0;
  heap->options = options;
  heap->size = options->common.heap_size;
  heap->roots = NULL;
  heap->finalizer_state = gc_make_finalizer_state();
  if (!heap->finalizer_state)
    GC_CRASH();

  heap->sizer = gc_make_heap_sizer(heap, &options->common,
                                   get_allocation_counter,
                                   ignore_async_heap_size_adjustment,
                                   NULL);

  return heap_prepare_pending_ephemerons(heap);
}

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

int gc_init(const struct gc_options *options, struct gc_stack_addr stack_base,
            struct gc_heap **heap, struct gc_mutator **mut,
            struct gc_event_listener event_listener,
            void *event_listener_data) {
  GC_ASSERT_EQ(gc_allocator_allocation_pointer_offset(),
               offsetof(struct semi_space, hp));
  GC_ASSERT_EQ(gc_allocator_allocation_limit_offset(),
               offsetof(struct semi_space, limit));

  if (!options) options = gc_allocate_options();

  if (options->common.parallelism != 1)
    fprintf(stderr, "warning: parallelism unimplemented in semispace copying collector\n");

  *mut = calloc(1, sizeof(struct gc_mutator));
  if (!*mut) GC_CRASH();
  *heap = mutator_heap(*mut);

  if (!heap_init(*heap, options))
    return 0;

  (*heap)->event_listener = event_listener;
  (*heap)->event_listener_data = event_listener_data;
  HEAP_EVENT(*heap, init, (*heap)->size);

  if (!semi_space_init(heap_semi_space(*heap), *heap))
    return 0;
  struct gc_background_thread *thread = NULL;
  if (!large_object_space_init(heap_large_object_space(*heap), *heap, thread))
    return 0;
  
  // Ignore stack base, as we are precise.
  (*mut)->roots = NULL;

  (*mut)->event_listener_data =
    event_listener.mutator_added(event_listener_data);

  return 1;
}

void gc_mutator_set_roots(struct gc_mutator *mut,
                          struct gc_mutator_roots *roots) {
  mut->roots = roots;
}
void gc_heap_set_roots(struct gc_heap *heap, struct gc_heap_roots *roots) {
  heap->roots = roots;
}
void gc_heap_set_extern_space(struct gc_heap *heap,
                              struct gc_extern_space *space) {
  heap->extern_space = space;
}

struct gc_mutator* gc_init_for_thread(struct gc_stack_addr base,
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
