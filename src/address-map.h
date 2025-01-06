#ifndef ADDRESS_MAP_H
#define ADDRESS_MAP_H

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include "address-hash.h"
#include "gc-assert.h"

struct hash_map_entry {
  uintptr_t k;
  uintptr_t v;
};

struct hash_map {
  struct hash_map_entry *data;
  size_t size;    	// total number of slots
  size_t n_items;	// number of items in set
  uint8_t *bits;        // bitvector indicating set slots
};

static void hash_map_clear(struct hash_map *map) {
  memset(map->bits, 0, map->size / 8);
  map->n_items = 0;
}
  
// Size must be a power of 2.
static void hash_map_init(struct hash_map *map, size_t size) {
  map->size = size;
  map->data = malloc(sizeof(struct hash_map_entry) * size);
  if (!map->data) GC_CRASH();
  map->bits = malloc(size / 8);
  if (!map->bits) GC_CRASH();
  hash_map_clear(map);
}
static void hash_map_destroy(struct hash_map *map) {
  free(map->data);
  free(map->bits);
}

static size_t hash_map_slot_index(struct hash_map *map, size_t idx) {
  return idx & (map->size - 1);
}
static struct hash_map_entry* hash_map_slot_entry(struct hash_map *map,
                                                  size_t idx) {
  return &map->data[hash_map_slot_index(map, idx)];
}
static int hash_map_slot_is_empty(struct hash_map *map, size_t idx) {
  idx = hash_map_slot_index(map, idx);
  return (map->bits[idx / 8] & (1 << (idx % 8))) == 0;
}
static void hash_map_slot_acquire(struct hash_map *map, size_t idx) {
  idx = hash_map_slot_index(map, idx);
  map->bits[idx / 8] |= (1 << (idx % 8));
  map->n_items++;
}
static void hash_map_slot_release(struct hash_map *map, size_t idx) {
  idx = hash_map_slot_index(map, idx);
  map->bits[idx / 8] &= ~(1 << (idx % 8));
  map->n_items--;
}
static size_t hash_map_slot_distance(struct hash_map *map, size_t idx) {
  return hash_map_slot_index(map, idx - hash_map_slot_entry(map, idx)->k);
}
static int hash_map_should_shrink(struct hash_map *map) {
  return map->size > 8 && map->n_items <= (map->size >> 3);
}
static int hash_map_should_grow(struct hash_map *map) {
  return map->n_items >= map->size - (map->size >> 3);
}

static void hash_map_do_insert(struct hash_map *map, uintptr_t k, uintptr_t v) {
  size_t displacement = 0;
  while (!hash_map_slot_is_empty(map, k + displacement)
         && displacement < hash_map_slot_distance(map, k + displacement))
    displacement++;
  while (!hash_map_slot_is_empty(map, k + displacement)
         && displacement == hash_map_slot_distance(map, k + displacement)) {
    if (hash_map_slot_entry(map, k + displacement)->k == k) {
      hash_map_slot_entry(map, k + displacement)->v = v;
      return;
    }
    displacement++;
  }
  size_t idx = k + displacement;
  size_t slots_to_move = 0;
  while (!hash_map_slot_is_empty(map, idx + slots_to_move))
    slots_to_move++;
  hash_map_slot_acquire(map, idx + slots_to_move);
  while (slots_to_move--)
    *hash_map_slot_entry(map, idx + slots_to_move + 1) =
      *hash_map_slot_entry(map, idx + slots_to_move);
  *hash_map_slot_entry(map, idx) = (struct hash_map_entry){ k, v };
}

static void hash_map_populate(struct hash_map *dst, struct hash_map *src) {
  for (size_t i = 0; i < src->size; i++)
    if (!hash_map_slot_is_empty(src, i))
      hash_map_do_insert(dst, hash_map_slot_entry(src, i)->k,
                         hash_map_slot_entry(src, i)->v);
}
static void hash_map_grow(struct hash_map *map) {
  struct hash_map fresh;
  hash_map_init(&fresh, map->size << 1);
  hash_map_populate(&fresh, map);
  hash_map_destroy(map);
  memcpy(map, &fresh, sizeof(fresh));
}
static void hash_map_shrink(struct hash_map *map) {
  struct hash_map fresh;
  hash_map_init(&fresh, map->size >> 1);
  hash_map_populate(&fresh, map);
  hash_map_destroy(map);
  memcpy(map, &fresh, sizeof(fresh));
}

