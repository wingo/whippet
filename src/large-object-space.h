#ifndef LARGE_OBJECT_SPACE_H
#define LARGE_OBJECT_SPACE_H

#include <pthread.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gc-assert.h"
#include "gc-ref.h"
#include "gc-conservative-ref.h"
#include "gc-trace.h"
#include "address-map.h"
#include "address-set.h"
#include "background-thread.h"
#include "freelist.h"

// A mark-sweep space with generational support.

struct gc_heap;

enum large_object_state {
  LARGE_OBJECT_NURSERY = 0,
  LARGE_OBJECT_MARKED_BIT = 1,
  LARGE_OBJECT_MARK_TOGGLE_BIT = 2,
  LARGE_OBJECT_MARK_0 = LARGE_OBJECT_MARKED_BIT,
  LARGE_OBJECT_MARK_1 = LARGE_OBJECT_MARKED_BIT | LARGE_OBJECT_MARK_TOGGLE_BIT
};

struct large_object {
  uintptr_t addr;
  size_t size;
};
struct large_object_node;
struct large_object_live_data {
  uint8_t mark;
  enum gc_trace_kind trace;
};
struct large_object_dead_data {
  uint8_t age;
  struct large_object_node **prev;
  struct large_object_node *next;
};
struct large_object_data {
  uint8_t is_live;
  union {
    struct large_object_live_data live;
    struct large_object_dead_data dead;
  };
};

#define SPLAY_TREE_PREFIX large_object_
typedef struct large_object large_object_key_span;
typedef uintptr_t large_object_key;
typedef struct large_object_data large_object_value;
static inline int
large_object_compare(uintptr_t addr, struct large_object obj) {
  if (addr < obj.addr) return -1;
  if (addr - obj.addr < obj.size) return 0;
  return 1;
}
static inline uintptr_t
large_object_span_start(struct large_object obj) {
  return obj.addr;
}
#include "splay-tree.h"

DEFINE_FREELIST(large_object_freelist, sizeof(uintptr_t) * 8 - 1, 2,
                struct large_object_node*);

struct large_object_space {
  // Lock for object_map, quarantine, nursery, and marked.
  pthread_mutex_t lock;
  // Lock for object_tree.
  pthread_mutex_t object_tree_lock;
  // Lock for remembered_edges.
  pthread_mutex_t remembered_edges_lock;
  // Locking order: You must hold the space lock when taking
  // object_tree_lock.  Take no other lock while holding
  // object_tree_lock.  remembered_edges_lock is a leaf; take no locks
  // when holding it.

  // The value for a large_object_node's "mark" field indicating a
  // marked object; always nonzero, and alternating between two values
  // at every major GC.
  uint8_t marked;

  // Splay tree of objects, keyed by <addr, size> tuple.  Useful when
  // looking up object-for-address.
  struct large_object_tree object_tree;

  // Hash table of objects, where values are pointers to splay tree
  // nodes.  Useful when you have the object address and just want to
  // check something about it (for example its size).
  struct address_map object_map;

  // In generational configurations, we collect all allocations in the
  // last cycle into the nursery.
  struct address_map nursery;

  // Size-segregated freelist of dead objects.  Allocations are first
  // served from the quarantine freelist before falling back to the OS
  // if needed.  Collected objects spend a second or two in quarantine
  // before being returned to the OS.  This is an optimization to avoid
  // mucking about too much with the TLB and so on.
  struct large_object_freelist quarantine;

  // Set of edges from lospace that may reference young objects,
  // possibly in other spaces.
  struct address_set remembered_edges;

  size_t page_size;
  size_t page_size_log2;
  size_t total_pages;
  size_t free_pages;
  size_t live_pages_at_last_collection;
  size_t pages_freed_by_last_collection;
  int synchronous_release;
};

static size_t
large_object_space_npages(struct large_object_space *space, size_t bytes) {
  return (bytes + space->page_size - 1) >> space->page_size_log2;
}

static size_t
large_object_space_size_at_last_collection(struct large_object_space *space) {
  return space->live_pages_at_last_collection << space->page_size_log2;
}

static inline int
large_object_space_contains_with_lock(struct large_object_space *space,
                                      struct gc_ref ref) {
  return address_map_contains(&space->object_map, gc_ref_value(ref));
}

static inline int
large_object_space_contains(struct large_object_space *space,
                            struct gc_ref ref) {
  pthread_mutex_lock(&space->lock);
  int ret = large_object_space_contains_with_lock(space, ref);
  pthread_mutex_unlock(&space->lock);
  return ret;
}

