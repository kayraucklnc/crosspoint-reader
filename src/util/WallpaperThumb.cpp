#include "WallpaperThumb.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <OrderedDither.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "WallpaperStore.h"

namespace wallpaper {
namespace {

constexpr uint32_t CACHE_MAGIC = 0x31485457;  // "WTH1"
// Bump whenever sampling or tone mapping changes so cached thumbnails rebuild.
constexpr uint8_t CACHE_VERSION = 1;
constexpr int MAX_SOURCE_W = 2048;
constexpr int MAX_SOURCE_H = 3072;
constexpr size_t BITS_BYTES = static_cast<size_t>(THUMB_STRIDE) * THUMB_MAX_H;
// A thumbnail row averages at most this many evenly spaced source rows, which
// bounds the I/O for sources much larger than the screen.
constexpr int MAX_ROWS_PER_BAND = 3;
// The histogram pass reads one thumbnail row in STATS_STRIDE.
constexpr int STATS_STRIDE = 8;

// Tone mapping. The 1st..99th percentile range fills 0..KNEE; brighter
// outliers are compressed into KNEE..1 instead of clipped. Images whose
// stretched mean is below TARGET_MEAN get a gamma lift toward it, which raises
// the dim contours of dark wallpapers while pure black stays black.
constexpr int MIN_LEVEL_SPAN = 48;
constexpr float KNEE = 0.9f;
constexpr float TARGET_MEAN = 0.22f;
constexpr float MIN_GAMMA = 0.6f;

// Naturally aligned so it can be read straight into the struct.
struct CacheHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved;
  uint16_t width;
  uint16_t height;
  uint16_t boxWidth;
  uint16_t boxHeight;
  uint16_t reserved2;
  uint32_t sourceSize;
  uint32_t sourceMtime;
  uint32_t sourceSample;
};
static_assert(sizeof(CacheHeader) == 28, "cache header layout");

struct SourceKey {
  uint32_t size = 0;
  uint32_t mtime = 0;
  uint32_t sample = 0;
};

struct BmpLayout {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint16_t bpp = 0;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
  uint32_t paletteOffset = 0;
  uint32_t colors = 0;
};

// Per-decode work block: column sums | histogram | averaged line | tone LUT | palette | source row.
struct Work {
  static constexpr size_t SUMS = 0;
  static constexpr size_t HISTOGRAM = SUMS + THUMB_MAX_W * sizeof(uint32_t);
  static constexpr size_t LINE = HISTOGRAM + 256 * sizeof(uint32_t);
  static constexpr size_t LUT = LINE + THUMB_MAX_W;
  static constexpr size_t PALETTE = LUT + 256;
  static constexpr size_t ROW = PALETTE + 256;
  static_assert(HISTOGRAM % alignof(uint32_t) == 0, "histogram must be aligned");

  std::unique_ptr<uint8_t[]> block;
  uint32_t* sums() const { return reinterpret_cast<uint32_t*>(block.get() + SUMS); }
  uint32_t* histogram() const { return reinterpret_cast<uint32_t*>(block.get() + HISTOGRAM); }
  uint8_t* line() const { return block.get() + LINE; }
  uint8_t* lut() const { return block.get() + LUT; }
  uint8_t* palette() const { return block.get() + PALETTE; }
  uint8_t* row() const { return block.get() + ROW; }
};

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t luminance(const uint8_t r, const uint8_t g, const uint8_t b) {
  return static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
}

// Size, mtime and a hash of four 16-byte samples: files written without a
// clock can share an mtime, and every 528x792 wallpaper has the same size.
SourceKey readSourceKey(HalFile& file) {
  SourceKey key;
  key.size = static_cast<uint32_t>(file.fileSize());
  key.mtime = file.modificationTime();
  uint32_t hash = 2166136261u;
  uint8_t sample[16];
  for (int k = 1; k <= 4; k++) {
    if (!file.seek(static_cast<size_t>(key.size) * k / 5)) break;
    const int n = file.read(sample, sizeof(sample));
    for (int i = 0; i < n; i++) {
      hash ^= sample[i];
      hash *= 16777619u;
    }
  }
  key.sample = hash;
  return key;
}

std::string cachePath(const std::string& name) { return std::string(THUMB_DIR) + "/" + name + ".thm"; }

bool readCache(const std::string& path, const SourceKey& key, const int boxWidth, const int boxHeight, uint8_t* bits,
               int& width, int& height) {
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("WALL", path, file)) return false;
  CacheHeader header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (header.magic != CACHE_MAGIC || header.version != CACHE_VERSION || header.boxWidth != boxWidth ||
      header.boxHeight != boxHeight || header.width == 0 || header.width > boxWidth || header.height == 0 ||
      header.height > boxHeight || header.sourceSize != key.size || header.sourceMtime != key.mtime ||
      header.sourceSample != key.sample) {
    return false;
  }
  const int bytes = THUMB_STRIDE * header.height;
  if (file.read(bits, bytes) != bytes) return false;
  width = header.width;
  height = header.height;
  return true;
}

