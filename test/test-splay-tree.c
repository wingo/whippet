#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

struct object {
  uintptr_t addr;
  size_t size;
};

struct data {
  size_t idx;
};

#define SPLAY_TREE_PREFIX object_
typedef struct object object_key_span;
typedef uintptr_t object_key;
typedef struct data object_value;
static inline int
object_compare(uintptr_t addr, struct object obj) {
  if (addr < obj.addr) return -1;
  if (addr - obj.addr < obj.size) return 0;
  return 1;
}
static inline uintptr_t
object_span_start(struct object obj) {
  return obj.addr;
}
#include "splay-tree.h"

// A power-law distribution.  Each integer was selected by starting at
// 0, taking a random number in [0,1), and then accepting the integer if
// the random number was less than 0.15, or trying again with the next
// integer otherwise.  Useful for modelling allocation sizes or number
// of garbage objects to allocate between live allocations.
static const uint8_t power_law_distribution[256] = {
  1, 15, 3, 12, 2, 8, 4, 0, 18, 7, 9, 8, 15, 2, 36, 5,
  1, 9, 6, 11, 9, 19, 2, 0, 0, 3, 9, 6, 3, 2, 1, 1,
  6, 1, 8, 4, 2, 0, 5, 3, 7, 0, 0, 3, 0, 4, 1, 7,
  1, 8, 2, 2, 2, 14, 0, 7, 8, 0, 2, 1, 4, 12, 7, 5,
  0, 3, 4, 13, 10, 2, 3, 7, 0, 8, 0, 23, 0, 16, 1, 1,
  6, 28, 1, 18, 0, 3, 6, 5, 8, 6, 14, 5, 2, 5, 0, 11,
  0, 18, 4, 16, 1, 4, 3, 13, 3, 23, 7, 4, 10, 5, 3, 13,
  0, 14, 5, 5, 2, 5, 0, 16, 2, 0, 1, 1, 0, 0, 4, 2,
  7, 7, 0, 5, 7, 2, 1, 24, 27, 3, 7, 1, 0, 8, 1, 4,
  0, 3, 0, 7, 7, 3, 9, 2, 9, 2, 5, 10, 1, 1, 12, 6,
  2, 9, 5, 0, 4, 6, 0, 7, 2, 1, 5, 4, 1, 0, 1, 15,
  4, 0, 15, 4, 0, 0, 32, 18, 2, 2, 1, 7, 8, 3, 11, 1,
  2, 7, 11, 1, 9, 1, 2, 6, 11, 17, 1, 2, 5, 1, 14, 3,
  6, 1, 1, 15, 3, 1, 0, 6, 10, 8, 1, 3, 2, 7, 0, 1,
  0, 11, 3, 3, 5, 8, 2, 0, 0, 7, 12, 2, 5, 20, 3, 7,
  4, 4, 5, 22, 1, 5, 2, 7, 15, 2, 4, 6, 11, 8, 12, 1
};

static size_t power_law(size_t *counter) {
  return power_law_distribution[(*counter)++ & 0xff];
}

static uintptr_t allocate(size_t size) {
  void *ret = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (ret == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }
  return (uintptr_t)ret;
}

static const size_t GB = 1024 * 1024 * 1024;

// Page size is at least 4 kB, so we will have at most 256 * 1024 allocations.
static uintptr_t all_objects[256 * 1024 + 1];
static size_t object_count;

#define ASSERT(x) do { if (!(x)) abort(); } while (0)

int main(int argc, char *arv[]) {
  struct object_tree tree;

  object_tree_init(&tree);

  size_t counter = 0;
  size_t page_size = getpagesize();

  // Use mmap as a source of nonoverlapping spans.  Allocate 1 GB of address space.
  size_t allocated = 0;
  while (allocated < 1 * GB) {
    size_t size = power_law(&counter) * page_size;
    if (!size)
      continue;
    uintptr_t addr = allocate(size);
    object_tree_insert(&tree,
                       (struct object){addr, size},
                       (struct data){object_count});
    all_objects[object_count++] = addr;
    ASSERT(object_count < sizeof(all_objects) / sizeof(all_objects[0]));
    allocated += size;
  }

  for (size_t i = 0; i < object_count; i++)
    ASSERT(object_tree_contains(&tree, all_objects[i]));

  for (size_t i = 0; i < object_count; i++)
    ASSERT(object_tree_lookup(&tree, all_objects[i])->value.idx == i);

  for (size_t i = 0; i < object_count; i++)
    ASSERT(object_tree_lookup(&tree, all_objects[i] + 42)->value.idx == i);

  for (size_t i = 0; i < object_count; i++)
    object_tree_remove(&tree, all_objects[i]);

  for (size_t i = 0; i < object_count; i++)
    ASSERT(!object_tree_contains(&tree, all_objects[i]));
  for (size_t i = 0; i < object_count; i++)
    ASSERT(object_tree_lookup(&tree, all_objects[i]) == NULL);
}
