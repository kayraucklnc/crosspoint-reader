#include "OrderedDither.h"

#include <cmath>

namespace ordered_dither {

uint8_t mode = BAYER;

namespace {
// Classic 8x8 Bayer index matrix (0..63).
constexpr uint8_t BAYER8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};

// Threshold in 0..254 for the cell, centred so a flat 50% tone gives exactly half the dots.
inline int threshold(const int x, const int y) { return (BAYER8[y & 7][x & 7] * 255 + 127) / 64; }
}  // namespace

Tone::Tone(const uint8_t lo, const uint8_t hi) {
  const int span = hi > lo ? hi - lo : 1;
  for (int g = 0; g < 256; g++) {
    float v = static_cast<float>(g - lo) / span;
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    v = v + 0.35f * v * (1 - v) * (2 * v - 1);  // gentle S-curve: deeper shadows, brighter highlights
    v = std::pow(v, 0.9f);                      // gamma 0.9 lifts midtones slightly
    lut[g] = static_cast<uint8_t>(v * 255 + 0.5f);
  }
}

uint8_t level4(const uint8_t gray, const int x, const int y) {
  const int scaled = gray * 3;  // 0..765 across three steps
  const int base = scaled / 255;
  if (base >= 3) return 3;
  const int frac = scaled - base * 255;
  return static_cast<uint8_t>(base + (frac > threshold(x, y) ? 1 : 0));
}

uint8_t level2(const uint8_t gray, const int x, const int y) { return gray > threshold(x, y) ? 1 : 0; }

void percentiles(const uint32_t* histogram, uint8_t& lo, uint8_t& hi) {
  uint64_t total = 0;
  for (int i = 0; i < 256; i++) total += histogram[i];
  lo = 0;
  hi = 255;
  if (total == 0) return;
  const uint64_t cut = total / 100;
  uint64_t acc = 0;
  for (int i = 0; i < 256; i++) {
    acc += histogram[i];
    if (acc > cut) {
      lo = static_cast<uint8_t>(i);
      break;
    }
  }
  acc = 0;
  for (int i = 255; i >= 0; i--) {
    acc += histogram[i];
    if (acc > cut) {
      hi = static_cast<uint8_t>(i);
      break;
    }
  }
  if (hi - lo < 32) {  // near-flat image: stretching would only amplify noise
    lo = 0;
    hi = 255;
  }
}

}  // namespace ordered_dither
