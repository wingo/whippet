#ifndef LARGE_OBJECT_SPACE_H
#define LARGE_OBJECT_SPACE_H

#include <pthread.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gc-assert.h"
#include "gc-ref.h"
#include "gc-conservative-ref.h"
#include "address-map.h"
#include "address-set.h"

// Logically the large object space is a treadmill space -- somewhat like a
// copying collector, in that we allocate into tospace, and collection flips
// tospace to fromspace, except that we just keep a record on the side of which
// objects are in which space.  That way we slot into the abstraction of a
// copying collector while not actually copying data.

struct gc_heap;

struct large_object_space {
  pthread_mutex_t lock;

  size_t page_size;
  size_t page_size_log2;
  size_t total_pages;
  size_t free_pages;
  size_t live_pages_at_last_collection;
  size_t pages_freed_by_last_collection;

  struct address_set from_space;
  struct address_set to_space;
  struct address_set free_space;
  struct address_map object_pages; // for each object: size in pages.
  struct address_map predecessors; // subsequent addr -> object addr
};

static int large_object_space_init(struct large_object_space *space,
                                   struct gc_heap *heap) {
  pthread_mutex_init(&space->lock, NULL);
  space->page_size = getpagesize();
  space->page_size_log2 = __builtin_ctz(space->page_size);
  address_set_init(&space->from_space);
  address_set_init(&space->to_space);
  address_set_init(&space->free_space);
  address_map_init(&space->object_pages);
  address_map_init(&space->predecessors);
  return 1;
}

static size_t large_object_space_npages(struct large_object_space *space,
                                       size_t bytes) {
  return (bytes + space->page_size - 1) >> space->page_size_log2;
}

static void large_object_space_clear_one_remembered(uintptr_t addr,
                                                    void *unused) {
  struct gc_ref ref = gc_ref(addr);
  if (gc_object_is_remembered_nonatomic(ref))
    gc_object_clear_remembered_nonatomic(ref);
}

static void
large_object_space_clear_remembered_set(struct large_object_space *space) {
  if (!GC_GENERATIONAL)
    return;
  address_set_for_each(&space->to_space,
                       large_object_space_clear_one_remembered, NULL);
}

struct large_object_space_trace_remembered_data {
  void (*trace)(struct gc_ref, struct gc_heap*);
  struct gc_heap *heap;
};

static void large_object_space_trace_one_remembered(uintptr_t addr,
                                                    void *data) {
  struct gc_ref ref = gc_ref(addr);
  if (gc_object_is_remembered_nonatomic(ref)) {
    gc_object_clear_remembered_nonatomic(ref);
    struct large_object_space_trace_remembered_data *vdata = data;
    vdata->trace(ref, vdata->heap);
  }
}

static void
large_object_space_trace_remembered_set(struct large_object_space *space,
                                        void (*trace)(struct gc_ref,
                                                      struct gc_heap*),
                                        struct gc_heap *heap) {
  struct large_object_space_trace_remembered_data vdata = { trace, heap };

  if (!GC_GENERATIONAL)
    return;
  address_set_for_each(&space->to_space,
                       large_object_space_trace_one_remembered, &vdata);
}

static void large_object_space_start_gc(struct large_object_space *space,
                                        int is_minor_gc) {
  if (is_minor_gc)
    return;

  // Flip.  Note that when we flip, fromspace is empty, but it might have
  // allocated storage, so we do need to do a proper swap.
  struct address_set tmp;
  memcpy(&tmp, &space->from_space, sizeof(tmp));
  memcpy(&space->from_space, &space->to_space, sizeof(tmp));
  memcpy(&space->to_space, &tmp, sizeof(tmp));
  space->live_pages_at_last_collection = 0;
}

static int large_object_space_copy(struct large_object_space *space,
                                   struct gc_ref ref) {
  int copied = 0;
  uintptr_t addr = gc_ref_value(ref);
  pthread_mutex_lock(&space->lock);
  if (!address_set_contains(&space->from_space, addr))
    // Already copied; object is grey or white.
    goto done;
  space->live_pages_at_last_collection +=
    address_map_lookup(&space->object_pages, addr, 0);
  address_set_remove(&space->from_space, addr);
  address_set_add(&space->to_space, addr);
  // Object should be placed on mark stack for visiting its fields.  (While on
  // mark stack it's actually grey, not black.)
  copied = 1;
done:
  pthread_mutex_unlock(&space->lock);
  return copied;
}

static int large_object_space_is_copied(struct large_object_space *space,
                                        struct gc_ref ref) {
  int copied = 0;
  uintptr_t addr = gc_ref_value(ref);
  pthread_mutex_lock(&space->lock);
  copied = address_set_contains(&space->from_space, addr);
  pthread_mutex_unlock(&space->lock);
  return copied;
}

static int large_object_space_mark_object(struct large_object_space *space,
                                          struct gc_ref ref) {
  return large_object_space_copy(space, ref);
}

static inline size_t large_object_space_object_size(struct large_object_space *space,
                                                    struct gc_ref ref) {
  size_t npages = address_map_lookup(&space->object_pages,
                                     gc_ref_value(ref), 0);
  GC_ASSERT(npages != 0);
  return npages * space->page_size;
}

