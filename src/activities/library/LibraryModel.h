#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LibraryLengthCache.h"

// The Library's resident catalog: one 20-byte record per indexed book holding
// what sorting, filtering and the row details need. Strings stay in the CLX1
// index and are read only for the books on screen.
namespace library_ui {

enum class Sort : uint8_t { Recent, Title, Author, Progress, Time, Length };
enum class Filter : uint8_t { All, Reading, Finished, Archived };
inline constexpr int SORT_COUNT = 6;
inline constexpr int FILTER_COUNT = 4;

inline constexpr int8_t PROGRESS_UNKNOWN = -2;  // not looked up yet
inline constexpr int8_t PROGRESS_NONE = -1;     // never opened / unreadable
inline constexpr uint8_t NOT_RECENT = 0xFF;

struct Book {
  uint32_t lastRead;     // epoch seconds from reading stats, 0 unknown
  uint32_t readSeconds;  // active reading time from reading stats
  uint32_t length;       // content bytes, or pages with LENGTH_PAGES; 0 unknown
  uint16_t authorRank;   // position in the index's author order
  uint16_t arrivalRank;  // 0 = newest on the card
  int8_t progress;
  uint8_t recentRank;  // position in RecentBooksStore, NOT_RECENT otherwise
  uint8_t flags;
  uint8_t info;

  enum : uint8_t {
    ARCHIVED = 1 << 0,
    MARKED_FINISHED = 1 << 1,    // Library meta
    MARKED_UNFINISHED = 1 << 2,  // Library meta, overrides the rest
    STATS_FINISHED = 1 << 3,     // reading stats
    OPENED = 1 << 4,             // a saved position or reading history exists
    OPENED_KNOWN = 1 << 5,
  };
  enum : uint8_t {
    READ_INFO_LOADED = 1 << 0,  // progress / reader caches consulted
    LENGTH_TRIED = 1 << 1,      // no further length lookup is worth doing
    LENGTH_PAGES = 1 << 2,      // `length` holds pages rather than bytes
    PAGES_EXACT = 1 << 3,       // the page count is the reader's own, not an estimate
  };

  bool archived() const { return flags & ARCHIVED; }
  bool finished() const {
    if (flags & MARKED_UNFINISHED) return false;
    return (flags & (MARKED_FINISHED | STATS_FINISHED)) || progress >= 100;
  }
  bool reading() const { return (flags & OPENED) && !finished(); }
};

static_assert(sizeof(Book) == 20, "the catalog keeps one Book per indexed book resident");

class LibraryModel {
 public:
  // Book `i` is index record `i` (title order). Pulls reading stats, Library
  // meta, the recent list and cached lengths in alongside. False on OOM or an
  // index read error.
  bool load(library::LibraryIndexFile& index);

  // Rebuilds the visible order. `match` (one bit per book, may be null) is the
  // active search. Sorts and filters that need per-book details assume
  // ensureDetails() ran for every book (see needsAllDetails).
  void arrange(Sort sort, bool descending, Filter filter, const uint8_t* match);
  static bool needsAllDetails(Sort sort, Filter filter) {
    return sort == Sort::Progress || sort == Sort::Length || filter == Filter::Reading || filter == Filter::Finished;
  }

  // Looks up what the book's row shows: whether it was opened, its progress
  // and its length. `path` is the book's path (read by the caller). Returns
  // true when anything was loaded from the card.
  bool ensureDetails(uint16_t i, const std::string& path, uint32_t fileSize, uint64_t pathHash);
  bool detailsKnown(uint16_t i) const;
  // Writes lengths measured since the last flush to the cache. Callers flush
  // at least every FLUSH_EVERY ensureDetails() calls.
  void flushLengths();
  static constexpr int FLUSH_EVERY = 32;

  // Page count for the book's row, 0 when unknown. `exact` is false for estimates.
  uint32_t pagesFor(uint16_t i, bool& exact) const;
  void setMetaFlags(uint16_t i, uint8_t metaFlags);

  uint16_t total() const { return bookCount; }
  // Recent books that exist on the card but not in the index: the index is
  // stale (books copied without the transfer screen marking it dirty).
  int missingRecents() const { return unmatchedRecents; }
  int count() const { return orderCount; }
  uint16_t bookAt(const int position) const { return order[position]; }
  const Book& book(const uint16_t i) const { return books[i]; }
  // Position of book `i` in the current order, or -1 when filtered out.
  int positionOf(uint16_t i) const;

 private:
  struct HashSlot {
    uint64_t hash;
    uint32_t fileSize;
    uint16_t ordinal;
  };
  static void visitCachedLength(void* ctx, const library_length::Entry& entry);
  void learnBytesPerPage(uint32_t sampleBytes, uint16_t samplePages);

  std::unique_ptr<Book[]> books;
  std::unique_ptr<uint16_t[]> order;
  uint16_t bookCount = 0;
  uint16_t orderCount = 0;
  int unmatchedRecents = 0;
  uint32_t bytesPerPage = library_length::DEFAULT_BYTES_PER_PAGE;
  bool bytesPerPageLearned = false;  // from the reader, not the built-in default
  bool bytesPerPageDirty = false;
  // Only during load(): books by path hash, for matching cached lengths.
  const HashSlot* loadSlots = nullptr;
  uint16_t loadSlotCount = 0;
  std::vector<library_length::Entry> pendingLengths;
};

}  // namespace library_ui
