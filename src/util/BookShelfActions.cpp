#include "BookShelfActions.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointState.h"
#include "LibraryMetaStore.h"
#include "RecentBooksStore.h"
#include "util/BookThumbs.h"

namespace book_shelf {
namespace {
// A book leaving Home must not be reopened by the next wake-from-reader boot.
void forgetOpenBook(const std::string& path) {
  if (APP_STATE.openEpubPath != path) return;
  APP_STATE.openEpubPath.clear();
  if (!APP_STATE.saveToFile()) LOG_ERR("SHELF", "Failed to save app state");
}
}  // namespace

bool isOnHome(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  return std::any_of(books.begin(), books.end(), [&](const RecentBook& book) { return book.path == path; });
}

bool removeFromHome(const std::string& path) {
  forgetOpenBook(path);
  return RECENT_BOOKS.removeByPath(path);
}

void markFinishedAndArchive(const std::string& path) {
  LIBRARY_META.updateFlags(path, LibraryMetaStore::FINISHED | LibraryMetaStore::ARCHIVED, LibraryMetaStore::UNFINISHED);
  forgetOpenBook(path);
}

bool resetProgress(const std::string& path) {
  const std::string dir = book_thumbs::cacheDirFor(path);
  const std::string progress = dir + "/progress.bin";
  const std::string pending = progress + ".tmp";
  bool ok = true;
  if (Storage.exists(progress.c_str()) && !Storage.remove(progress.c_str())) {
    LOG_ERR("SHELF", "Cannot remove %s", progress.c_str());
    ok = false;
  }
  if (Storage.exists(pending.c_str())) Storage.remove(pending.c_str());
  LIBRARY_META.updateFlags(path, 0, LibraryMetaStore::FINISHED);
  removeFromHome(path);
  return ok;
}

}  // namespace book_shelf
