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

static inline uint64_t
match_bytes_against_bits(uint64_t bytes, uint8_t mask) {
  return bytes & broadcast_byte(mask);
}

static inline size_t
scan_for_byte_with_bits(uint8_t *ptr, size_t limit, uint8_t mask) {
  size_t n = 0;
  size_t unaligned = ((uintptr_t) ptr) & 7;
  if (unaligned) {
    uint64_t bytes = load_eight_aligned_bytes(ptr - unaligned) >> (unaligned * 8);
    uint64_t match = match_bytes_against_bits(bytes, mask);
    if (match)
      return count_zero_bytes(match);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_eight_aligned_bytes(ptr + n);
    uint64_t match = match_bytes_against_bits(bytes, mask);
    if (match)
      return n + count_zero_bytes(match);
  }

  return limit;
}

static inline uint64_t
match_bytes_against_tag(uint64_t bytes, uint8_t mask, uint8_t tag) {
  // Precondition: tag within mask.
  GC_ASSERT_EQ(tag & mask, tag);
  // Precondition: high bit of mask byte is empty, so that we can add without
  // overflow.
  GC_ASSERT_EQ(mask & 0x7f, mask);
  // Precondition: mask is low bits of byte.
  GC_ASSERT(mask);
  GC_ASSERT_EQ(mask & (mask + 1), 0);

  uint64_t vmask = broadcast_byte(mask);
  uint64_t vtest = broadcast_byte(mask + 1);
  uint64_t vtag = broadcast_byte(tag);

  bytes &= vmask;
  uint64_t m = (bytes ^ vtag) + vmask;
  return (m & vtest) ^ vtest;
}

static inline size_t
scan_for_byte_with_tag(uint8_t *ptr, size_t limit, uint8_t mask, uint8_t tag) {
  // The way we handle unaligned reads by padding high bytes with zeroes assumes
  // that all-zeroes is not a matching byte.
  GC_ASSERT(tag);

  size_t n = 0;
  size_t unaligned = ((uintptr_t) ptr) & 7;
  if (unaligned) {
    uint64_t bytes = load_eight_aligned_bytes(ptr - unaligned) >> (unaligned * 8);
    uint64_t match = match_bytes_against_tag(bytes, mask, tag);
    if (match)
      return count_zero_bytes(match);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_eight_aligned_bytes(ptr + n);
    uint64_t match = match_bytes_against_tag(bytes, mask, tag);
    if (match)
      return n + count_zero_bytes(match);
  }

  return limit;
}

static inline uint64_t
match_bytes_against_2_tags(uint64_t bytes, uint8_t mask, uint8_t tag1,
                           uint8_t tag2) 
{
  // Precondition: tags are covered by within mask.
  GC_ASSERT_EQ(tag1 & mask, tag1);
  GC_ASSERT_EQ(tag2 & mask, tag2);
  // Precondition: high bit of mask byte is empty, so that we can add without
  // overflow.
  GC_ASSERT_EQ(mask & 0x7f, mask);
  // Precondition: mask is low bits of byte.
  GC_ASSERT(mask);
  GC_ASSERT_EQ(mask & (mask + 1), 0);
  
  uint64_t vmask = broadcast_byte(mask);
  uint64_t vtest = broadcast_byte(mask + 1);
  uint64_t vtag1 = broadcast_byte(tag1);
  uint64_t vtag2 = broadcast_byte(tag2);

  bytes &= vmask;
  uint64_t m1 = (bytes ^ vtag1) + vmask;
  uint64_t m2 = (bytes ^ vtag2) + vmask;
  return ((m1 & m2) & vtest) ^ vtest;
}

static inline size_t
scan_for_byte_with_tags(uint8_t *ptr, size_t limit, uint8_t mask,
                        uint8_t tag1, uint8_t tag2) {
  // The way we handle unaligned reads by padding high bytes with zeroes assumes
  // that all-zeroes is not a matching byte.
  GC_ASSERT(tag1 && tag2);

  size_t n = 0;
  size_t unaligned = ((uintptr_t) ptr) & 7;
  if (unaligned) {
    uint64_t bytes = load_eight_aligned_bytes(ptr - unaligned) >> (unaligned * 8);
    uint64_t match = match_bytes_against_2_tags(bytes, mask, tag1, tag2);
    if (match)
      return count_zero_bytes(match);
    n += 8 - unaligned;
  }

  for(; n < limit; n += 8) {
    uint64_t bytes = load_eight_aligned_bytes(ptr + n);
    uint64_t match = match_bytes_against_2_tags(bytes, mask, tag1, tag2);
    if (match)
      return n + count_zero_bytes(match);
  }

  return limit;
}

#endif // SWAR_H
