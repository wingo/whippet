#ifndef SWAR_H
#define SWAR_H

#include <string.h>

static inline size_t
count_zero_bytes(uint64_t bytes) {
  return bytes ? (__builtin_ctzll(bytes) / 8) : sizeof(bytes);
}

static uint64_t
broadcast_byte(uint8_t byte) {
  uint64_t result = byte;
  return result * 0x0101010101010101ULL;
}

static inline uint64_t
load_eight_aligned_bytes(uint8_t *ptr) {
  GC_ASSERT(((uintptr_t)ptr & 7) == 0);
  uint8_t * __attribute__((aligned(8))) aligned_ptr = ptr;
  uint64_t word;
  memcpy(&word, aligned_ptr, 8);
#ifdef WORDS_BIGENDIAN
  word = __builtin_bswap64(word);
#endif
  return word;
}

static size_t
scan_for_byte(uint8_t *ptr, size_t limit, uint64_t mask) {
  size_t n = 0;
  size_t unaligned = ((uintptr_t) ptr) & 7;
  if (unaligned) {
    uint64_t bytes = load_eight_aligned_bytes(ptr - unaligned) >> (unaligned * 8);
    bytes &= mask;
    if (bytes)
      return count_zero_bytes(bytes);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_eight_aligned_bytes(ptr + n);
    bytes &= mask;
    if (bytes)
      return n + count_zero_bytes(bytes);
  }

  return limit;
}

#endif // SWAR_H