static void hash_map_insert(struct hash_map *map, uintptr_t k, uintptr_t v) {
  if (hash_map_should_grow(map))
    hash_map_grow(map);
  hash_map_do_insert(map, k, v);
}
static void hash_map_remove(struct hash_map *map, uintptr_t k) {
  size_t slot = k;
  while (!hash_map_slot_is_empty(map, slot) && hash_map_slot_entry(map, slot)->k != k)
    slot++;
  if (hash_map_slot_is_empty(map, slot))
    __builtin_trap();
  while (!hash_map_slot_is_empty(map, slot + 1)
         && hash_map_slot_distance(map, slot + 1)) {
    *hash_map_slot_entry(map, slot) = *hash_map_slot_entry(map, slot + 1);
    slot++;
  }
  hash_map_slot_release(map, slot);
  if (hash_map_should_shrink(map))
    hash_map_shrink(map);
}
static int hash_map_contains(struct hash_map *map, uintptr_t k) {
  for (size_t slot = k; !hash_map_slot_is_empty(map, slot); slot++) {
    if (hash_map_slot_entry(map, slot)->k == k)
      return 1;
    if (hash_map_slot_distance(map, slot) < (slot - k))
      return 0;
  }
  return 0;
}
static uintptr_t hash_map_lookup(struct hash_map *map, uintptr_t k, uintptr_t default_) {
  for (size_t slot = k; !hash_map_slot_is_empty(map, slot); slot++) {
    if (hash_map_slot_entry(map, slot)->k == k)
      return hash_map_slot_entry(map, slot)->v;
    if (hash_map_slot_distance(map, slot) < (slot - k))
      break;
  }
  return default_;
}
static inline void hash_map_for_each (struct hash_map *map,
                                      void (*f)(uintptr_t, uintptr_t, void*),
                                      void *data) __attribute__((always_inline));
static inline void hash_map_for_each(struct hash_map *map,
                                     void (*f)(uintptr_t, uintptr_t, void*),
                                     void *data) {
  for (size_t i = 0; i < map->size; i++)
    if (!hash_map_slot_is_empty(map, i))
      f(hash_map_slot_entry(map, i)->k, hash_map_slot_entry(map, i)->v, data);
}
  
struct address_map {
  struct hash_map hash_map;
};

static void address_map_init(struct address_map *map) {
  hash_map_init(&map->hash_map, 8);
}
static void address_map_destroy(struct address_map *map) {
  hash_map_destroy(&map->hash_map);
}
static void address_map_clear(struct address_map *map) {
  hash_map_clear(&map->hash_map);
}
  
static void address_map_add(struct address_map *map, uintptr_t addr, uintptr_t v) {
  hash_map_insert(&map->hash_map, hash_address(addr), v);
}
static void address_map_remove(struct address_map *map, uintptr_t addr) {
  hash_map_remove(&map->hash_map, hash_address(addr));
}
static int address_map_contains(struct address_map *map, uintptr_t addr) {
  return hash_map_contains(&map->hash_map, hash_address(addr));
}
static uintptr_t address_map_lookup(struct address_map *map, uintptr_t addr,
                                 uintptr_t default_) {
  return hash_map_lookup(&map->hash_map, hash_address(addr), default_);
}

struct address_map_for_each_data {
  void (*f)(uintptr_t, uintptr_t, void *);
  void *data;
};
static void address_map_do_for_each(uintptr_t k, uintptr_t v, void *data) {
  struct address_map_for_each_data *for_each_data = data;
  for_each_data->f(unhash_address(k), v, for_each_data->data);
}
static inline void address_map_for_each (struct address_map *map,
                                         void (*f)(uintptr_t, uintptr_t, void*),
                                         void *data) __attribute__((always_inline));
static inline void address_map_for_each (struct address_map *map,
                                         void (*f)(uintptr_t, uintptr_t, void*),
                                         void *data) {
  struct address_map_for_each_data for_each_data = { f, data };
  hash_map_for_each(&map->hash_map, address_map_do_for_each, &for_each_data);
}

#endif // ADDRESS_MAP_H