static inline struct gc_ref
large_object_space_object_containing_edge(struct large_object_space *space,
                                          struct gc_edge edge) {
  pthread_mutex_lock(&space->object_tree_lock);
  struct large_object_node *node =
    large_object_tree_lookup(&space->object_tree, gc_edge_address(edge));
  uintptr_t addr = (node && node->value.is_live) ? node->key.addr : 0;
  pthread_mutex_unlock(&space->object_tree_lock);
  return gc_ref(addr);
}

static void
large_object_space_start_gc(struct large_object_space *space, int is_minor_gc) {
  // Take the space lock to prevent
  // large_object_space_process_quarantine from concurrently mutating
  // the object map.
  pthread_mutex_lock(&space->lock);
  if (!is_minor_gc) {
    space->marked ^= LARGE_OBJECT_MARK_TOGGLE_BIT;
    space->live_pages_at_last_collection = 0;
  }
}

static inline struct gc_trace_plan
large_object_space_object_trace_plan(struct large_object_space *space,
                                     struct gc_ref ref) {
  uintptr_t node_bits =
    address_map_lookup(&space->object_map, gc_ref_value(ref), 0);
  GC_ASSERT(node_bits);
  struct large_object_node *node = (struct large_object_node*) node_bits;
  switch (node->value.live.trace) {
    case GC_TRACE_PRECISELY:
      return (struct gc_trace_plan){ GC_TRACE_PRECISELY, };
    case GC_TRACE_NONE:
      return (struct gc_trace_plan){ GC_TRACE_NONE, };
#if GC_CONSERVATIVE_TRACE
    case GC_TRACE_CONSERVATIVELY: {
      return (struct gc_trace_plan){ GC_TRACE_CONSERVATIVELY, node->key.size };
    }
    // No large ephemerons.
#endif
    default:
      GC_CRASH();
  }
}

static uint8_t*
large_object_node_mark_loc(struct large_object_node *node) {
  GC_ASSERT(node->value.is_live);
  return &node->value.live.mark;
}

static uint8_t
large_object_node_get_mark(struct large_object_node *node) {
  return atomic_load_explicit(large_object_node_mark_loc(node),
                              memory_order_acquire);
}

static struct large_object_node*
large_object_space_lookup(struct large_object_space *space, struct gc_ref ref) {
  return (struct large_object_node*) address_map_lookup(&space->object_map,
                                                        gc_ref_value(ref),
                                                        0);
}

static int
large_object_space_mark(struct large_object_space *space, struct gc_ref ref) {
  struct large_object_node *node = large_object_space_lookup(space, ref);
  if (!node)
    return 0;
  GC_ASSERT(node->value.is_live);

  uint8_t *loc = large_object_node_mark_loc(node);
  uint8_t mark = atomic_load_explicit(loc, memory_order_relaxed);
  do {
    if (mark == space->marked)
      return 0;
  } while (!atomic_compare_exchange_weak_explicit(loc, &mark, space->marked,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));

  size_t pages = node->key.size >> space->page_size_log2;
  atomic_fetch_add(&space->live_pages_at_last_collection, pages);

  return 1;
}

static int
large_object_space_is_marked(struct large_object_space *space,
                             struct gc_ref ref) {
  struct large_object_node *node = large_object_space_lookup(space, ref);
  if (!node)
    return 0;
  GC_ASSERT(node->value.is_live);

  return atomic_load_explicit(large_object_node_mark_loc(node),
                              memory_order_acquire) == space->marked;
}

static int
large_object_space_is_survivor(struct large_object_space *space,
                               struct gc_ref ref) {
  GC_ASSERT(large_object_space_contains(space, ref));
  pthread_mutex_lock(&space->lock);
  int old = large_object_space_is_marked(space, ref);
  pthread_mutex_unlock(&space->lock);
  return old;
}

static int
large_object_space_remember_edge(struct large_object_space *space,
                                 struct gc_ref obj,
                                 struct gc_edge edge) {
  GC_ASSERT(large_object_space_contains(space, obj));
  if (!large_object_space_is_survivor(space, obj))
    return 0;

  uintptr_t edge_addr = gc_edge_address(edge);
  int remembered = 0;
  pthread_mutex_lock(&space->remembered_edges_lock);
  if (!address_set_contains(&space->remembered_edges, edge_addr)) {
    address_set_add(&space->remembered_edges, edge_addr);
    remembered = 1;
  }
  pthread_mutex_unlock(&space->remembered_edges_lock);
  return remembered;
}

