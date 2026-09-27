#include "LibraryModel.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <vector>

#include "LibraryMetaStore.h"
#include "RecentBooksStore.h"
#include "util/ReadingStatsReader.h"

namespace library_ui {
namespace {

// Recency: the recent list's order first (it is the true "last opened"), then
// reading-stats timestamps, then arrival on the card.
bool recentBefore(const Book& a, const Book& b) {
  if (a.recentRank != b.recentRank) return a.recentRank < b.recentRank;
  if (a.lastRead != b.lastRead) return a.lastRead > b.lastRead;
  return a.arrivalRank < b.arrivalRank;
}

// One comparator object for every sort keeps std::sort to a single instantiation.
struct OrderBy {
  const Book* books;
  Sort sort;
  bool operator()(const uint16_t a, const uint16_t b) const {
    const Book& x = books[a];
    const Book& y = books[b];
    switch (sort) {
      case Sort::Title:
        return a < b;
      case Sort::Author:
        return x.authorRank != y.authorRank ? x.authorRank < y.authorRank : a < b;
      case Sort::Progress: {
        // Finished books count as 100% even without a saved position.
        const int px = x.finished() ? 100 : x.progress;
        const int py = y.finished() ? 100 : y.progress;
        return px != py ? px > py : recentBefore(x, y);
      }
      case Sort::Recent:
      default:
        return recentBefore(x, y);
    }
  }
};

bool passes(const Book& book, const Filter filter) {
  switch (filter) {
    case Filter::Reading:
      return !book.archived() && book.reading();
    case Filter::Finished:
      return !book.archived() && book.finished();
    case Filter::Archived:
      return book.archived();
    case Filter::All:
    default:
      return !book.archived();
  }
}
}  // namespace

bool LibraryModel::load(library::LibraryIndexFile& index) {
  bookCount = 0;
  orderCount = 0;
  const uint16_t n = index.bookCount();
  books = n ? makeUniqueNoThrow<Book[]>(n) : nullptr;
  order = n ? makeUniqueNoThrow<uint16_t[]>(n) : nullptr;
  if (n && (!books || !order)) {
    LOG_ERR("LIBM", "OOM: catalog of %u books", static_cast<unsigned>(n));
    books.reset();
    order.reset();
    return false;
  }

  std::vector<reading_stats::BookStats> stats;
  reading_stats::load(stats);

  // Hashes of the few paths flagged in the meta store and the recent list.
  const auto& metaEntries = LIBRARY_META.entries();
  const auto& recents = RECENT_BOOKS.getBooks();
  std::vector<uint64_t> metaHashes;
  metaHashes.reserve(metaEntries.size());
  for (const auto& e : metaEntries) metaHashes.push_back(library::clixPathHash(e.path.data(), e.path.size()));
  uint64_t recentHashes[RecentBooksStore::MAX_RECENT_BOOKS];
  const int recentCount = std::min<int>(static_cast<int>(recents.size()), RecentBooksStore::MAX_RECENT_BOOKS);
  for (int i = 0; i < recentCount; i++) {
    recentHashes[i] = library::clixPathHash(recents[i].path.data(), recents[i].path.size());
  }

  for (uint16_t i = 0; i < n; i++) {
    Book& book = books[i];
    book = Book{};
    book.progress = PROGRESS_UNKNOWN;
    book.recentRank = NOT_RECENT;
    library::ClixRecord record{};
    uint64_t hash = 0;
    if (!index.readRecord(i, record) || !index.readPathHash(record, hash)) {
      LOG_ERR("LIBM", "index read failed at record %u", static_cast<unsigned>(i));
      return false;
    }
    book.fileSize = record.fileSize;
    if (const auto* s = reading_stats::find(stats, hash)) {
      book.readSeconds = s->seconds;
      book.lastRead = s->lastRead;
      book.sessions = s->sessions;
      book.flags |= Book::OPENED | Book::OPENED_KNOWN;
      if (s->finished) book.flags |= Book::STATS_FINISHED;
    }
    for (size_t m = 0; m < metaHashes.size(); m++) {
      if (metaHashes[m] != hash) continue;
      if (metaEntries[m].flags & LibraryMetaStore::ARCHIVED) book.flags |= Book::ARCHIVED;
      if (metaEntries[m].flags & LibraryMetaStore::FINISHED) book.flags |= Book::MARKED_FINISHED;
    }
    for (int r = 0; r < recentCount; r++) {
      if (recentHashes[r] != hash) continue;
      book.recentRank = static_cast<uint8_t>(r);
      book.flags |= Book::OPENED | Book::OPENED_KNOWN;
      break;
    }
  }
  for (uint16_t k = 0; k < n; k++) {
    const uint16_t byAuthor = index.ordinalForRow(library::SortOrder::AuthorAsc, k);
    const uint16_t byArrival = index.ordinalForRow(library::SortOrder::RecentDesc, k);
    if (byAuthor < n) books[byAuthor].authorRank = k;
    if (byArrival < n) books[byArrival].arrivalRank = k;
  }
  unmatchedRecents = 0;
  for (int r = 0; r < recentCount; r++) {
    bool matched = false;
    for (uint16_t i = 0; i < n && !matched; i++) matched = books[i].recentRank == r;
    if (!matched && !RecentBooksStore::isMissing(recents[r])) unmatchedRecents++;
  }
  bookCount = n;
  LOG_DBG("LIBM", "catalog: %u books, %d with stats", static_cast<unsigned>(n), static_cast<int>(stats.size()));
  return true;
}

bool LibraryModel::arrange(const Sort sort, const Filter filter) {
  orderCount = 0;
  for (uint16_t i = 0; i < bookCount; i++) {
    if (passes(books[i], filter)) order[orderCount++] = i;
  }
  // Shell sort: a few hundred bytes of code instead of std::sort's introsort,
  // and fast enough for the <= 4096-book index.
  const OrderBy before{books.get(), sort};
  uint16_t* o = order.get();
  for (int gap = orderCount / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11) {
    for (int i = gap; i < orderCount; i++) {
      const uint16_t v = o[i];
      int j = i;
      for (; j >= gap && before(v, o[j - gap]); j -= gap) o[j] = o[j - gap];
      o[j] = v;
    }
  }
  return true;
}

int LibraryModel::positionOf(const uint16_t i) const {
  for (int p = 0; p < orderCount; p++) {
    if (order[p] == i) return p;
  }
  return -1;
}

}  // namespace library_ui
