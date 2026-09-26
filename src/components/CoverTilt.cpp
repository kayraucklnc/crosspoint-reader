#include "CoverTilt.h"

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <OrderedDither.h>

#include <cstring>

namespace cover_tilt {
namespace {
constexpr int MAX_DIM = 1024;

// 2-bit packed level (0 black .. 3 white).
inline uint8_t getLevel(const uint8_t* buf, const int stride, const int x, const int y) {
  return (buf[y * stride + (x >> 2)] >> (6 - (x & 3) * 2)) & 3;
}
inline void setLevel(uint8_t* buf, const int stride, const int x, const int y, const uint8_t v) {
  uint8_t& b = buf[y * stride + (x >> 2)];
  const int shift = 6 - (x & 3) * 2;
  b = static_cast<uint8_t>((b & ~(3 << shift)) | (v << shift));
}

void writeLE16(uint8_t* p, const uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
void writeLE32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

// 2-bit bottom-up BMP with the four native gray levels.
bool writeBmp2(const std::string& path, const uint8_t* pixels, const int width, const int height, const int stride) {
  const int rowBytes = (width * 2 + 31) / 32 * 4;
  uint8_t header[54 + 16] = {};
  header[0] = 'B';
  header[1] = 'M';
  writeLE32(header + 2, sizeof(header) + rowBytes * height);
  writeLE32(header + 10, sizeof(header));
  writeLE32(header + 14, 40);
  writeLE32(header + 18, width);
  writeLE32(header + 22, height);
  writeLE16(header + 26, 1);
  writeLE16(header + 28, 2);
  writeLE32(header + 34, rowBytes * height);
  writeLE32(header + 46, 4);
  for (int i = 0; i < 4; i++) memset(header + 54 + i * 4, i * 85, 3);

  HalFile file;
  if (!Storage.openFileForWrite("TILT", path.c_str(), file)) return false;
  if (file.write(header, sizeof(header)) != sizeof(header)) return false;
  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!row) return false;
  for (int y = height - 1; y >= 0; y--) {
    memset(row.get(), 0, rowBytes);
    memcpy(row.get(), pixels + y * stride, stride);
    if (file.write(row.get(), rowBytes) != static_cast<size_t>(rowBytes)) return false;
  }
  return true;
}
}  // namespace

std::string pathFor(const std::string& thumbPath, const Side side) {
  const size_t dot = thumbPath.rfind('.');
  const std::string suffix = side == Side::LEFT ? "_L" : "_R";
  return dot == std::string::npos ? thumbPath + suffix : thumbPath.substr(0, dot) + suffix + thumbPath.substr(dot);
}

bool ensure(const std::string& thumbPath, const Side side) {
  const std::string outPath = pathFor(thumbPath, side);
  if (Storage.exists(outPath.c_str())) return true;

  HalFile in;
  if (!Storage.openFileForRead("TILT", thumbPath, in)) return false;
  Bitmap bitmap(in, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  const int w = bitmap.getWidth();
  const int h = bitmap.getHeight();
  if (w <= 1 || h <= 1 || w > MAX_DIM || h > MAX_DIM) return false;

  // Source as packed 2-bit levels (about 18 KB for a 220x330 thumbnail).
  const int srcStride = (w + 3) / 4;
  auto src = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(srcStride) * h);
  auto row = makeUniqueNoThrow<uint8_t[]>(bitmap.getRowBytes());
  if (!src || !row) {
    LOG_ERR("TILT", "OOM: %dx%d", w, h);
    return false;
  }
  for (int i = 0; i < h; i++) {
    const int y = bitmap.isTopDown() ? i : h - 1 - i;
    if (bitmap.readNextRow(src.get() + y * srcStride, row.get()) != BmpReaderError::Ok) return false;
  }

  const int ow = w * WIDTH_PERCENT / 100;
  const int outStride = (ow + 3) / 4;
  auto out = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(outStride) * h);
  if (!out) return false;
  memset(out.get(), 0xFF, static_cast<size_t>(outStride) * h);  // white

  // Depth runs from zFar (far edge) to 1 (near edge); projected size is 1/z.
  // Screen position of source column u in [0,1]: t(u) = (u/z(u)) / 1, so we
  // invert it per output column. far = 0 side of u.
  const float zFar = 100.0f / FAR_SCALE_PERCENT;
  const auto z = [zFar](const float u) { return zFar + (1.0f - zFar) * u; };
  const float tNear = 1.0f / z(1.0f);
  for (int c = 0; c < ow; c++) {
    const float t = (ow == 1 ? 1.0f : static_cast<float>(c) / (ow - 1));  // 0 at far edge
    // Solve t = (u / z(u)) / tNear for u: u = t*tNear*zFar / (1 - t*tNear*(1 - zFar)).
    const float k = t * tNear;
    const float u = k * zFar / (1.0f - k * (1.0f - zFar));
    const float scale = 1.0f / z(u);  // column height relative to the near edge
    const int colH = static_cast<int>(h * scale + 0.5f);
    const int top = (h - colH) / 2;
    const int sx = std::min(w - 1, static_cast<int>(u * (w - 1) + 0.5f));
    const int dx = side == Side::LEFT ? c : ow - 1 - c;  // left neighbour: far edge on the left
    const int srcX = side == Side::LEFT ? sx : w - 1 - sx;
    for (int y = 0; y < colH; y++) {
      const int sy = std::min(h - 1, y * h / std::max(1, colH));
      setLevel(out.get(), outStride, dx, top + y, getLevel(src.get(), srcStride, srcX, sy));
    }
    // Hairline edges keep the trapezoid readable against white.
    setLevel(out.get(), outStride, dx, top, 0);
    setLevel(out.get(), outStride, dx, top + colH - 1, 0);
  }
  const int nearX = side == Side::LEFT ? ow - 1 : 0;
  for (int y = 0; y < h; y++) setLevel(out.get(), outStride, nearX, y, 0);

  return writeBmp2(outPath, out.get(), ow, h, outStride);
}

void invalidateIfDitherChanged(const std::string& thumbPath) {
  const size_t slash = thumbPath.rfind('/');
  if (slash == std::string::npos) return;
  const std::string dir = thumbPath.substr(0, slash);
  const std::string marker = dir + "/dither.mode";
  const char want = static_cast<char>('0' + ordered_dither::mode);
  if (Storage.exists(marker.c_str())) {
    const String current = Storage.readFile(marker.c_str());
    if (current.length() > 0 && current[0] == want) return;
  }
  auto folder = Storage.open(dir.c_str());
  if (folder && folder.isDirectory()) {
    char name[64];
    std::string stale[16];
    int count = 0;
    for (auto file = folder.openNextFile(); file && count < 16; file = folder.openNextFile()) {
      file.getName(name, sizeof(name));
      if (strncmp(name, "thumb_", 6) == 0) stale[count++] = dir + "/" + name;
    }
    folder.close();
    for (int i = 0; i < count; i++) Storage.remove(stale[i].c_str());
    if (count) LOG_INF("TILT", "Dither mode changed; dropped %d cached thumbs in %s", count, dir.c_str());
  }
  const char text[2] = {want, '\0'};
  Storage.writeFile(marker.c_str(), String(text));
}

}  // namespace cover_tilt