static void
large_object_space_forget_edge(struct large_object_space *space,
                               struct gc_edge edge) {
  uintptr_t edge_addr = gc_edge_address(edge);
  pthread_mutex_lock(&space->remembered_edges_lock);
  GC_ASSERT(address_set_contains(&space->remembered_edges, edge_addr));
  address_set_remove(&space->remembered_edges, edge_addr);
  pthread_mutex_unlock(&space->remembered_edges_lock);
}

static void
large_object_space_clear_remembered_edges(struct large_object_space *space) {
  address_set_clear(&space->remembered_edges);
}

static void
large_object_space_add_to_freelist(struct large_object_space *space,
                                   struct large_object_node *node) {
  node->value.is_live = 0;
  struct large_object_dead_data *data = &node->value.dead;
  memset(data, 0, sizeof(*data));
  data->age = 0;
  struct large_object_node **bucket =
    large_object_freelist_bucket(&space->quarantine, node->key.size);
  data->next = *bucket;
  if (data->next)
    data->next->value.dead.prev = &data->next;
  data->prev = bucket;
  *bucket = node;
}

static void
large_object_space_remove_from_freelist(struct large_object_space *space,
                                        struct large_object_node *node) {
  GC_ASSERT(!node->value.is_live);
  struct large_object_dead_data *dead = &node->value.dead;
  GC_ASSERT(dead->prev);
  if (dead->next)
    dead->next->value.dead.prev = dead->prev;
  *dead->prev = dead->next;
  dead->prev = NULL;
  dead->next = NULL;
}

static void
large_object_space_sweep_one(uintptr_t addr, uintptr_t node_bits,
                             void *data) {
  struct large_object_space *space = data;
  struct large_object_node *node = (struct large_object_node*) node_bits;
  if (!node->value.is_live)
    return;
  GC_ASSERT(node->value.is_live);
  uint8_t mark = atomic_load_explicit(large_object_node_mark_loc(node),
                                      memory_order_acquire);
  if (mark != space->marked)
    large_object_space_add_to_freelist(space, node);
}

static void
large_object_space_process_quarantine(void *data) {
  struct large_object_space *space = data;
  pthread_mutex_lock(&space->lock);
  pthread_mutex_lock(&space->object_tree_lock);
  for (size_t idx = 0; idx < large_object_freelist_num_size_classes(); idx++) {
    struct large_object_node **link = &space->quarantine.buckets[idx];
    for (struct large_object_node *node = *link; node; node = *link) {
      GC_ASSERT(!node->value.is_live);
      if (++node->value.dead.age < 2) {
        link = &node->value.dead.next;
      } else {
        struct large_object obj = node->key;
        large_object_space_remove_from_freelist(space, node);
        address_map_remove(&space->object_map, obj.addr);
        large_object_tree_remove(&space->object_tree, obj.addr);
        gc_platform_release_memory((void*)obj.addr, obj.size);
      }
    }
  }
  pthread_mutex_unlock(&space->object_tree_lock);
  pthread_mutex_unlock(&space->lock);
}

static void
large_object_space_finish_gc(struct large_object_space *space,
                             int is_minor_gc) {
  if (GC_GENERATIONAL) {
    address_map_for_each(is_minor_gc ? &space->nursery : &space->object_map,
                         large_object_space_sweep_one,
                         space);
    address_map_clear(&space->nursery);
  } else {
    address_map_for_each(&space->object_map,
                         large_object_space_sweep_one,
                         space);
  }
  size_t free_pages =
    space->total_pages - space->live_pages_at_last_collection;
  space->pages_freed_by_last_collection = free_pages - space->free_pages;
  space->free_pages = free_pages;
  pthread_mutex_unlock(&space->lock);
  if (space->synchronous_release)
    large_object_space_process_quarantine(space);
}

static void
large_object_space_add_to_allocation_counter(struct large_object_space *space,
                                             uint64_t *counter) {
  size_t pages = space->total_pages - space->free_pages;
  pages -= space->live_pages_at_last_collection;
  *counter += pages << space->page_size_log2;
}

