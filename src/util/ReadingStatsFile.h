#pragma once

// Internal to ReadingStats: the streaming reader/writer for
// /.crosspoint/reading_stats.json and its calendar-day keys.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "ReadingStats.h"

namespace reading_stats::file {

constexpr const char* TMP_PATH = "/.crosspoint/reading_stats.json.tmp";

using BookFn = void (*)(void* ctx, const char* path, const BookStats& stats);
using DayFn = void (*)(void* ctx, const char* day, uint32_t sec);

// One SAX pass over the stats file. Returns false when it does not exist or
// cannot be parsed. `recover` must be false while a rewrite has the temp file open.
bool scanFile(BookFn onBook, DayFn onDay, void* ctx, bool recover = true);
// A crash between remove() and rename() in a rewrite leaves only the temp file.
void recoverTempFile();

void formatDayKey(int32_t day, char* buf, size_t len);
bool parseDayKey(const char* s, int32_t& out);

// Buffered JSON writer for the stats file's entries.
class JsonOut {
 public:
  explicit JsonOut(HalFile& file) : file(file) {}

  void raw(const char* s);
  void raw(const char* s, size_t len);
  void quoted(const char* s);
  void book(const char* path, const BookStats& b, bool first);
  void day(int32_t dayNumber, uint32_t sec, bool first);
  // Flushes the buffer; false if any write came up short.
  bool finish();

 private:
  void drain();

  HalFile& file;
  char buf[256] = {};
  size_t used = 0;
  bool ok = true;
};

}  // namespace reading_stats::file
