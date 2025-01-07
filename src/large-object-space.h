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
#include "address-map.h"
#include "address-set.h"
#include "background-thread.h"
#include "freelist.h"

// Logically the large object space is a treadmill space -- somewhat like a
// copying collector, in that we allocate into tospace, and collection flips
// tospace to fromspace, except that we just keep a record on the side of which
// objects are in which space.  That way we slot into the abstraction of a
// copying collector while not actually copying data.

struct gc_heap;

struct large_object {
  uintptr_t addr;
  size_t size;
};
struct large_object_node;
struct large_object_live_data {
  uint8_t is_survivor;
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
  // Access to all members protected by lock.
  pthread_mutex_t lock;

  // Splay tree of objects, keyed by <addr, size> tuple.  Useful when
  // looking up object-for-address.
  struct large_object_tree object_tree;
  // Hash table of objects, where values are pointers to splay tree
  // nodes.  Useful when you have the object address and just want to
  // check something about it (for example its size).
  struct address_map object_map;

  // Size-segregated freelist of dead objects.  Allocations are first
  // served from the quarantine freelist before falling back to the OS
  // if needed.  Collected objects spend a second or two in quarantine
  // before being returned to the OS.  This is an optimization to avoid
  // mucking about too much with the TLB and so on.
  struct large_object_freelist quarantine;

  size_t page_size;
  size_t page_size_log2;
  size_t total_pages;
  size_t free_pages;
  size_t live_pages_at_last_collection;
  size_t pages_freed_by_last_collection;
  int synchronous_release;

  // A partition of the set of live objects into three sub-spaces.  If
  // all collections are major, the survivor space will always be empty.
  // The values of these maps are splay tree nodes.
  struct address_map from_space;
  struct address_map to_space;
  struct address_map survivor_space;

