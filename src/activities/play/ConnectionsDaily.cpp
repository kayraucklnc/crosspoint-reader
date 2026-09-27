#include "ConnectionsDaily.h"

namespace connections {

namespace {

// splitmix32-style finaliser: cheap and well mixed, which is all a round
// function needs here.
uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// A bijection on [0, 4^halfBits): a four-round balanced Feistel network.
uint32_t feistel(const uint32_t value, const int halfBits, const uint32_t key) {
  const uint32_t mask = (1u << halfBits) - 1u;
  uint32_t left = value >> halfBits;
  uint32_t right = value & mask;
  for (uint32_t round = 0; round < 4; round++) {
    const uint32_t next = left ^ (mix(right ^ key ^ (round * 0x9E3779B9u)) & mask);
    left = right;
    right = next;
  }
  return (left << halfBits) | right;
}

}  // namespace

int dailyPuzzle(const uint32_t day, const int count, const uint32_t salt) {
  if (count < 1) return -1;
  const uint32_t n = static_cast<uint32_t>(count);
  const uint32_t cycle = day / n;
  const uint32_t key = mix(salt ^ mix(cycle + 0x632BE5ABu));
  int halfBits = 1;
  while ((1u << (2 * halfBits)) < n) halfBits++;
  // Cycle-walking: a bijection on the power-of-four domain, iterated until it
  // lands inside [0, n), is a bijection on [0, n). The domain is under 4n, so
  // this takes a few steps on average.
  uint32_t value = day % n;
  do {
    value = feistel(value, halfBits, key);
  } while (value >= n);
  return static_cast<int>(value);
}

}  // namespace connections
