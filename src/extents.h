#ifndef EXTENTS_H
#define EXTENTS_H

#include <stdint.h>
#include <stdlib.h>

#include "gc-assert.h"

struct extent_range {
  uintptr_t lo_addr;
  uintptr_t hi_addr;
};

struct extents {
  size_t size;
  size_t capacity;
  struct extent_range ranges[];
};

static inline int
extents_contain_addr(struct extents *extents, uintptr_t addr) {
  size_t lo = 0;
  size_t hi = extents->size;
  while (lo != hi) {
    size_t mid = (lo + hi) / 2;
    struct extent_range range = extents->ranges[mid];
    if (addr < range.lo_addr) {
      hi = mid;
    } else if (addr < range.hi_addr) {
      return 1;
    } else {
      lo = mid + 1;
    }
  }
  return 0;
}

static struct extents*
extents_allocate(size_t capacity) {
  size_t byte_size =
    sizeof(struct extents) + sizeof(struct extent_range) * capacity;
  struct extents *ret = malloc(byte_size);
  if (!ret) __builtin_trap();
  memset(ret, 0, byte_size);
  ret->capacity = capacity;
  return ret;
}

static struct extents*
extents_insert(struct extents *old, size_t idx, struct extent_range range) {
  if (old->size < old->capacity) {
    size_t bytes_to_move = sizeof(struct extent_range) * (old->size - idx);
    memmove(&old->ranges[idx + 1], &old->ranges[idx], bytes_to_move);
    old->ranges[idx] = range;
    old->size++;
    return old;
  } else {
    struct extents *new_ = extents_allocate(old->capacity * 2 + 1);
    memcpy(&new_->ranges[0], &old->ranges[0],
           sizeof(struct extent_range) * idx);
    memcpy(&new_->ranges[idx + 1], &old->ranges[idx],
           sizeof(struct extent_range) * (old->size - idx));
    new_->ranges[idx] = range;
    new_->size = old->size + 1;
    free(old);
    return new_;
  }
}

static struct extents*
extents_adjoin(struct extents *extents, void *lo_addr, size_t size) {
  size_t i;
  struct extent_range range = { (uintptr_t)lo_addr, (uintptr_t)lo_addr + size };
  for (i = 0; i < extents->size; i++) {
    if (range.hi_addr < extents->ranges[i].lo_addr) {
      break;
    } else if (range.hi_addr == extents->ranges[i].lo_addr) {
      extents->ranges[i].lo_addr = range.lo_addr;
      return extents;
    } else if (range.lo_addr == extents->ranges[i].hi_addr) {
      extents->ranges[i].hi_addr = range.hi_addr;
      return extents;
    }
  }
  return extents_insert(extents, i, range);
}
  
#endif // EXTENTS_H