  // Set of edges from lospace that may reference young objects,
  // possibly in other spaces.
  struct address_set remembered_edges;
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
large_object_space_contains(struct large_object_space *space,
                            struct gc_ref ref) {
  pthread_mutex_lock(&space->lock);
  int ret = address_map_contains(&space->object_map, gc_ref_value(ref));
  pthread_mutex_unlock(&space->lock);
  return ret;
}

static inline struct gc_ref
large_object_space_object_containing_edge(struct large_object_space *space,
                                          struct gc_edge edge) {
  pthread_mutex_lock(&space->lock);
  struct large_object_node *node =
    large_object_tree_lookup(&space->object_tree, gc_edge_address(edge));
  uintptr_t addr = (node && node->value.is_live) ? node->key.addr : 0;
  pthread_mutex_unlock(&space->lock);
  return gc_ref(addr);
}

static void
large_object_space_flip_survivor(uintptr_t addr, uintptr_t node_bits,
                                 void *data) {
  struct large_object_space *space = data;
  struct large_object_node *node = (void*)node_bits;
  GC_ASSERT(node->value.is_live && node->value.live.is_survivor);
  node->value.live.is_survivor = 0;
  address_map_add(&space->from_space, addr, (uintptr_t)node);
}

static void
large_object_space_start_gc(struct large_object_space *space, int is_minor_gc) {
  // Flip.  Note that when we flip, fromspace is empty, but it might have
  // allocated storage, so we do need to do a proper swap.
  struct address_map tmp;
  memcpy(&tmp, &space->from_space, sizeof(tmp));
  memcpy(&space->from_space, &space->to_space, sizeof(tmp));
  memcpy(&space->to_space, &tmp, sizeof(tmp));
  
  if (!is_minor_gc) {
    address_map_for_each(&space->survivor_space,
                         large_object_space_flip_survivor, space);
    address_map_clear(&space->survivor_space);
    space->live_pages_at_last_collection = 0;
  }
}

static inline size_t
large_object_space_object_size(struct large_object_space *space,
                               struct gc_ref ref) {
  uintptr_t node_bits =
    address_map_lookup(&space->object_map, gc_ref_value(ref), 0);
  GC_ASSERT(node_bits);
  struct large_object_node *node = (struct large_object_node*) node_bits;
  return node->key.size;
}

static void
large_object_space_do_copy(struct large_object_space *space,
                           struct large_object_node *node) {
  GC_ASSERT(address_map_contains(&space->from_space, node->key.addr));
  GC_ASSERT(node->value.is_live);
  GC_ASSERT(!node->value.live.is_survivor);
  uintptr_t addr = node->key.addr;
  size_t bytes = node->key.size;
  uintptr_t node_bits = (uintptr_t)node;
  space->live_pages_at_last_collection += bytes >> space->page_size_log2;
  address_map_remove(&space->from_space, addr);
  if (GC_GENERATIONAL) {
    node->value.live.is_survivor = 1;
    address_map_add(&space->survivor_space, addr, node_bits);
  } else {
    address_map_add(&space->to_space, addr, node_bits);
  }
}

static int
large_object_space_copy(struct large_object_space *space, struct gc_ref ref) {
  int copied = 0;
  uintptr_t addr = gc_ref_value(ref);
  pthread_mutex_lock(&space->lock);
  uintptr_t node_bits = address_map_lookup(&space->from_space, addr, 0);
  if (node_bits) {
    large_object_space_do_copy(space, (struct large_object_node*) node_bits);
    // Object is grey; place it on mark stack to visit its fields.
    copied = 1;
  }
  pthread_mutex_unlock(&space->lock);
  return copied;
}

static int
large_object_space_is_copied(struct large_object_space *space,
                             struct gc_ref ref) {
  GC_ASSERT(large_object_space_contains(space, ref));
  int copied = 0;
  uintptr_t addr = gc_ref_value(ref);
  pthread_mutex_lock(&space->lock);
  copied = !address_map_contains(&space->from_space, addr);
  pthread_mutex_unlock(&space->lock);
  return copied;
}

static int
large_object_space_is_survivor_with_lock(struct large_object_space *space,
                                         struct gc_ref ref) {
  return address_map_contains(&space->survivor_space, gc_ref_value(ref));
}

static int
large_object_space_is_survivor(struct large_object_space *space,
                               struct gc_ref ref) {
  GC_ASSERT(large_object_space_contains(space, ref));
  pthread_mutex_lock(&space->lock);
  int old = large_object_space_is_survivor_with_lock(space, ref);
  pthread_mutex_unlock(&space->lock);
  return old;
}

static int
large_object_space_remember_edge(struct large_object_space *space,
                                 struct gc_ref obj,
                                 struct gc_edge edge) {
  GC_ASSERT(large_object_space_contains(space, obj));
  int remembered = 0;
  uintptr_t edge_addr = gc_edge_address(edge);
  pthread_mutex_lock(&space->lock);
  if (large_object_space_is_survivor_with_lock(space, obj)
      && !address_set_contains(&space->remembered_edges, edge_addr)) {
    address_set_add(&space->remembered_edges, edge_addr);
    remembered = 1;
  }
  pthread_mutex_unlock(&space->lock);
  return remembered;
}

static void
large_object_space_clear_remembered_edges(struct large_object_space *space) {
  address_set_clear(&space->remembered_edges);
}

static int large_object_space_mark_object(struct large_object_space *space,
                                          struct gc_ref ref) {
  return large_object_space_copy(space, ref);
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
}

static void
large_object_space_reclaim_one(uintptr_t addr, uintptr_t node_bits,
                               void *data) {
  struct large_object_space *space = data;
  struct large_object_node *node = (struct large_object_node*) node_bits;
  GC_ASSERT(node->value.is_live);
  large_object_space_add_to_freelist(space, node);
}

static void
large_object_space_process_quarantine(void *data) {
  struct large_object_space *space = data;
  pthread_mutex_lock(&space->lock);
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
  pthread_mutex_unlock(&space->lock);
}

static void
large_object_space_finish_gc(struct large_object_space *space,
                             int is_minor_gc) {
  pthread_mutex_lock(&space->lock);
  address_map_for_each(&space->from_space, large_object_space_reclaim_one,
                       space);
  address_map_clear(&space->from_space);
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

  pthread_mutex_lock(&space->lock);
  struct large_object_node *node = NULL;
  if (possibly_interior) {
    node = large_object_tree_lookup(&space->object_tree, addr);
    if (node && !address_map_contains(&space->from_space, node->key.addr))
      node = NULL;
  } else {
    uintptr_t node_bits = address_map_lookup(&space->from_space, addr, 0);
    node = (struct large_object_node*) node_bits;
  }
  struct gc_ref ret = gc_ref_null();
  if (node) {
    large_object_space_do_copy(space, node);
    ret = gc_ref(node->key.addr);
  }
  pthread_mutex_unlock(&space->lock);

  return ret;
}

static void*
large_object_space_alloc(struct large_object_space *space, size_t npages) {
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

      // If the hole is actually too big, trim its tail.
      if (node->key.size > size) {
        struct large_object tail = {node->key.addr + size, node->key.size - size};
        struct large_object_data tail_value = {0,};
        node->key.size = size;
        struct large_object_node *tail_node =
          large_object_tree_insert(&space->object_tree, tail, tail_value);
        large_object_space_add_to_freelist(space, tail_node);
      }

      // Add the object to tospace.
      address_map_add(&space->to_space, node->key.addr, (uintptr_t)node);
    
      space->free_pages -= npages;
      ret = (void*)node->key.addr;
      break;
    }
  }
  pthread_mutex_unlock(&space->lock);
  return ret;
}

static void*
large_object_space_obtain_and_alloc(struct large_object_space *space,
                                    size_t npages) {
  size_t bytes = npages * space->page_size;
  void *ret = gc_platform_acquire_memory(bytes, 0);
  if (!ret)
    return NULL;

  uintptr_t addr = (uintptr_t)ret;
  struct large_object k = { addr, bytes };
  struct large_object_data v = {0,};
  v.is_live = 1;
  v.live.is_survivor = 0;

  pthread_mutex_lock(&space->lock);
  struct large_object_node *node =
    large_object_tree_insert(&space->object_tree, k, v);
  uintptr_t node_bits = (uintptr_t)node;
  address_map_add(&space->object_map, addr, node_bits);
  address_map_add(&space->to_space, addr, node_bits);
  space->total_pages += npages;
  pthread_mutex_unlock(&space->lock);

  return ret;
}

static int
large_object_space_init(struct large_object_space *space,
                        struct gc_heap *heap,
                        struct gc_background_thread *thread) {
  memset(space, 0, sizeof(*space));
  pthread_mutex_init(&space->lock, NULL);

  space->page_size = getpagesize();
  space->page_size_log2 = __builtin_ctz(space->page_size);

  large_object_tree_init(&space->object_tree);
  address_map_init(&space->object_map);

  large_object_freelist_init(&space->quarantine);

  address_map_init(&space->from_space);
  address_map_init(&space->to_space);
  address_map_init(&space->survivor_space);
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
