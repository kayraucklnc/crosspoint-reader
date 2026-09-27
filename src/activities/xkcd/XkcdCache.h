#pragma once

#include <cstdint>
#include <string>

// Online cache of comics newer than the offline pack: per comic a trimmed
// info.0.json ("N.json") and the original image ("N.png" / "N.jpg").
namespace xkcd_cache {

constexpr char DIR[] = "/.crosspoint/xkcd";
constexpr char STATE_PATH[] = "/.crosspoint/xkcd/state.json";

struct Meta {
  uint16_t num = 0;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  std::string title;
  std::string alt;
  std::string imageUrl;
  std::string imagePath;  // empty for formats the decoders lack (gif)
};

std::string metaPath(int num);
// Parses info.0.json (full or trimmed) and fills the cache image path.
bool parseMeta(const char* json, Meta& out);
// Reads the image size from the file header, without an image decoder.
bool imageSize(const std::string& path, int16_t& width, int16_t& height);

}  // namespace xkcd_cache