static inline struct gc_ref
large_object_space_mark_conservative_ref(struct large_object_space *space,
                                         struct gc_conservative_ref ref,
                                         int possibly_interior) {
  uintptr_t addr = gc_conservative_ref_value(ref);

  if (!possibly_interior) {
    // Addr not aligned on page boundary?  Not a large object.
    // Otherwise strip the displacement to obtain the true base address.
    uintptr_t displacement = addr & (space->page_size - 1);
    if (!gc_is_valid_conservative_ref_displacement(displacement))
      return gc_ref_null();
    addr -= displacement;
  }

  struct large_object_node *node;
  if (possibly_interior) {
    pthread_mutex_lock(&space->object_tree_lock);
    node = large_object_tree_lookup(&space->object_tree, addr);
    pthread_mutex_unlock(&space->object_tree_lock);
  } else {
    node = large_object_space_lookup(space, gc_ref(addr));
  }

  if (node && node->value.is_live &&
      large_object_space_mark(space, gc_ref(node->key.addr)))
    return gc_ref(node->key.addr);

  return gc_ref_null();
}

static void*
large_object_space_alloc(struct large_object_space *space, size_t npages,
                         enum gc_trace_kind trace) {
  void *ret = NULL;
  pthread_mutex_lock(&space->lock);
  
  size_t size = npages << space->page_size_log2;
  for (size_t idx = large_object_freelist_size_class(size);
       idx < large_object_freelist_num_size_classes();
       idx++) {
    struct large_object_node *node = space->quarantine.buckets[idx];
    while (node && node->key.size < size)
      node = node->value.dead.next;
    if (node) {
      // We found a suitable hole in quarantine.  Unlink it from the
      // freelist.
      large_object_space_remove_from_freelist(space, node);

      // Mark the hole as live.
      node->value.is_live = 1;
      memset(&node->value.live, 0, sizeof(node->value.live));
      node->value.live.mark = LARGE_OBJECT_NURSERY;
      node->value.live.trace = trace;

      // If the hole is actually too big, trim its tail.
      if (node->key.size > size) {
        struct large_object tail = {node->key.addr + size, node->key.size - size};
        struct large_object_data tail_value = {0,};
        node->key.size = size;
        pthread_mutex_lock(&space->object_tree_lock);
        struct large_object_node *tail_node =
          large_object_tree_insert(&space->object_tree, tail, tail_value);
        pthread_mutex_unlock(&space->object_tree_lock);
        uintptr_t tail_node_bits = (uintptr_t)tail_node;
        address_map_add(&space->object_map, tail_node->key.addr,
                        tail_node_bits);
        large_object_space_add_to_freelist(space, tail_node);
      }

      // Add the object to the nursery.
      if (GC_GENERATIONAL)
        address_map_add(&space->nursery, node->key.addr, (uintptr_t)node);
    
      space->free_pages -= npages;
      ret = (void*)node->key.addr;
      memset(ret, 0, size);
      break;
    }
  }

  // If we didn't find anything in the quarantine, get fresh pages from the OS.
  if (!ret) {
    ret = gc_platform_acquire_memory(size, 0);
    if (ret) {
      uintptr_t addr = (uintptr_t)ret;
      struct large_object k = { addr, size };
      struct large_object_data v = {0,};
      v.is_live = 1;
      v.live.mark = LARGE_OBJECT_NURSERY;
      v.live.trace = trace;

      pthread_mutex_lock(&space->object_tree_lock);
      struct large_object_node *node =
        large_object_tree_insert(&space->object_tree, k, v);
      uintptr_t node_bits = (uintptr_t)node;
      address_map_add(&space->object_map, addr, node_bits);
      space->total_pages += npages;
      pthread_mutex_unlock(&space->object_tree_lock);
    }
  }

  pthread_mutex_unlock(&space->lock);
  return ret;
}

static int
large_object_space_init(struct large_object_space *space,
                        struct gc_heap *heap,
                        struct gc_background_thread *thread) {
  memset(space, 0, sizeof(*space));
  pthread_mutex_init(&space->lock, NULL);
  pthread_mutex_init(&space->object_tree_lock, NULL);
  pthread_mutex_init(&space->remembered_edges_lock, NULL);

  space->page_size = getpagesize();
  space->page_size_log2 = __builtin_ctz(space->page_size);

  space->marked = LARGE_OBJECT_MARK_0;

  large_object_tree_init(&space->object_tree);
  address_map_init(&space->object_map);
  address_map_init(&space->nursery);
  large_object_freelist_init(&space->quarantine);

  address_set_init(&space->remembered_edges);

  if (thread)
    gc_background_thread_add_task(thread, GC_BACKGROUND_TASK_START,
                                  large_object_space_process_quarantine,
                                  space);
  else
    space->synchronous_release = 1;

  return 1;
}

#endif // LARGE_OBJECT_SPACE_H
