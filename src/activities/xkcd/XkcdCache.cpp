#include "XkcdCache.h"

#include <ArduinoJson.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xkcd_cache {
namespace {
// JPEG headers can trail an EXIF block; give up past this many bytes.
constexpr size_t MAX_HEADER_BYTES = 64 * 1024;
constexpr size_t PROBE_CHUNK = 128;

// ".png" / ".jpg" from an image URL; empty for formats the decoders lack (gif).
const char* imageExtension(const char* url) {
  const char* dot = strrchr(url, '.');
  if (!dot) return "";
  if (strcasecmp(dot, ".png") == 0) return ".png";
  if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return ".jpg";
  return "";
}
}  // namespace

std::string metaPath(const int num) {
  char path[48];
  snprintf(path, sizeof(path), "%s/%d.json", DIR, num);
  return path;
}

bool parseMeta(const char* json, Meta& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  out.num = doc["num"] | 0;
  out.title = doc["safe_title"] | "";
  out.alt = doc["alt"] | "";
  out.imageUrl = doc["img"] | "";
  out.year = static_cast<uint16_t>(atoi(doc["year"] | "0"));
  out.month = static_cast<uint8_t>(atoi(doc["month"] | "0"));
  out.day = static_cast<uint8_t>(atoi(doc["day"] | "0"));
  const char* extension = imageExtension(out.imageUrl.c_str());
  out.imagePath.clear();
  if (extension[0] && out.num > 0) {
    char path[48];
    snprintf(path, sizeof(path), "%s/%u%s", DIR, out.num, extension);
    out.imagePath = path;
  }
  return out.num > 0;
}

bool imageSize(const std::string& path, int16_t& width, int16_t& height) {
  HalFile file;
  if (!Storage.openFileForRead("XKCD", path, file)) return false;
  ImageDimsProbe probe;
  uint8_t chunk[PROBE_CHUNK];
  size_t total = 0;
  while (total < MAX_HEADER_BYTES) {
    const int got = file.read(chunk, sizeof(chunk));
    if (got <= 0) break;
    total += static_cast<size_t>(got);
    if (probe.write(chunk, static_cast<size_t>(got)) < static_cast<size_t>(got)) break;
  }
  ImageDimensions dims{};
  if (!probe.getDimensions(dims)) return false;
  width = dims.width;
  height = dims.height;
  return true;
}

}  // namespace xkcd_cache
