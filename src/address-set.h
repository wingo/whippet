#ifndef ADDRESS_SET_H
#define ADDRESS_SET_H

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include "address-hash.h"
#include "gc-assert.h"

struct hash_set {
  uintptr_t *data;
  size_t size;    	// total number of slots
  size_t n_items;	// number of items in set
  uint8_t *bits;        // bitvector indicating set slots
};

static void hash_set_clear(struct hash_set *set) {
  memset(set->bits, 0, set->size / 8);
  set->n_items = 0;
}
  
// Size must be a power of 2.
static void hash_set_init(struct hash_set *set, size_t size) {
  set->size = size;
  set->data = malloc(sizeof(uintptr_t) * size);
  if (!set->data) GC_CRASH();
  set->bits = malloc(size / 8);
  if (!set->bits) GC_CRASH();
  hash_set_clear(set);
}
static void hash_set_destroy(struct hash_set *set) {
  free(set->data);
  free(set->bits);
}

static size_t hash_set_slot_index(struct hash_set *set, size_t idx) {
  return idx & (set->size - 1);
}
static int hash_set_slot_is_empty(struct hash_set *set, size_t idx) {
  idx = hash_set_slot_index(set, idx);
  return (set->bits[idx / 8] & (1 << (idx % 8))) == 0;
}
static uintptr_t hash_set_slot_ref(struct hash_set *set, size_t idx) {
  return set->data[hash_set_slot_index(set, idx)];
}
static void hash_set_slot_set(struct hash_set *set, size_t idx, uintptr_t v) {
  set->data[hash_set_slot_index(set, idx)] = v;
}
static void hash_set_slot_acquire(struct hash_set *set, size_t idx) {
  idx = hash_set_slot_index(set, idx);
  set->bits[idx / 8] |= (1 << (idx % 8));
  set->n_items++;
}
static void hash_set_slot_release(struct hash_set *set, size_t idx) {
  idx = hash_set_slot_index(set, idx);
  set->bits[idx / 8] &= ~(1 << (idx % 8));
  set->n_items--;
}
static size_t hash_set_slot_distance(struct hash_set *set, size_t idx) {
  return hash_set_slot_index(set, idx - hash_set_slot_ref(set, idx));
}
static int hash_set_should_shrink(struct hash_set *set) {
  return set->size > 8 && set->n_items <= (set->size >> 3);
}
static int hash_set_should_grow(struct hash_set *set) {
  return set->n_items >= set->size - (set->size >> 3);
}

static void hash_set_do_insert(struct hash_set *set, uintptr_t v) {
  size_t displacement = 0;
  while (!hash_set_slot_is_empty(set, v + displacement)
         && displacement < hash_set_slot_distance(set, v + displacement))
    displacement++;
  while (!hash_set_slot_is_empty(set, v + displacement)
         && displacement == hash_set_slot_distance(set, v + displacement)) {
    if (hash_set_slot_ref(set, v + displacement) == v)
      return;
    displacement++;
  }
  size_t idx = v + displacement;
  size_t slots_to_move = 0;
  while (!hash_set_slot_is_empty(set, idx + slots_to_move))
    slots_to_move++;
  hash_set_slot_acquire(set, idx + slots_to_move);
  while (slots_to_move--)
    hash_set_slot_set(set, idx + slots_to_move + 1,
                      hash_set_slot_ref(set, idx + slots_to_move));
  hash_set_slot_set(set, idx, v);
}

static void hash_set_populate(struct hash_set *dst, struct hash_set *src) {
  for (size_t i = 0; i < src->size; i++)
    if (!hash_set_slot_is_empty(src, i))
      hash_set_do_insert(dst, hash_set_slot_ref(src, i));
}
static void hash_set_grow(struct hash_set *set) {
  struct hash_set fresh;
  hash_set_init(&fresh, set->size << 1);
  hash_set_populate(&fresh, set);
  hash_set_destroy(set);
  memcpy(set, &fresh, sizeof(fresh));
}
static void hash_set_shrink(struct hash_set *set) {
  struct hash_set fresh;
  hash_set_init(&fresh, set->size >> 1);
  hash_set_populate(&fresh, set);
  hash_set_destroy(set);
  memcpy(set, &fresh, sizeof(fresh));
}

