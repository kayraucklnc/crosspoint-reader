#include "LibraryModel.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "LibraryMetaStore.h"
#include "RecentBooksStore.h"
#include "util/BookProgress.h"
#include "util/BookThumbs.h"
#include "util/ReadingStats.h"

namespace library_ui {
namespace {

// A chapter must be at least this long before its bytes-per-page is trusted.
constexpr uint16_t MIN_SAMPLE_PAGES = 8;
constexpr uint32_t MIN_SAMPLE_BYTES = 4096;

// Recency: the recent list's order first (it is the true "last opened"), then
// reading-stats timestamps, then arrival on the card. Negative: `a` is newer.
int compareRecency(const Book& a, const Book& b) {
  if (a.recentRank != b.recentRank) return a.recentRank < b.recentRank ? -1 : 1;
  if (a.lastRead != b.lastRead) return a.lastRead > b.lastRead ? -1 : 1;
  if (a.arrivalRank != b.arrivalRank) return a.arrivalRank < b.arrivalRank ? -1 : 1;
  return 0;
}

uint32_t pagesOf(const Book& book, const uint32_t bytesPerPage) {
  if (book.info & Book::LENGTH_PAGES) return book.length;
  if (book.length == 0 || bytesPerPage == 0) return 0;
  return std::max<uint32_t>(1, (book.length + bytesPerPage / 2) / bytesPerPage);
}

// Numeric sorts: 0 = unknown, which always sorts last whatever the direction.
uint32_t numericKey(const Book& book, const Sort sort, const uint32_t bytesPerPage) {
  switch (sort) {
    case Sort::Progress:
      if (book.finished()) return 101;
      return book.progress >= 0 ? static_cast<uint32_t>(book.progress) + 1 : 0;
    case Sort::Time:
      return book.readSeconds;
    case Sort::Length:
    default:
      return pagesOf(book, bytesPerPage);
  }
}

// One comparator object for every sort keeps the sort to a single instantiation.
struct OrderBy {
  const Book* books;
  Sort sort;
  bool descending;
  uint32_t bytesPerPage;

