#include "XkcdPack.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr char INDEX_PATH[] = "/.crosspoint/xkcd/pack/index.bin";
constexpr char TEXT_PATH[] = "/.crosspoint/xkcd/pack/text.bin";
constexpr char MAGIC[8] = {'X', 'K', 'C', 'D', 'P', 'A', 'K', '1'};
constexpr uint16_t VERSION = 1;
constexpr size_t HEADER_SIZE = 36;
constexpr size_t RECORD_SIZE = 32;
// Pack images are sized to the panel, so a row is at most 792 px (99 bytes).
constexpr uint16_t MAX_WIDTH = 1024;
constexpr size_t MAX_STRIDE = MAX_WIDTH / 8;
constexpr size_t READ_CHUNK = 1024;

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool readRecord(HalFile& file, const uint32_t index, XkcdPack::Entry& out) {
  uint8_t raw[RECORD_SIZE];
  if (!file.seek(HEADER_SIZE + index * RECORD_SIZE) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
    return false;
  }
  out.num = u16(raw);
  out.landscape = (raw[2] & 1) != 0;
  out.dataFile = raw[3];
  out.width = u16(raw + 4);
  out.height = u16(raw + 6);
  out.year = u16(raw + 8);
  out.month = raw[10];
  out.day = raw[11];
  out.titleLen = raw[12];
  out.altLen = u16(raw + 14);
  out.textOffset = u32(raw + 16);
  out.dataOffset = u32(raw + 20);
  out.dataSize = u32(raw + 24);
  return out.num > 0 && out.width > 0 && out.width <= MAX_WIDTH && out.height > 0;
}

// Plots one row of 1-bit pixels (MSB first, 1 = ink); white bytes cost one test.
void drawRow(const GfxRenderer& renderer, const uint8_t* row, const int width, const int x, const int y) {
  const int bytes = (width + 7) / 8;
  for (int bx = 0; bx < bytes; bx++) {
    const uint8_t bits = row[bx];
    if (bits == 0) continue;
    for (int bit = 0; bit < 8; bit++) {
      const int px = bx * 8 + bit;
      if ((bits & (0x80 >> bit)) && px < width) renderer.drawPixel(x + px, y, true);
    }
  }
}
}  // namespace

bool XkcdPack::open() {
  count_ = 0;
  maxNum_ = 0;
  if (!Storage.exists(INDEX_PATH)) return false;
  HalFile file;
  if (!Storage.openFileForRead("XKCD", INDEX_PATH, file)) return false;
  uint8_t header[HEADER_SIZE];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      memcmp(header, MAGIC, sizeof(MAGIC)) != 0 || u16(header + 8) != VERSION || u16(header + 10) != RECORD_SIZE) {
    LOG_ERR("XKCD", "Ignoring pack: bad index header");
    return false;
  }
  const uint32_t count = u32(header + 12);
  if (file.fileSize() < HEADER_SIZE + static_cast<size_t>(count) * RECORD_SIZE) {
    LOG_ERR("XKCD", "Ignoring pack: index truncated (%u records)", static_cast<unsigned>(count));
    return false;
  }
  count_ = count;
  maxNum_ = u16(header + 16);
  dataFiles_ = u16(header + 18);
  LOG_INF("XKCD", "Pack: %u comics up to #%u in %u data files", static_cast<unsigned>(count_), maxNum_, dataFiles_);
  return count_ > 0;
}

bool XkcdPack::entryAt(const uint32_t index, Entry& out) const {
  if (index >= count_) return false;
  HalFile file;
  if (!Storage.openFileForRead("XKCD", INDEX_PATH, file)) return false;
  return readRecord(file, index, out);
}

uint32_t XkcdPack::lowerBound(const uint16_t num) const {
  HalFile file;
  if (count_ == 0 || !Storage.openFileForRead("XKCD", INDEX_PATH, file)) return count_;
  uint32_t lo = 0;
  uint32_t hi = count_;
  // Comic numbers are nearly dense, so probe num-1 first: usually exact.
  Entry probe;
  const uint32_t guess = num > 0 ? std::min<uint32_t>(num - 1, count_ - 1) : 0;
  if (readRecord(file, guess, probe)) {
    if (probe.num >= num) {
      hi = guess;
    } else {
      lo = guess + 1;
    }
  }
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (!readRecord(file, mid, probe)) return count_;
    if (probe.num < num) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

int32_t XkcdPack::find(const uint16_t num) const {
  const uint32_t index = lowerBound(num);
  Entry entry;
  if (index < count_ && entryAt(index, entry) && entry.num == num) return static_cast<int32_t>(index);
  return -1;
}

bool XkcdPack::readText(const Entry& entry, std::string& title, std::string& alt) const {
  HalFile file;
  if (!Storage.openFileForRead("XKCD", TEXT_PATH, file) || !file.seek(entry.textOffset)) return false;
  title.resize(entry.titleLen);
  alt.resize(entry.altLen);
  if (entry.titleLen && file.read(title.data(), entry.titleLen) != entry.titleLen) return false;
  return entry.altLen == 0 || file.read(alt.data(), entry.altLen) == entry.altLen;
}

bool XkcdPack::draw(const Entry& entry, const GfxRenderer& renderer, const int x, const int y) const {
  char path[48];
  snprintf(path, sizeof(path), "/.crosspoint/xkcd/pack/data%02u.bin", entry.dataFile);
  HalFile file;
  if (!Storage.openFileForRead("XKCD", path, file) || !file.seek(entry.dataOffset)) {
    LOG_ERR("XKCD", "Pack data missing: %s", path);
    return false;
  }
  auto chunk = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  if (!chunk) {
    LOG_ERR("XKCD", "OOM: pack read buffer");
    return false;
  }

  // Streaming PackBits: a control byte n >= 0 copies n + 1 literal bytes,
  // n < 0 repeats the next byte 1 - n times (-128 is a no-op).
  const size_t stride = (entry.width + 7) / 8;
  uint8_t row[MAX_STRIDE];
  size_t fill = 0;
  int rowY = 0;
  int literal = 0;
  int repeat = 0;
  uint32_t left = entry.dataSize;
  const auto emit = [&](const uint8_t value) {
    row[fill++] = value;
    if (fill == stride) {
      drawRow(renderer, row, entry.width, x, y + rowY);
      rowY++;
      fill = 0;
    }
  };
  while (left > 0 && rowY < entry.height) {
    const size_t want = left < READ_CHUNK ? left : READ_CHUNK;
    const int got = file.read(chunk.get(), want);
    if (got <= 0) break;
    left -= static_cast<uint32_t>(got);
    for (int i = 0; i < got && rowY < entry.height; i++) {
      const uint8_t c = chunk[i];
      if (literal > 0) {
        emit(c);
        literal--;
      } else if (repeat > 0) {
        for (int k = 0; k < repeat && rowY < entry.height; k++) emit(c);
        repeat = 0;
      } else if (c < 0x80) {
        literal = c + 1;
      } else if (c != 0x80) {
        repeat = 257 - c;  // 1 - (int8_t)c
      }
    }
  }
  if (rowY < entry.height) {
    LOG_ERR("XKCD", "Pack image #%u short: %d of %u rows", entry.num, rowY, entry.height);
    return false;
  }
  return true;
}
