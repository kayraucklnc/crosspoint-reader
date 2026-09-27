#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <memory>

// The Library's resident catalog: one small record per indexed book (28 bytes)
// holding what sorting, filtering and the per-book badges need. Strings stay in
// the CLX1 index and are read only for the books on screen.
namespace library_ui {

enum class View : uint8_t { Shelf, Grid, List };
enum class Sort : uint8_t { Recent, Title, Author, Progress };
enum class Filter : uint8_t { All, Reading, Finished, Archived };

inline constexpr int8_t PROGRESS_UNKNOWN = -2;  // not looked up yet
inline constexpr int8_t PROGRESS_NONE = -1;     // never opened / unreadable
inline constexpr uint8_t NOT_RECENT = 0xFF;

struct Book {
  uint32_t fileSize;
  uint32_t lastRead;     // epoch seconds from reading stats, 0 unknown
  uint32_t readSeconds;  // time spent reading, from reading stats
  uint16_t authorRank;   // position in the index's author order
  uint16_t arrivalRank;  // 0 = newest on the card
  uint16_t sessions;
  uint16_t spineSig;  // tonal profile of the cover, 0 until derived
  int8_t progress;
  uint8_t recentRank;  // position in RecentBooksStore, NOT_RECENT otherwise
  uint8_t flags;

  enum : uint8_t {
    ARCHIVED = 1 << 0,
    MARKED_FINISHED = 1 << 1,  // Library meta
    STATS_FINISHED = 1 << 2,   // reading stats
    OPENED = 1 << 3,           // a saved position or reading history exists
    OPENED_KNOWN = 1 << 4,
  };

  bool archived() const { return flags & ARCHIVED; }
  bool finished() const { return (flags & (MARKED_FINISHED | STATS_FINISHED)) || progress >= 100; }
  bool reading() const { return (flags & OPENED) && !finished(); }
};

class LibraryModel {
 public:
  // Book `i` is index record `i` (title order). Pulls reading stats, Library
  // meta and the recent list in alongside. False on OOM or an index read error.
  bool load(library::LibraryIndexFile& index);

  // Rebuilds the visible order. Filters that depend on progress assume
  // progressKnown() for every book (see needsAllProgress).
  bool arrange(Sort sort, Filter filter);
  static bool needsAllProgress(Sort sort, Filter filter) {
    return sort == Sort::Progress || filter == Filter::Reading || filter == Filter::Finished;
  }

  uint16_t total() const { return bookCount; }
  // Recent books that exist on the card but not in the index: the index is
  // stale (books copied without the transfer screen marking it dirty).
  int missingRecents() const { return unmatchedRecents; }
  int count() const { return orderCount; }
  uint16_t bookAt(const int position) const { return order[position]; }
  Book& book(const uint16_t i) { return books[i]; }
  const Book& book(const uint16_t i) const { return books[i]; }
  // Position of book `i` in the current order, or -1 when filtered out.
  int positionOf(uint16_t i) const;

 private:
  std::unique_ptr<Book[]> books;
  std::unique_ptr<uint16_t[]> order;
  uint16_t bookCount = 0;
  uint16_t orderCount = 0;
  int unmatchedRecents = 0;
};

}  // namespace library_ui
