#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Low-resolution 1-bit previews of the /sleep wallpapers for the picker.
// The BMP is streamed row by row: a sparse first pass over every 8th
// thumbnail row gathers a luminance histogram, the second pass box-averages
// each thumbnail pixel's source area, stretches levels, lifts dark images with
// a gamma curve and 8x8-Bayer dithers straight into the bit buffer, so no
// full-resolution or 8-bit plane is ever held. Results are cached in
// THUMB_DIR, keyed on the box size and the source's size, mtime and a content
// sample.
namespace wallpaper {

constexpr int THUMB_MAX_W = 264;
constexpr int THUMB_MAX_H = 400;
constexpr int THUMB_STRIDE = (THUMB_MAX_W + 7) / 8;
constexpr const char* THUMB_DIR = "/.crosspoint/wallpaper_thumbs";

class ThumbLoader {
 public:
  enum class Result : uint8_t { Cached, Decoded, Failed };

  // Loads /sleep/<name> fitted inside boxWidth x boxHeight (clamped to THUMB_MAX_*).
  Result load(const std::string& name, int boxWidth, int boxHeight);

  int width() const { return thumbWidth; }
  int height() const { return thumbHeight; }
  bool isWhite(const int x, const int y) const { return (bits[y * THUMB_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1; }

 private:
  // 1-bit rows of THUMB_STRIDE bytes, MSB = leftmost pixel, 1 = white.
  std::unique_ptr<uint8_t[]> bits;
  int thumbWidth = 0;
  int thumbHeight = 0;
};

// "rocky_again.bmp" -> "rocky again".
void displayName(const std::string& fileName, char* out, size_t outSize);

}  // namespace wallpaper