  bool operator()(const uint16_t a, const uint16_t b) const {
    const Book& x = books[a];
    const Book& y = books[b];
    switch (sort) {
      case Sort::Title:
        return descending ? a > b : a < b;
      case Sort::Author:
        if (x.authorRank != y.authorRank) return descending ? x.authorRank > y.authorRank : x.authorRank < y.authorRank;
        return a < b;
      case Sort::Recent: {
        const int c = compareRecency(x, y);
        if (c != 0) return descending ? c < 0 : c > 0;
        return a < b;
      }
      default: {
        const uint32_t kx = numericKey(x, sort, bytesPerPage);
        const uint32_t ky = numericKey(y, sort, bytesPerPage);
        if ((kx == 0) != (ky == 0)) return kx != 0;
        if (kx != ky) return descending ? kx > ky : kx < ky;
        const int c = compareRecency(x, y);
        return c != 0 ? c < 0 : a < b;
      }
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

struct StatSlot {
  uint64_t hash;
  uint32_t seconds;
  uint32_t lastRead;
  bool finished;
};

// Reading stats by path hash, grown without aborting on OOM (a failed growth
// only drops the remaining books' stats from the list's details).
struct StatTable {
  static constexpr int MAX = 1024;
  std::unique_ptr<StatSlot[]> slots;
  int count = 0;
  int capacity = 0;

  void add(const StatSlot& slot) {
    if (count == capacity) {
      const int grown = capacity == 0 ? 32 : std::min(capacity * 2, MAX);
      if (grown == capacity) return;
      auto bigger = makeUniqueNoThrow<StatSlot[]>(static_cast<size_t>(grown));
      if (!bigger) {
        LOG_ERR("LIBM", "OOM: reading stats table of %d", grown);
        return;
      }
      if (count > 0) memcpy(bigger.get(), slots.get(), sizeof(StatSlot) * count);
      slots = std::move(bigger);
      capacity = grown;
    }
    slots[count++] = slot;
  }
  const StatSlot* find(const uint64_t hash) const {
    const StatSlot* begin = slots.get();
    const StatSlot* end = begin + count;
    const StatSlot* it =
        std::lower_bound(begin, end, hash, [](const StatSlot& slot, const uint64_t h) { return slot.hash < h; });
    return it != end && it->hash == hash ? it : nullptr;
  }
};

void collectStat(void* ctx, const char* path, const reading_stats::BookStats& stats) {
  static_cast<StatTable*>(ctx)->add({library::clixPathHash(path, strlen(path)), stats.sec, stats.last, stats.finished});
}

uint8_t bookFlagsFromMeta(const uint8_t meta) {
  uint8_t flags = 0;
  if (meta & LibraryMetaStore::ARCHIVED) flags |= Book::ARCHIVED;
  if (meta & LibraryMetaStore::FINISHED) flags |= Book::MARKED_FINISHED;
  if (meta & LibraryMetaStore::UNFINISHED) flags |= Book::MARKED_UNFINISHED;
  return flags;
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

  // Transient: reading stats by path hash, 24 bytes per book with history.
  StatTable stats;
  reading_stats::forEachBook(collectStat, &stats);
  std::sort(stats.slots.get(), stats.slots.get() + stats.count,
            [](const StatSlot& a, const StatSlot& b) { return a.hash < b.hash; });

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
  // Transient: books by hash for the cached lengths, 16 bytes per book. Without
  // it the lengths are simply measured again.
  auto slots = n ? makeUniqueNoThrow<HashSlot[]>(n) : nullptr;
  if (n && !slots) LOG_ERR("LIBM", "OOM: length lookup for %u books", static_cast<unsigned>(n));

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
    if (slots) slots[i] = {hash, record.fileSize, i};
    if (const StatSlot* stat = stats.find(hash)) {
      book.readSeconds = stat->seconds;
      book.lastRead = stat->lastRead;
      book.flags |= Book::OPENED | Book::OPENED_KNOWN;
      if (stat->finished) book.flags |= Book::STATS_FINISHED;
    }
    for (size_t m = 0; m < metaHashes.size(); m++) {
      if (metaHashes[m] == hash) book.flags |= bookFlagsFromMeta(metaEntries[m].flags);
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

  uint32_t stored = 0;
  if (slots) {
    std::sort(slots.get(), slots.get() + n, [](const HashSlot& a, const HashSlot& b) { return a.hash < b.hash; });
    loadSlots = slots.get();
    loadSlotCount = n;
    stored = library_length::load(&LibraryModel::visitCachedLength, this);
    loadSlots = nullptr;
    loadSlotCount = 0;
  }
  bytesPerPage = stored > 0 ? stored : library_length::DEFAULT_BYTES_PER_PAGE;
  bytesPerPageLearned = stored > 0;
  bytesPerPageDirty = false;
  pendingLengths.clear();
  // Callers flush at least every FLUSH_EVERY books, so this never regrows.
  pendingLengths.reserve(FLUSH_EVERY);

  bookCount = n;
  LOG_DBG("LIBM", "catalog: %u books, %d with stats, %u bytes/page", static_cast<unsigned>(n), stats.count,
          static_cast<unsigned>(bytesPerPage));
  return true;
}

void LibraryModel::visitCachedLength(void* ctx, const library_length::Entry& entry) {
  auto* self = static_cast<LibraryModel*>(ctx);
  const HashSlot* end = self->loadSlots + self->loadSlotCount;
  const HashSlot* it = std::lower_bound(self->loadSlots, end, entry.pathHash,
                                        [](const HashSlot& slot, const uint64_t hash) { return slot.hash < hash; });
  if (it == end || it->hash != entry.pathHash || it->fileSize != entry.fileSize) return;
  Book& book = self->books[it->ordinal];
  book.length = entry.contentBytes;
  book.info = static_cast<uint8_t>((book.info & ~(Book::LENGTH_PAGES | Book::PAGES_EXACT)) | Book::LENGTH_TRIED);
}

void LibraryModel::learnBytesPerPage(const uint32_t sampleBytes, const uint16_t samplePages) {
  if (samplePages < MIN_SAMPLE_PAGES || sampleBytes < MIN_SAMPLE_BYTES) return;
  const uint32_t sample = sampleBytes / samplePages;
  if (sample < 100 || sample > 20000) return;
  // Blend with what earlier chapters taught so one dense chapter cannot swing
  // every estimate; the first sample replaces the built-in default outright.
  const uint32_t next = bytesPerPageLearned ? (bytesPerPage * 3 + sample) / 4 : sample;
  bytesPerPageLearned = true;
  if (next == bytesPerPage) return;
  bytesPerPage = next;
  bytesPerPageDirty = true;
}

bool LibraryModel::ensureDetails(const uint16_t i, const std::string& path, const uint32_t fileSize,
                                 const uint64_t pathHash) {
  Book& book = books[i];
  bool loaded = false;
  if (!(book.flags & Book::OPENED_KNOWN)) {
    book.flags |= Book::OPENED_KNOWN;
    if (book_thumbs::wasOpened(path)) book.flags |= Book::OPENED;
    loaded = true;
  }
  if (!(book.info & Book::READ_INFO_LOADED)) {
    book.info |= Book::READ_INFO_LOADED;
    BookReadInfo info;
    if ((book.flags & Book::OPENED) && loadBookReadInfo(path, info)) {
      loaded = true;
      book.progress = static_cast<int8_t>(info.progress);
      if (info.exactPages > 0) {
        book.length = info.exactPages;
        book.info |= Book::LENGTH_PAGES | Book::PAGES_EXACT | Book::LENGTH_TRIED;
      } else if (info.contentBytes > 0) {
        learnBytesPerPage(info.sampleBytes, info.samplePages);
        book.info = static_cast<uint8_t>(book.info & ~(Book::LENGTH_PAGES | Book::PAGES_EXACT));
        book.length = info.contentBytes;
        // The book's own laid-out chapter is the best scale for its length.
        if (info.samplePages >= MIN_SAMPLE_PAGES && info.sampleBytes >= MIN_SAMPLE_BYTES) {
          book.length = static_cast<uint32_t>(
              (static_cast<uint64_t>(info.contentBytes) * info.samplePages + info.sampleBytes / 2) / info.sampleBytes);
          book.info |= Book::LENGTH_PAGES;
        }
        book.info |= Book::LENGTH_TRIED;
      }
    }
    if (book.progress == PROGRESS_UNKNOWN) book.progress = PROGRESS_NONE;
  }
  if (!(book.info & Book::LENGTH_TRIED)) {
    book.info |= Book::LENGTH_TRIED;
    loaded = true;
    if (FsHelpers::hasEpubExtension(path)) {
      book.length = library_length::epubContentBytes(path);
      if (book.length > 0) pendingLengths.push_back({pathHash, fileSize, book.length});
    } else if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
      // Plain text carries no markup: weigh it like the equivalent XHTML.
      book.length = fileSize / 3 * 4;
    }
  }
  return loaded;
}

bool LibraryModel::detailsKnown(const uint16_t i) const {
  const Book& book = books[i];
  return (book.flags & Book::OPENED_KNOWN) && (book.info & Book::READ_INFO_LOADED) && (book.info & Book::LENGTH_TRIED);
}

void LibraryModel::flushLengths() {
  if (!pendingLengths.empty()) {
    library_length::append(pendingLengths.data(), static_cast<int>(pendingLengths.size()));
    pendingLengths.clear();
  }
  if (bytesPerPageDirty) {
    bytesPerPageDirty = false;
    library_length::storeBytesPerPage(bytesPerPage);
    LOG_DBG("LIBM", "learned %u bytes/page", static_cast<unsigned>(bytesPerPage));
  }
}

uint32_t LibraryModel::pagesFor(const uint16_t i, bool& exact) const {
  const Book& book = books[i];
  exact = (book.info & Book::PAGES_EXACT) != 0;
  return pagesOf(book, bytesPerPage);
}

void LibraryModel::setMetaFlags(const uint16_t i, const uint8_t metaFlags) {
  constexpr uint8_t META_BITS = Book::ARCHIVED | Book::MARKED_FINISHED | Book::MARKED_UNFINISHED;
  books[i].flags = static_cast<uint8_t>((books[i].flags & ~META_BITS) | bookFlagsFromMeta(metaFlags));
}

void LibraryModel::arrange(const Sort sort, const bool descending, const Filter filter, const uint8_t* match) {
  orderCount = 0;
  for (uint16_t i = 0; i < bookCount; i++) {
    if (match && !(match[i >> 3] & (1u << (i & 7)))) continue;
    if (passes(books[i], filter)) order[orderCount++] = i;
  }
  // Shell sort: a few hundred bytes of code instead of std::sort's introsort,
  // and fast enough for the <= 4096-book index.
  const OrderBy before{books.get(), sort, descending, bytesPerPage};
  uint16_t* o = order.get();
  for (int gap = orderCount / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11) {
    for (int i = gap; i < orderCount; i++) {
      const uint16_t v = o[i];
      int j = i;
      for (; j >= gap && before(v, o[j - gap]); j -= gap) o[j] = o[j - gap];
      o[j] = v;
    }
  }
}

int LibraryModel::positionOf(const uint16_t i) const {
  for (int p = 0; p < orderCount; p++) {
    if (order[p] == i) return p;
  }
  return -1;
}

}  // namespace library_ui
