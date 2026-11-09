#include "BookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>

namespace {
uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool loadEpubInfo(const std::string& path, BookReadInfo& out) {
  // Metadata objects exceed the stack budget.
  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("PROG", "OOM: progress metadata");
    return false;
  }
  if (!epub->load(false, true)) return false;
  const int spines = epub->getSpineItemsCount();
  if (spines <= 0 || epub->getBookSize() == 0) return false;
  out.contentBytes = static_cast<uint32_t>(epub->getBookSize());

  uint8_t data[10]{};
  HalFile file;
  if (!Storage.openFileForRead("PROG", epub->getCachePath() + "/progress.bin", file)) return true;
  const int size = file.read(data, sizeof(data));
  if (size != 4 && size != 6 && size != 10) return true;
  const int spine = data[0] | (data[1] << 8);
  const int page = data[2] | (data[3] << 8);
  const int total = size >= 6 ? data[4] | (data[5] << 8) : 0;
  if (spine == spines) {
    out.progress = 100;
    return true;
  }
  if (spine > spines) return true;
  const float fraction = total > 0 && page != UINT16_MAX ? std::clamp(float(page) / total, 0.0f, 1.0f) : 0;
  out.progress = std::clamp(static_cast<int>(epub->calculateProgress(spine, fraction) * 100 + 0.5f), 0, 100);
  if (total > 0) {
    const size_t before = spine > 0 ? epub->getCumulativeSpineItemSize(spine - 1) : 0;
    const size_t end = epub->getCumulativeSpineItemSize(spine);
    if (end > before) {
      out.sampleBytes = static_cast<uint32_t>(end - before);
      out.samplePages = static_cast<uint16_t>(total);
    }
  }
  return true;
}

bool loadXtcInfo(const std::string& path, BookReadInfo& out) {
  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("PROG", "OOM: XTC progress metadata");
    return false;
  }
  if (!xtc->load() || xtc->getPageCount() == 0) return false;
  out.exactPages = xtc->getPageCount();
  uint8_t data[4]{};
  HalFile file;
  if (!Storage.openFileForRead("PROG", xtc->getCachePath() + "/progress.bin", file) || file.read(data, 4) != 4)
    return true;
  const uint32_t page = readLe32(data);
  out.progress = page >= xtc->getPageCount() ? 100 : xtc->calculateProgress(page);
  return true;
}

}  // namespace

bool loadBookReadInfo(const std::string& path, BookReadInfo& out) {
  out = BookReadInfo{};
  // TXT and Markdown open through the EPUB pipeline and share its cache layout.
  if (FsHelpers::hasReflowableBookExtension(path)) return loadEpubInfo(path, out);
  if (FsHelpers::hasXtcExtension(path)) return loadXtcInfo(path, out);
  return false;
}

int loadBookProgress(const std::string& path) {
  BookReadInfo info;
  loadBookReadInfo(path, info);
  return info.progress;
}
