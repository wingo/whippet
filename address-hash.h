#ifndef ADDRESS_HASH_H
#define ADDRESS_HASH_H

#include <stdint.h>

static uintptr_t hash_address(uintptr_t x) {
  if (sizeof (x) < 8) {
    // Chris Wellon's lowbias32, from https://nullprogram.com/blog/2018/07/31/.
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
  } else {
    // Sebastiano Vigna's splitmix64 integer mixer, from
    // https://prng.di.unimi.it/splitmix64.c.
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9U;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebU;
    x ^= x >> 31;
    return x;
  }
}
// Inverse of hash_address from https://nullprogram.com/blog/2018/07/31/.
static uintptr_t unhash_address(uintptr_t x) {
  if (sizeof (x) < 8) {
    x ^= x >> 16;
    x *= 0x43021123U;
    x ^= x >> 15 ^ x >> 30;
    x *= 0x1d69e2a5U;
    x ^= x >> 16;
    return x;
  } else {
    x ^= x >> 31 ^ x >> 62;
    x *= 0x319642b2d24d8ec3U;
    x ^= x >> 27 ^ x >> 54;
    x *= 0x96de1b173f119089U;
    x ^= x >> 30 ^ x >> 60;
    return x;
  }
}

#endif // ADDRESS_HASH_H
