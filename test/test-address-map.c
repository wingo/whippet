#include <stdio.h>

#include "address-map.h"

#define COUNT (1000 * 1000)

static void add_to_other(uintptr_t addr, uintptr_t val, void *data) {
  struct address_map *other = data;
  if (addr >= COUNT)
    fprintf(stdout, "unexpected address: %zu\n", addr);
  if (address_map_contains(other, addr))
    fprintf(stdout, "missing: %zu\n", addr);
  address_map_add(other, addr, val);
}

int main(int argc, char *arv[]) {
  struct address_map set;
  address_map_init(&set);
  for (size_t i = 0; i < COUNT; i++)
    address_map_add(&set, i, -i);
  fprintf(stdout, "after initial add, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  for (size_t i = 0; i < COUNT; i++) {
    if (!address_map_contains(&set, i)) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
    if (address_map_lookup(&set, i, -1) != -i) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = COUNT; i < COUNT * 2; i++) {
    if (address_map_contains(&set, i)) {
      fprintf(stdout, "unexpectedly present: %zu\n", i);
      return 1;
    }
  }
  address_map_clear(&set);
  fprintf(stdout, "after clear, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  for (size_t i = 0; i < COUNT; i++)
    address_map_add(&set, i, 0);
  // Now update.
  fprintf(stdout, "after re-add, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  for (size_t i = 0; i < COUNT; i++)
    address_map_add(&set, i, i + 1);
  fprintf(stdout, "after idempotent re-add, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  for (size_t i = 0; i < COUNT; i++) {
    if (!address_map_contains(&set, i)) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
    if (address_map_lookup(&set, i, -1) != i + 1) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = 0; i < COUNT; i++)
    address_map_remove(&set, i);
  fprintf(stdout, "after one-by-one removal, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  for (size_t i = COUNT; i < 2 * COUNT; i++) {
    if (address_map_contains(&set, i)) {
      fprintf(stdout, "unexpectedly present: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = 0; i < COUNT; i++)
    address_map_add(&set, i, i + 2);
  struct address_map set2;
  address_map_init(&set2);
  address_map_for_each(&set, add_to_other, &set2);
  fprintf(stdout, "after for-each set, %zu/%zu\n", set2.hash_map.n_items,
          set2.hash_map.size);
  for (size_t i = 0; i < COUNT; i++) {
    if (address_map_lookup(&set2, i, -1) != i + 2) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
  }
  address_map_destroy(&set2);

  size_t burnin = 1000 * 1000 * 1000 / COUNT;
  fprintf(stdout, "beginning clear then add %zu items, %zu times\n",
          (size_t)COUNT, burnin);
  for (size_t j = 0; j < burnin; j++) {
    address_map_clear(&set);
    for (size_t i = 0; i < COUNT; i++)
      address_map_add(&set, i, i + 3);
  }
  fprintf(stdout, "after burnin, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  fprintf(stdout, "beginning lookup %zu items, %zu times\n",
          (size_t)COUNT, burnin);
  for (size_t j = 0; j < burnin; j++) {
    for (size_t i = 0; i < COUNT; i++) {
      if (address_map_lookup(&set, i, -1) != i + 3) {
        fprintf(stdout, "missing: %zu\n", i);
        return 1;
      }
    }
  }
  fprintf(stdout, "after burnin, %zu/%zu\n", set.hash_map.n_items,
          set.hash_map.size);
  address_map_destroy(&set);
}
