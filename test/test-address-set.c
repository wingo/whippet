#include <stdio.h>

#include "address-set.h"

#define COUNT (1000 * 1000)

static void remove_from_other(uintptr_t addr, void *data) {
  struct address_set *other = data;
  if (addr >= COUNT)
    fprintf(stdout, "unexpected address: %zu\n", addr);
  if (!address_set_contains(other, addr))
    fprintf(stdout, "missing: %zu\n", addr);
  address_set_remove(other, addr);
}

int main(int argc, char *arv[]) {
  struct address_set set;
  address_set_init(&set);
  for (size_t i = 0; i < COUNT; i++)
    address_set_add(&set, i);
  fprintf(stdout, "after initial add, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  for (size_t i = 0; i < COUNT; i++) {
    if (!address_set_contains(&set, i)) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = COUNT; i < COUNT * 2; i++) {
    if (address_set_contains(&set, i)) {
      fprintf(stdout, "unexpectedly present: %zu\n", i);
      return 1;
    }
  }
  address_set_clear(&set);
  fprintf(stdout, "after clear, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  for (size_t i = 0; i < COUNT; i++)
    address_set_add(&set, i);
  // Do it twice.
  fprintf(stdout, "after re-add, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  for (size_t i = 0; i < COUNT; i++)
    address_set_add(&set, i);
  fprintf(stdout, "after idempotent re-add, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  for (size_t i = 0; i < COUNT; i++) {
    if (!address_set_contains(&set, i)) {
      fprintf(stdout, "missing: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = 0; i < COUNT; i++)
    address_set_remove(&set, i);
  fprintf(stdout, "after one-by-one removal, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  for (size_t i = COUNT; i < 2 * COUNT; i++) {
    if (address_set_contains(&set, i)) {
      fprintf(stdout, "unexpectedly present: %zu\n", i);
      return 1;
    }
  }
  for (size_t i = 0; i < COUNT; i++)
    address_set_add(&set, i);
  struct address_set set2;
  address_set_init(&set2);
  address_set_union(&set2, &set);
  fprintf(stdout, "populated set2, %zu/%zu\n", set2.hash_set.n_items,
          set2.hash_set.size);
  address_set_for_each(&set, remove_from_other, &set2);
  fprintf(stdout, "after for-each removal, %zu/%zu\n", set2.hash_set.n_items,
          set2.hash_set.size);
  address_set_destroy(&set2);

  size_t burnin = 1000 * 1000 * 1000 / COUNT;
  fprintf(stdout, "beginning clear then add %zu items, %zu times\n",
          (size_t)COUNT, burnin);
  for (size_t j = 0; j < burnin; j++) {
    address_set_clear(&set);
    for (size_t i = 0; i < COUNT; i++)
      address_set_add(&set, i);
  }
  fprintf(stdout, "after burnin, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  fprintf(stdout, "beginning lookup %zu items, %zu times\n",
          (size_t)COUNT, burnin);
  for (size_t j = 0; j < burnin; j++) {
    for (size_t i = 0; i < COUNT; i++) {
      if (!address_set_contains(&set, i)) {
        fprintf(stdout, "missing: %zu\n", i);
        return 1;
      }
    }
  }
  fprintf(stdout, "after burnin, %zu/%zu\n", set.hash_set.n_items,
          set.hash_set.size);
  address_set_destroy(&set);
}