void writeCache(const std::string& path, const SourceKey& key, const int boxWidth, const int boxHeight,
                const uint8_t* bits, const int width, const int height) {
  if (!Storage.ensureDirectoryExists(THUMB_DIR)) return;
  HalFile file;
  if (!Storage.openFileForWrite("WALL", path, file)) return;
  const CacheHeader header{CACHE_MAGIC,
                           CACHE_VERSION,
                           0,
                           static_cast<uint16_t>(width),
                           static_cast<uint16_t>(height),
                           static_cast<uint16_t>(boxWidth),
                           static_cast<uint16_t>(boxHeight),
                           0,
                           key.size,
                           key.mtime,
                           key.sample};
  const size_t bytes = static_cast<size_t>(THUMB_STRIDE) * height;
  if (file.write(&header, sizeof(header)) != sizeof(header) || file.write(bits, bytes) != bytes) {
    LOG_ERR("WALL", "Failed to write %s", path.c_str());
    file.close();
    Storage.remove(path.c_str());
  }
}

bool parseBmpHeader(HalFile& file, BmpLayout& bmp) {
  uint8_t header[54];
  if (!file.seek(0) || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (le16(header) != 0x4D42) return false;
  bmp.dataOffset = le32(header + 10);
  const uint32_t dibSize = le32(header + 14);
  const auto width = static_cast<int32_t>(le32(header + 18));
  const auto height = static_cast<int32_t>(le32(header + 22));
  const uint16_t planes = le16(header + 26);
  bmp.bpp = le16(header + 28);
  const uint32_t compression = le32(header + 30);

  const bool bppOk = bmp.bpp == 1 || bmp.bpp == 2 || bmp.bpp == 4 || bmp.bpp == 8 || bmp.bpp == 24 || bmp.bpp == 32;
  // 32bpp BI_BITFIELDS is read as BGRA, like the sleep screen renderer does.
  if (dibSize < 40 || planes != 1 || !bppOk || !(compression == 0 || (compression == 3 && bmp.bpp == 32)) ||
      height == INT32_MIN) {
    return false;
  }
  bmp.topDown = height < 0;
  bmp.width = width;
  bmp.height = bmp.topDown ? -height : height;
  if (bmp.width <= 0 || bmp.height <= 0 || bmp.width > MAX_SOURCE_W || bmp.height > MAX_SOURCE_H) return false;
  bmp.rowBytes = (static_cast<uint32_t>(bmp.width) * bmp.bpp + 31) / 32 * 4;
  bmp.paletteOffset = 14 + dibSize;
  if (bmp.bpp <= 8) {
    const uint32_t maxColors = 1u << bmp.bpp;
    bmp.colors = le32(header + 46);
    if (bmp.colors == 0 || bmp.colors > maxColors) bmp.colors = maxColors;
  }
  return true;
}

// rawPalette needs room for 256 BGRA entries (1 KB).
bool readPalette(HalFile& file, const BmpLayout& bmp, uint8_t* paletteLum, uint8_t* rawPalette) {
  memset(paletteLum, 0, 256);
  if (bmp.colors == 0) return true;
  const int bytes = static_cast<int>(bmp.colors * 4);
  if (!file.seek(bmp.paletteOffset) || file.read(rawPalette, bytes) != bytes) return false;
  for (uint32_t i = 0; i < bmp.colors; i++) {
    paletteLum[i] = luminance(rawPalette[i * 4 + 2], rawPalette[i * 4 + 1], rawPalette[i * 4]);
  }
  return true;
}

uint8_t pixelLuminance(const uint8_t* row, const int x, const uint16_t bpp, const uint8_t* paletteLum) {
  switch (bpp) {
    case 8:
      return paletteLum[row[x]];
    case 4:
      return paletteLum[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F];
    case 2:
      return paletteLum[(row[x >> 2] >> (6 - 2 * (x & 3))) & 0x03];
    case 1:
      return paletteLum[(row[x >> 3] >> (7 - (x & 7))) & 0x01];
    case 24:
      return luminance(row[x * 3 + 2], row[x * 3 + 1], row[x * 3]);
    default:
      return luminance(row[x * 4 + 2], row[x * 4 + 1], row[x * 4]);
  }
}

// Streams the source rows under one thumbnail row and box-averages them into work.line().
class BandReader {
 public:
  BandReader(HalFile& file, const BmpLayout& bmp, const Work& work, const int outW, const int outH)
      : file(file), bmp(bmp), work(work), outW(outW), outH(outH) {}

  // Rows are read in file order (bottom-up files: ty descending), so seeks only move forward.
  int bandAt(const int step) const { return bmp.topDown ? step : outH - 1 - step; }

  bool read(const int ty) const {
    const int y0 = ty * bmp.height / outH;
    const int y1 = std::max(y0 + 1, (ty + 1) * bmp.height / outH);
    const int stride = (y1 - y0 + MAX_ROWS_PER_BAND - 1) / MAX_ROWS_PER_BAND;
    uint32_t* sums = work.sums();
    memset(sums, 0, outW * sizeof(uint32_t));
    int rows = 0;
    for (int k = 0; k < y1 - y0; k += stride) {
      const int sy = bmp.topDown ? y0 + k : y1 - 1 - k;
      const int fileRow = bmp.topDown ? sy : bmp.height - 1 - sy;
      const uint32_t offset = bmp.dataOffset + static_cast<uint32_t>(fileRow) * bmp.rowBytes;
      if ((file.position() != offset && !file.seek(offset)) ||
          file.read(work.row(), bmp.rowBytes) != static_cast<int>(bmp.rowBytes)) {
        LOG_ERR("WALL", "Short read in thumbnail row %d", fileRow);
        return false;
      }
      for (int tx = 0; tx < outW; tx++) {
        const int x0 = tx * bmp.width / outW;
        const int x1 = std::max(x0 + 1, (tx + 1) * bmp.width / outW);
        for (int x = x0; x < x1; x++) sums[tx] += pixelLuminance(work.row(), x, bmp.bpp, work.palette());
      }
      rows++;
    }
    uint8_t* line = work.line();
    for (int tx = 0; tx < outW; tx++) {
      const int x0 = tx * bmp.width / outW;
      const int x1 = std::max(x0 + 1, (tx + 1) * bmp.width / outW);
      line[tx] = static_cast<uint8_t>(sums[tx] / static_cast<uint32_t>(rows * (x1 - x0)));
    }
    return true;
  }

 private:
  HalFile& file;
  const BmpLayout& bmp;
  const Work& work;
  const int outW;
  const int outH;
};

void buildTone(const uint32_t* histogram, const uint32_t total, uint8_t* lut) {
  const uint32_t cut = total / 100;
  int lo = 0;
  uint32_t acc = 0;
  for (; lo < 255; lo++) {
    acc += histogram[lo];
    if (acc > cut) break;
  }
  int hi = 255;
  acc = 0;
  for (; hi > 0; hi--) {
    acc += histogram[hi];
    if (acc > cut) break;
  }
  if (hi < lo + MIN_LEVEL_SPAN) hi = std::min(255, lo + MIN_LEVEL_SPAN);
  if (hi - lo < MIN_LEVEL_SPAN) lo = hi - MIN_LEVEL_SPAN;

  const auto stretch = [lo, hi](const int g) -> float {
    if (g <= lo) return 0.0f;
    if (g <= hi) return KNEE * static_cast<float>(g - lo) / static_cast<float>(hi - lo);
    return KNEE + (1.0f - KNEE) * static_cast<float>(g - hi) / static_cast<float>(255 - hi);
  };
  float sum = 0.0f;
  for (int g = 0; g < 256; g++) sum += static_cast<float>(histogram[g]) * stretch(g);
  const float mean = total > 0 ? sum / static_cast<float>(total) : 0.0f;
  float gamma = 1.0f;
  if (mean > 0.0f && mean < TARGET_MEAN) gamma = std::max(MIN_GAMMA, std::log(TARGET_MEAN) / std::log(mean));
  for (int g = 0; g < 256; g++) lut[g] = static_cast<uint8_t>(std::pow(stretch(g), gamma) * 255.0f + 0.5f);
  LOG_DBG("WALL", "Thumb tone lo=%d hi=%d mean=%.3f gamma=%.2f", lo, hi, static_cast<double>(mean),
          static_cast<double>(gamma));
}

bool decodeThumb(HalFile& file, const int boxWidth, const int boxHeight, uint8_t* bits, int& width, int& height) {
  BmpLayout bmp;
  if (!parseBmpHeader(file, bmp)) return false;
  // One block per decode: fixed tables plus a single source row.
  Work work;
  work.block = makeUniqueNoThrow<uint8_t[]>(Work::ROW + bmp.rowBytes);
  if (!work.block) {
    LOG_ERR("WALL", "OOM: thumbnail work (%u bytes)", static_cast<unsigned>(Work::ROW + bmp.rowBytes));
    return false;
  }
  // The histogram area doubles as the raw palette buffer before the first pass.
  if (!readPalette(file, bmp, work.palette(), reinterpret_cast<uint8_t*>(work.histogram()))) return false;

  int outW = boxWidth;
  int outH = boxHeight;
  if (bmp.width * boxHeight > bmp.height * boxWidth) {
    outH = std::max(1, bmp.height * boxWidth / bmp.width);
  } else {
    outW = std::max(1, bmp.width * boxHeight / bmp.height);
  }
  const BandReader reader(file, bmp, work, outW, outH);

  uint32_t* histogram = work.histogram();
  memset(histogram, 0, 256 * sizeof(uint32_t));
  uint32_t samples = 0;
  for (int step = 0; step < outH; step += STATS_STRIDE) {
    if (!reader.read(reader.bandAt(step))) return false;
    for (int tx = 0; tx < outW; tx++) histogram[work.line()[tx]]++;
    samples += static_cast<uint32_t>(outW);
  }
  buildTone(histogram, samples, work.lut());

  memset(bits, 0, BITS_BYTES);
  const uint8_t* lut = work.lut();
  for (int step = 0; step < outH; step++) {
    const int ty = reader.bandAt(step);
    if (!reader.read(ty)) return false;
    const uint8_t* line = work.line();
    uint8_t* dst = bits + ty * THUMB_STRIDE;
    for (int tx = 0; tx < outW; tx++) {
      if (ordered_dither::level2(lut[line[tx]], tx, ty)) dst[tx >> 3] |= static_cast<uint8_t>(0x80 >> (tx & 7));
    }
  }
  width = outW;
  height = outH;
  return true;
}

}  // namespace

