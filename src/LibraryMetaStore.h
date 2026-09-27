#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Per-book shelf state the Library owns (archived / marked finished) plus the
// Library's remembered view, sort and filter. Books are keyed by SD path and
// never moved: archiving is only a flag. Entries exist only for books with a
// flag set, so the file stays a handful of lines on a typical card.
class LibraryMetaStore : public PersistableStore<LibraryMetaStore> {
 public:
  enum Flag : uint8_t { ARCHIVED = 1 << 0, FINISHED = 1 << 1 };
  static constexpr uint8_t VIEW_COUNT = 3;
  static constexpr uint8_t SORT_COUNT = 4;
  static constexpr uint8_t FILTER_COUNT = 4;
  // Hard cap so a hand-edited file cannot grow the resident list unbounded.
  static constexpr size_t MAX_ENTRIES = 512;

  struct Entry {
    std::string path;
    uint8_t flags;
  };

  static const char* getFilePath() { return "/.crosspoint/library_meta.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  uint8_t flagsFor(const std::string& path) const;
  bool isArchived(const std::string& path) const { return (flagsFor(path) & ARCHIVED) != 0; }
  // Sets or clears `flag` for `path` and persists. Returns the new flags.
  uint8_t toggle(const std::string& path, Flag flag);
  const std::vector<Entry>& entries() const { return books; }

  uint8_t view = 0;    // 0 shelf, 1 grid, 2 list
  uint8_t sort = 0;    // 0 recent, 1 title, 2 author, 3 progress
  uint8_t filter = 0;  // 0 all (not archived), 1 reading, 2 finished, 3 archived
  void savePrefs(uint8_t newView, uint8_t newSort, uint8_t newFilter);

 private:
  std::vector<Entry> books;

  LibraryMetaStore() = default;
  ~LibraryMetaStore() = default;
  friend class PersistableStore<LibraryMetaStore>;
};

#define LIBRARY_META LibraryMetaStore::getInstance()