static void large_object_space_reclaim_one(uintptr_t addr, void *data) {
  struct large_object_space *space = data;
  size_t npages = address_map_lookup(&space->object_pages, addr, 0);
  // Release the pages to the OS, and cause them to be zero on next use.
  madvise((void*) addr, npages * space->page_size, MADV_DONTNEED);
  size_t did_merge = 0;
  uintptr_t pred = address_map_lookup(&space->predecessors, addr, 0);
  uintptr_t succ = addr + npages * space->page_size;
  if (pred && address_set_contains(&space->free_space, pred)) {
    // Merge with free predecessor.
    address_map_remove(&space->predecessors, addr);
    address_map_remove(&space->object_pages, addr);
    addr = pred;
    npages += address_map_lookup(&space->object_pages, addr, 0);
    did_merge = 1;
  } else {
    // Otherwise this is a new free object.
    address_set_add(&space->free_space, addr);
  }
  if (address_set_contains(&space->free_space, succ)) {
    // Merge with free successor.
    size_t succ_npages = address_map_lookup(&space->object_pages, succ, 0);
    address_map_remove(&space->predecessors, succ);
    address_map_remove(&space->object_pages, succ);
    address_set_remove(&space->free_space, succ);
    npages += succ_npages;
    succ += succ_npages * space->page_size;
    did_merge = 1;
  }
  if (did_merge) {
    // Update extents.
    address_map_add(&space->object_pages, addr, npages);
    address_map_add(&space->predecessors, succ, addr);
  }
}

static void large_object_space_finish_gc(struct large_object_space *space,
                                         int is_minor_gc) {
  pthread_mutex_lock(&space->lock);
  if (is_minor_gc) {
    space->live_pages_at_last_collection =
      space->total_pages - space->free_pages;
    space->pages_freed_by_last_collection = 0;
  } else {
    address_set_for_each(&space->from_space, large_object_space_reclaim_one,
                         space);
    address_set_clear(&space->from_space);
    size_t free_pages =
      space->total_pages - space->live_pages_at_last_collection;
    space->pages_freed_by_last_collection = free_pages - space->free_pages;
    space->free_pages = free_pages;
  }
  pthread_mutex_unlock(&space->lock);
}

static inline struct gc_ref
large_object_space_mark_conservative_ref(struct large_object_space *space,
                                         struct gc_conservative_ref ref,
                                         int possibly_interior) {
  uintptr_t addr = gc_conservative_ref_value(ref);

  if (possibly_interior) {
    // FIXME: This only allows interior pointers within the first page.
    // BDW-GC doesn't have all-interior-pointers on for intraheap edges
    // or edges originating in static data but by default does allow
    // them from stack edges; probably we should too.
    addr &= ~(space->page_size - 1);
  } else {
    // Addr not aligned on page boundary?  Not a large object.
    uintptr_t displacement = addr & (space->page_size - 1);
    if (!gc_is_valid_conservative_ref_displacement(displacement))
      return gc_ref_null();
    addr -= displacement;
  }

  pthread_mutex_lock(&space->lock);
  // ptr might be in fromspace or tospace.  Just check the object_pages table, which
  // contains both, as well as object_pages for free blocks.
  int found = address_map_contains(&space->object_pages, addr);
  pthread_mutex_unlock(&space->lock);

  if (found && large_object_space_copy(space, gc_ref(addr)))
    return gc_ref(addr);

  return gc_ref_null();
}

static inline int large_object_space_contains(struct large_object_space *space,
                                              struct gc_ref ref) {
  pthread_mutex_lock(&space->lock);
  // ptr might be in fromspace or tospace.  Just check the object_pages table, which
  // contains both, as well as object_pages for free blocks.
  int ret = address_map_contains(&space->object_pages, gc_ref_value(ref));
  pthread_mutex_unlock(&space->lock);
  return ret;
}

struct large_object_space_candidate {
  struct large_object_space *space;
  size_t min_npages;
  uintptr_t addr;
  size_t npages;
};

static int large_object_space_best_fit(uintptr_t addr, void *data) {
  struct large_object_space_candidate *found = data;
  size_t npages = address_map_lookup(&found->space->object_pages, addr, 0);
  if (npages < found->min_npages)
    return 0;
  if (npages >= found->npages)
    return 0;
  found->addr = addr;
  found->npages = npages;
  return found->min_npages == npages;
}
    
static void* large_object_space_alloc(struct large_object_space *space,
                                      size_t npages) {
  void *ret;
  pthread_mutex_lock(&space->lock);
  ret = NULL;
  struct large_object_space_candidate found = { space, npages, 0, -1 };
  address_set_find(&space->free_space, large_object_space_best_fit, &found);
  if (found.addr) {
    uintptr_t addr = found.addr;
    ret = (void*)addr;
    address_set_remove(&space->free_space, addr);
    address_set_add(&space->to_space, addr);

    if (found.npages > npages) {
      uintptr_t succ = addr + npages * space->page_size;
      uintptr_t succ_succ = succ + (found.npages - npages) * space->page_size;
      address_map_add(&space->object_pages, addr, npages);
      address_map_add(&space->object_pages, succ, found.npages - npages);
      address_set_add(&space->free_space, succ);
      address_map_add(&space->predecessors, succ, addr);
      address_map_add(&space->predecessors, succ_succ, succ);
    }
    space->free_pages -= npages;
  }
  pthread_mutex_unlock(&space->lock);
  return ret;
}

static void*
large_object_space_obtain_and_alloc(struct large_object_space *space,
                                    size_t npages) {
  size_t bytes = npages * space->page_size;
  void *ret = mmap(NULL, bytes, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (ret == MAP_FAILED)
    return NULL;

  uintptr_t addr = (uintptr_t)ret;
  pthread_mutex_lock(&space->lock);
  address_map_add(&space->object_pages, addr, npages);
  address_map_add(&space->predecessors, addr + bytes, addr);
  address_set_add(&space->to_space, addr);
  space->total_pages += npages;
  pthread_mutex_unlock(&space->lock);

  return ret;
}

#endif // LARGE_OBJECT_SPACE_H