ThumbLoader::Result ThumbLoader::load(const std::string& name, int boxWidth, int boxHeight) {
  const unsigned long start = millis();
  boxWidth = std::min(std::max(boxWidth, 1), THUMB_MAX_W);
  boxHeight = std::min(std::max(boxHeight, 1), THUMB_MAX_H);
  thumbWidth = 0;
  thumbHeight = 0;
  if (!bits) bits = makeUniqueNoThrow<uint8_t[]>(BITS_BYTES);
  if (!bits) {
    LOG_ERR("WALL", "OOM: thumbnail bits");
    return Result::Failed;
  }

  HalFile source;
  if (!Storage.openFileForRead("WALL", std::string(DIR) + "/" + name, source)) return Result::Failed;
  const SourceKey key = readSourceKey(source);
  const std::string cache = cachePath(name);
  if (readCache(cache, key, boxWidth, boxHeight, bits.get(), thumbWidth, thumbHeight)) {
    LOG_INF("WALL", "Thumb %s: cache hit in %lu ms", name.c_str(), millis() - start);
    return Result::Cached;
  }
  if (!decodeThumb(source, boxWidth, boxHeight, bits.get(), thumbWidth, thumbHeight)) {
    thumbWidth = 0;
    thumbHeight = 0;
    return Result::Failed;
  }
  const unsigned long decoded = millis();
  writeCache(cache, key, boxWidth, boxHeight, bits.get(), thumbWidth, thumbHeight);
  LOG_INF("WALL", "Thumb %s: decoded %dx%d in %lu ms (+%lu ms cache write)", name.c_str(), thumbWidth, thumbHeight,
          decoded - start, millis() - decoded);
  return Result::Decoded;
}

void displayName(const std::string& fileName, char* out, const size_t outSize) {
  if (outSize == 0) return;
  size_t length = fileName.size();
  const size_t dot = fileName.rfind('.');
  if (dot != std::string::npos && dot > 0) length = dot;
  length = std::min(length, outSize - 1);
  for (size_t i = 0; i < length; i++) {
    const char c = fileName[i];
    out[i] = (c == '_' || c == '-') ? ' ' : c;
  }
  out[length] = '\0';
}

}  // namespace wallpaper
