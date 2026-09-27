#include "BookThumbs.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <functional>

#include "components/GeneratedCover.h"

namespace book_thumbs {
namespace {
constexpr const char* CACHE_ROOT = "/.crosspoint";

// Title for a generated cover when the caller has none: the filename stem.
std::string stemOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.rfind('.');
  return path.substr(start, dot == std::string::npos || dot < start ? std::string::npos : dot - start);
}
}  // namespace

std::string cacheDirFor(const std::string& bookPath) {
  const char* prefix = "/txt_";
  if (FsHelpers::hasEpubExtension(bookPath)) prefix = "/epub_";
  if (FsHelpers::hasXtcExtension(bookPath)) prefix = "/xtc_";
  return std::string(CACHE_ROOT) + prefix + std::to_string(std::hash<std::string>{}(bookPath));
}

std::string thumbPath(const std::string& bookPath, const int height) {
  return cacheDirFor(bookPath) + "/thumb_" + std::to_string(height) + ".bmp";
}

bool ensure(GfxRenderer& renderer, const std::string& bookPath, const int height, const char* title,
            const char* author) {
  const std::string path = thumbPath(bookPath, height);
  if (Storage.exists(path.c_str())) return true;

  bool extracted = false;
  if (FsHelpers::hasEpubExtension(bookPath)) {
    // Parser objects exceed the stack budget; one lives at a time.
    auto epub = makeUniqueNoThrow<Epub>(bookPath, CACHE_ROOT);
    if (!epub) {
      LOG_ERR("THUMB", "OOM: cover EPUB");
      return false;
    }
    extracted = epub->generateThumbBmpFromSource(height);
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, CACHE_ROOT);
    if (!xtc) {
      LOG_ERR("THUMB", "OOM: cover XTC");
      return false;
    }
    extracted = xtc->load() && xtc->generateThumbBmp(height);
  }
  if (extracted) return true;

  Storage.mkdir(cacheDirFor(bookPath).c_str());
  const std::string fallbackTitle = title && title[0] ? std::string() : stemOf(bookPath);
  LOG_DBG("THUMB", "Generating cover for %s", bookPath.c_str());
  return generated_cover::writeThumb(renderer, path, generatedWidth(height), height,
                                     fallbackTitle.empty() ? title : fallbackTitle.c_str(), author ? author : "");
}

bool wasOpened(const std::string& bookPath) {
  return Storage.exists((cacheDirFor(bookPath) + "/progress.bin").c_str());
}

}  // namespace book_thumbs
