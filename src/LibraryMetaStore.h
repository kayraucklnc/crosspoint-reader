#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

// Per-book shelf state the Library owns (archived, marked finished or marked
// unfinished) plus the Library's remembered sort and its direction. Books
// are keyed by SD path and never moved: archiving is only a flag. Entries exist
// only for books with a flag set, so the file stays a handful of lines.
class LibraryMetaStore : public PersistableStore<LibraryMetaStore> {
 public:
  // UNFINISHED overrides a finish that came from reading stats or progress.
  enum Flag : uint8_t { ARCHIVED = 1 << 0, FINISHED = 1 << 1, UNFINISHED = 1 << 2 };
  static constexpr uint8_t SORT_COUNT = 6;
  // Recent, Progress, Time and Length start newest / most / longest first.
  static constexpr uint8_t DEFAULT_DESCENDING = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5);
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
  // Sets `set`, clears `clear` for `path` and persists. Returns the new flags.
  uint8_t updateFlags(const std::string& path, uint8_t set, uint8_t clear);
  const std::vector<Entry>& entries() const { return books; }

  uint8_t sort = 0;                         // library_ui::Sort
  uint8_t descending = DEFAULT_DESCENDING;  // one bit per sort
  void savePrefs(uint8_t newSort, uint8_t newDescending);

 private:
  std::vector<Entry> books;

  LibraryMetaStore() = default;
  ~LibraryMetaStore() = default;
  friend class PersistableStore<LibraryMetaStore>;
};

#define LIBRARY_META LibraryMetaStore::getInstance()
