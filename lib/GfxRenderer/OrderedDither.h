#pragma once

#include <cstdint>

// Image quantization shared by BMP drawing and cover/wallpaper conversion.
// BAYER (default): 8x8 ordered dither between adjacent gray levels after a
// tone curve; stable, grain-free patterns that suit e-ink. ERROR_DIFFUSION:
// the original Atkinson/Floyd-Steinberg path.
namespace ordered_dither {

enum Mode : uint8_t { BAYER = 0, ERROR_DIFFUSION = 1 };

// Set from the Image Dithering setting at boot and whenever it changes.
extern uint8_t mode;
inline bool bayer() { return mode == BAYER; }

// Contrast stretch between lo..hi (auto-levels), then a mild S-curve and gamma 0.9.
struct Tone {
  uint8_t lut[256];
  explicit Tone(uint8_t lo = 0, uint8_t hi = 255);
  uint8_t operator()(uint8_t gray) const { return lut[gray]; }
};

// 4 display levels (0 black .. 3 white), dithered between the two nearest.
uint8_t level4(uint8_t gray, int x, int y);
// 1-bit (0 black, 1 white).
uint8_t level2(uint8_t gray, int x, int y);

// 1st/99th percentile of a 256-bin luminance histogram, for Tone's auto-levels.
void percentiles(const uint32_t* histogram, uint8_t& lo, uint8_t& hi);

}  // namespace ordered_dither