static void hash_set_insert(struct hash_set *set, uintptr_t v) {
  if (hash_set_should_grow(set))
    hash_set_grow(set);
  hash_set_do_insert(set, v);
}

static void hash_set_remove(struct hash_set *set, uintptr_t v) {
  size_t slot = v;
  while (!hash_set_slot_is_empty(set, slot) && hash_set_slot_ref(set, slot) != v)
    slot++;
  if (hash_set_slot_is_empty(set, slot))
    __builtin_trap();
  while (!hash_set_slot_is_empty(set, slot + 1)
         && hash_set_slot_distance(set, slot + 1)) {
    hash_set_slot_set(set, slot, hash_set_slot_ref(set, slot + 1));
    slot++;
  }
  hash_set_slot_release(set, slot);
  if (hash_set_should_shrink(set))
    hash_set_shrink(set);
}
static int hash_set_contains(struct hash_set *set, uintptr_t v) {
  for (size_t slot = v; !hash_set_slot_is_empty(set, slot); slot++) {
    if (hash_set_slot_ref(set, slot) == v)
      return 1;
    if (hash_set_slot_distance(set, slot) < (slot - v))
      return 0;
  }
  return 0;
}
static inline void hash_set_find(struct hash_set *set,
                                 int (*f)(uintptr_t, void*), void *data) __attribute__((always_inline));
static inline void hash_set_find(struct hash_set *set,
                                 int (*f)(uintptr_t, void*), void *data) {
  for (size_t i = 0; i < set->size; i++)
    if (!hash_set_slot_is_empty(set, i))
      if (f(hash_set_slot_ref(set, i), data))
        return;
}
  
struct address_set {
  struct hash_set hash_set;
};

static void address_set_init(struct address_set *set) {
  hash_set_init(&set->hash_set, 8);
}
static void address_set_destroy(struct address_set *set) {
  hash_set_destroy(&set->hash_set);
}
static void address_set_clear(struct address_set *set) {
  hash_set_clear(&set->hash_set);
}
  
static void address_set_add(struct address_set *set, uintptr_t addr) {
  hash_set_insert(&set->hash_set, hash_address(addr));
}
static void address_set_remove(struct address_set *set, uintptr_t addr) {
  hash_set_remove(&set->hash_set, hash_address(addr));
}
static int address_set_contains(struct address_set *set, uintptr_t addr) {
  return hash_set_contains(&set->hash_set, hash_address(addr));
}
static void address_set_union(struct address_set *set, struct address_set *other) {
  while (set->hash_set.size < other->hash_set.size)
    hash_set_grow(&set->hash_set);
  hash_set_populate(&set->hash_set, &other->hash_set);
}

struct address_set_for_each_data {
  void (*f)(uintptr_t, void *);
  void *data;
};
static int address_set_do_for_each(uintptr_t v, void *data) {
  struct address_set_for_each_data *for_each_data = data;
  for_each_data->f(unhash_address(v), for_each_data->data);
  return 0;
}
static inline void address_set_for_each(struct address_set *set,
                                        void (*f)(uintptr_t, void*), void *data) __attribute__((always_inline));
static inline void address_set_for_each(struct address_set *set,
                                        void (*f)(uintptr_t, void*), void *data) {
  struct address_set_for_each_data for_each_data = { f, data };
  hash_set_find(&set->hash_set, address_set_do_for_each, &for_each_data);
}

struct address_set_find_data {
  int (*f)(uintptr_t, void *);
  void *data;
};
static int address_set_do_find(uintptr_t v, void *data) {
  struct address_set_find_data *find_data = data;
  return find_data->f(unhash_address(v), find_data->data);
}
static inline void address_set_find(struct address_set *set,
                                    int (*f)(uintptr_t, void*), void *data) __attribute__((always_inline));
static inline void address_set_find(struct address_set *set,
                                    int (*f)(uintptr_t, void*), void *data) {
  struct address_set_find_data find_data = { f, data };
  hash_set_find(&set->hash_set, address_set_do_find, &find_data);
}

#endif // ADDRESS_SET_H
