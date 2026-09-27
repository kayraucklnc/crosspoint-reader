#pragma once

#include <cstddef>
#include <cstdint>

// Reading statistics, persisted to /.crosspoint/reading_stats.json:
//
//   {"v":1,
//    "books":{"<sd path>":{"sec":<int>,"pages":<int>,"sessions":<int>,
//                          "first":<epoch>,"last":<epoch>,"finished":<bool>,"wpm":<int>}},
//    "days":{"YYYY-MM-DD":<int seconds>}}
//
// "sec" counts active reading only: time between page turns, with any gap
// longer than IDLE_LIMIT_MS (and sleep) excluded. "days" keeps the last
// MAX_DAYS local calendar days. The file is never held in RAM: queries and the
// batched rewrite stream it through StreamingJsonParser.
//
// Threading: tracker calls come from the reader's loop task, except
// onPageShown()/secondsPerPage(), which the render task calls; those only touch
// word-sized fields, so a race costs at most one stale estimate.
namespace reading_stats {

constexpr const char* FILE_PATH = "/.crosspoint/reading_stats.json";
constexpr unsigned long IDLE_LIMIT_MS = 3UL * 60UL * 1000UL;
constexpr unsigned long FLUSH_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr int MAX_DAYS = 400;

struct BookStats {
  uint32_t sec = 0;
  uint32_t pages = 0;  // forward page turns
  uint32_t sessions = 0;
  uint32_t first = 0;  // epoch; 0 = unknown (clock never set)
  uint32_t last = 0;
  bool finished = false;
  uint16_t wpm = 0;  // smoothed words per minute (6 text bytes = 1 word); 0 = unknown
};

struct TopBook {
  char path[160];
  uint32_t sec;
  bool finished;
};

struct Summary {
  static constexpr int WEEKS = 8;
  static constexpr int TOP_BOOKS = 6;
  bool clockValid = false;  // false: day-based figures are unavailable
  uint32_t totalSec = 0;
  uint32_t todaySec = 0;
  uint32_t weekSec = 0;  // since Monday
  uint16_t streakDays = 0;
  uint16_t booksStarted = 0;
  uint16_t booksFinished = 0;
  uint32_t weekdayAvgSec[7] = {};   // Monday first: mean seconds per calendar weekday
  int8_t bestWeekday = -1;          // 0 = Monday; -1 = no data
  uint32_t weekTotals[WEEKS] = {};  // oldest first; the last entry is the current week
  TopBook top[TOP_BOOKS] = {};
  uint8_t topCount = 0;
};

// --- Queries (stats screen, library UI). Include the open book's unsaved time. ---
bool getBook(const char* path, BookStats& out);
uint32_t secondsForBook(const char* path);
bool isFinished(const char* path);
using BookVisitor = void (*)(void* ctx, const char* path, const BookStats& stats);
// One streaming pass over every stored book (use this for lists, not N x getBook).
void forEachBook(BookVisitor visitor, void* ctx);
// Returns false only when the summary could not be computed (OOM).
bool loadSummary(Summary& out);

// --- Tracker (reader activities) ---
void beginBook(const char* path);
// A page turn that moved the reader; forward turns count as pages read.
void onPageTurn(bool forward);
// The page now on screen: its start/end text offsets within `chapter`
// (end = 0 when unknown). Feeds the words-per-minute and time-left estimates.
void onPageShown(int chapter, uint32_t pageStart, uint32_t pageEnd);
void markFinished();
// Credits the last page, then flushes. Safe to call when no book is open.
void endBook();
void flush();
// Batched write: flushes when dirty and FLUSH_INTERVAL_MS has passed.
void tick();

// Estimated seconds to read one page of the open book at the current layout;
// 0 when there is nothing to base it on yet.
float secondsPerPage();
// Average text bytes per page seen this session (0 = unknown).
float bytesPerPage();

// "<1 min", "12 min", "3 h 5 m".
void formatDuration(uint32_t sec, char* buf, size_t len);
// Status-bar text for a STATUS_BAR_TIME_LEFT mode: "12 min left", "3 h 5 m left"
// or "12 min · 3 h 5 m". A 0 estimate leaves its part out; empty when nothing is known.
void formatTimeLeftShort(uint32_t chapterSec, uint32_t bookSec, uint8_t mode, char* buf, size_t len);
// formatTimeLeftShort() for page counts of the open book (0 = unknown).
void formatPagesLeftShort(int chapterPages, int bookPages, uint8_t mode, char* buf, size_t len);
// Menu headers: "12 min left in chapter · 3 h 5 m left in book".
void formatTimeLeftLong(uint32_t chapterSec, uint32_t bookSec, char* buf, size_t len);

// Local calendar day helpers (days since 1970-01-01). Monday = 0 weekday.
int32_t daysFromCivil(int y, unsigned m, unsigned d);
int weekdayMondayFirst(int32_t dayNumber);
// Today's local day number, or INT32_MIN when the clock is unknown.
int32_t today();

}  // namespace reading_stats
