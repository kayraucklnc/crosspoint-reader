#include "ReadingStats.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "ReadingStatsFile.h"
#include "network/NetworkTime.h"

namespace reading_stats {

using file::BookFn;
using file::DayFn;
using file::JsonOut;
using file::parseDayKey;
using file::recoverTempFile;
using file::scanFile;
using file::TMP_PATH;

namespace {

constexpr unsigned long MIN_SAMPLE_MS = 4000;  // shorter dwell = skimming, not a speed sample
constexpr float EMA_ALPHA = 0.2f;
constexpr int MAX_PENDING_DAYS = 4;
constexpr uint16_t MIN_WPM = 30;
constexpr uint16_t MAX_WPM = 1500;
constexpr float BYTES_PER_WORD = 6.0f;
constexpr uint32_t MIN_PAGE_BYTES = 40;
constexpr uint32_t MAX_PAGE_BYTES = 20000;

uint32_t nowEpoch() { return static_cast<uint32_t>(network_time::current()); }

// --- Tracker state -----------------------------------------------------------

struct DayDelta {
  int32_t day;
  uint32_t sec;
};

struct Tracker {
  std::string path;  // open book; empty = none
  BookStats stored;  // on-disk values for `path` (pending deltas folded in after each flush)
  uint16_t globalWpm = 0;

  // Not yet written to disk.
  uint32_t addSec = 0;
  uint32_t addPages = 0;
  uint32_t addSessions = 0;
  uint32_t firstSeen = 0;
  uint32_t lastSeen = 0;
  bool finishedSet = false;
  DayDelta days[MAX_PENDING_DAYS] = {};
  uint8_t dayCount = 0;
  bool dirty = false;

  unsigned long lastActivityMs = 0;
  unsigned long pageArrivedMs = 0;
  unsigned long lastFlushMs = 0;
  uint32_t msCarry = 0;
  bool sessionCounted = false;

  int shownChapter = -1;
  uint32_t shownBytes = 0;  // text bytes on the page on screen; 0 = unknown
  float bytesPerPageEma = 0;
  float secPerPageEma = 0;
  float wpmEma = 0;
};

Tracker tracker;

float ema(const float prev, const float sample) { return prev <= 0 ? sample : prev + EMA_ALPHA * (sample - prev); }

uint16_t currentWpm() {
  if (tracker.wpmEma > 0) return static_cast<uint16_t>(tracker.wpmEma + 0.5f);
  return tracker.stored.wpm;
}

BookStats withPending(const BookStats& base) {
  BookStats out = base;
  out.sec += tracker.addSec;
  out.pages += tracker.addPages;
  out.sessions += tracker.addSessions;
  if (out.first == 0) out.first = tracker.firstSeen;
  out.last = std::max(out.last, tracker.lastSeen);
  out.finished = out.finished || tracker.finishedSet;
  out.wpm = currentWpm();
  return out;
}

bool isOpenBook(const char* path) { return !tracker.path.empty() && tracker.path == path; }

void addDaySeconds(const int32_t day, const uint32_t sec) {
  for (int i = 0; i < tracker.dayCount; i++) {
    if (tracker.days[i].day == day) {
      tracker.days[i].sec += sec;
      return;
    }
  }
  if (tracker.dayCount == MAX_PENDING_DAYS) {
    tracker.dirty = true;
    flush();
    if (tracker.dayCount == MAX_PENDING_DAYS) return;  // write failed; drop rather than overflow
  }
  tracker.days[tracker.dayCount++] = {day, sec};
}

void credit(const unsigned long ms) {
  tracker.msCarry += ms;
  const uint32_t sec = tracker.msCarry / 1000;
  tracker.msCarry %= 1000;
  if (!tracker.sessionCounted) {
    tracker.sessionCounted = true;
    tracker.addSessions++;
  }
  const uint32_t epoch = nowEpoch();
  if (epoch != 0) {
    tracker.lastSeen = epoch;
    if (tracker.stored.first == 0 && tracker.firstSeen == 0) tracker.firstSeen = epoch;
  }
  if (sec == 0) return;
  // Day first: a full pending-day table flushes, and the book total must not
  // be written without its matching day entry.
  const int32_t day = today();
  if (day != INT32_MIN) addDaySeconds(day, sec);
  tracker.addSec += sec;
  tracker.dirty = true;
}

// Credits the time since the last page turn, unless the reader went idle.
void creditActivity(const unsigned long now) {
  if (tracker.lastActivityMs != 0) {
    const unsigned long gap = now - tracker.lastActivityMs;
    if (gap <= IDLE_LIMIT_MS) credit(gap);
  }
  tracker.lastActivityMs = now;
}

// --- Rewrite -----------------------------------------------------------------

struct RewriteCtx {
  explicit RewriteCtx(HalFile& f) : out(f) {}
  JsonOut out;
  bool firstEntry = true;
  bool bookWritten = false;
  bool dayUsed[MAX_PENDING_DAYS] = {};
  int32_t cutoff = INT32_MIN;
};

void rewriteBook(void* p, const char* path, const BookStats& stats) {
  auto& c = *static_cast<RewriteCtx*>(p);
  if (isOpenBook(path)) {
    if (c.bookWritten) return;  // a duplicate key keeps its first entry only
    c.out.book(path, withPending(stats), c.firstEntry);
    c.bookWritten = true;
  } else {
    c.out.book(path, stats, c.firstEntry);
  }
  c.firstEntry = false;
}

void rewriteDay(void* p, const char* key, uint32_t sec) {
  auto& c = *static_cast<RewriteCtx*>(p);
  int32_t day;
  if (!parseDayKey(key, day) || day < c.cutoff) return;
  for (int i = 0; i < tracker.dayCount; i++) {
    if (tracker.days[i].day == day && !c.dayUsed[i]) {
      sec += tracker.days[i].sec;
      c.dayUsed[i] = true;
    }
  }
  c.out.day(day, sec, c.firstEntry);
  c.firstEntry = false;
}

bool rewrite() {
  recoverTempFile();
  HalFile f;
  if (!Storage.openFileForWrite("RST", TMP_PATH, f)) return false;
  auto ctx = makeUniqueNoThrow<RewriteCtx>(f);
  if (!ctx) {
    LOG_ERR("RST", "OOM: stats rewrite");
    return false;
  }
  const int32_t day = today();
  if (day != INT32_MIN) ctx->cutoff = day - (MAX_DAYS - 1);

  ctx->out.raw("{\"v\":1,\"books\":{\n");
  scanFile(rewriteBook, nullptr, ctx.get(), false);
  if (!ctx->bookWritten && !tracker.path.empty()) {
    ctx->out.book(tracker.path.c_str(), withPending(BookStats{}), ctx->firstEntry);
  }
  ctx->out.raw("},\"days\":{\n");
  ctx->firstEntry = true;
  scanFile(nullptr, rewriteDay, ctx.get(), false);
  for (int i = 0; i < tracker.dayCount; i++) {
    if (ctx->dayUsed[i] || tracker.days[i].day < ctx->cutoff) continue;
    ctx->out.day(tracker.days[i].day, tracker.days[i].sec, ctx->firstEntry);
    ctx->firstEntry = false;
  }
  ctx->out.raw("}}\n");
  if (!ctx->out.finish()) {
    LOG_ERR("RST", "Stats write failed");
    f.close();
    Storage.remove(TMP_PATH);
    return false;
  }
  f.close();
  if (Storage.exists(FILE_PATH) && !Storage.remove(FILE_PATH)) return false;
  return Storage.rename(TMP_PATH, FILE_PATH);
}

// --- Query helpers -----------------------------------------------------------

struct FindCtx {
  const char* path;
  BookStats stats;
  bool found = false;
};

void findBook(void* p, const char* path, const BookStats& stats) {
  auto& c = *static_cast<FindCtx*>(p);
  if (!c.found && strcmp(path, c.path) == 0) {
    c.stats = stats;
    c.found = true;
  }
}

struct WpmCtx {
  uint64_t weighted = 0;
  uint64_t seconds = 0;
  const char* path;
  BookStats stored;
  bool found = false;
};

void collectWpm(void* p, const char* path, const BookStats& stats) {
  auto& c = *static_cast<WpmCtx*>(p);
  if (!c.found && strcmp(path, c.path) == 0) {
    c.stored = stats;
    c.found = true;
  }
  if (stats.wpm >= MIN_WPM && stats.sec > 0) {
    c.weighted += static_cast<uint64_t>(stats.wpm) * stats.sec;
    c.seconds += stats.sec;
  }
}

struct VisitCtx {
  BookVisitor visitor;
  void* user;
  bool openSeen = false;
};

void visitBook(void* p, const char* path, const BookStats& stats) {
  auto& c = *static_cast<VisitCtx*>(p);
  if (isOpenBook(path)) {
    if (c.openSeen) return;
    c.openSeen = true;
    c.visitor(c.user, path, withPending(stats));
    return;
  }
  c.visitor(c.user, path, stats);
}

struct SummaryCtx {
  Summary* out;
  DayDelta days[MAX_DAYS + MAX_PENDING_DAYS];
  int dayCount = 0;
};

void addTop(Summary& s, const char* path, const BookStats& b) {
  if (b.sec == 0) return;
  int pos = s.topCount;
  while (pos > 0 && s.top[pos - 1].sec < b.sec) pos--;
  if (pos >= Summary::TOP_BOOKS) return;
  const int last = std::min<int>(s.topCount, Summary::TOP_BOOKS - 1);
  for (int i = last; i > pos; i--) s.top[i] = s.top[i - 1];
  snprintf(s.top[pos].path, sizeof(s.top[pos].path), "%s", path);
  s.top[pos].sec = b.sec;
  s.top[pos].finished = b.finished;
  if (s.topCount < Summary::TOP_BOOKS) s.topCount++;
}

void summaryBook(void* p, const char* path, const BookStats& b) {
  Summary& s = *static_cast<SummaryCtx*>(p)->out;
  s.totalSec += b.sec;
  if (b.sec > 0) s.booksStarted++;
  if (b.finished) s.booksFinished++;
  addTop(s, path, b);
}

void summaryDay(void* p, const char* key, const uint32_t sec) {
  auto& c = *static_cast<SummaryCtx*>(p);
  int32_t day;
  if (c.dayCount >= MAX_DAYS || !parseDayKey(key, day)) return;
  c.days[c.dayCount++] = {day, sec};
}

void mergePendingDays(SummaryCtx& c) {
  for (int i = 0; i < tracker.dayCount; i++) {
    bool merged = false;
    for (int j = 0; j < c.dayCount; j++) {
      if (c.days[j].day == tracker.days[i].day) {
        c.days[j].sec += tracker.days[i].sec;
        merged = true;
        break;
      }
    }
    if (!merged && c.dayCount < MAX_DAYS + MAX_PENDING_DAYS) c.days[c.dayCount++] = tracker.days[i];
  }
}

void computeDayFigures(SummaryCtx& c, const int32_t refDay) {
  Summary& s = *c.out;
  int32_t minDay = refDay;
  uint32_t weekdaySum[7] = {};
  const int32_t monday = refDay - weekdayMondayFirst(refDay);
  const int32_t firstWeek = monday - 7 * (Summary::WEEKS - 1);
  uint8_t active[(MAX_DAYS + 7) / 8] = {};  // bit i: refDay - i had >= 1 min of reading

  for (int i = 0; i < c.dayCount; i++) {
    const DayDelta& d = c.days[i];
    if (d.day > refDay || d.sec == 0) continue;
    minDay = std::min(minDay, d.day);
    weekdaySum[weekdayMondayFirst(d.day)] += d.sec;
    if (d.day == refDay) s.todaySec += d.sec;
    if (d.day >= monday) s.weekSec += d.sec;
    if (d.day >= firstWeek) s.weekTotals[(d.day - firstWeek) / 7] += d.sec;
    const int32_t age = refDay - d.day;
    if (age < MAX_DAYS && d.sec >= 60) active[age / 8] |= static_cast<uint8_t>(1u << (age % 8));
  }

  // Occurrences of each weekday in [minDay, refDay], so a weekday average
  // counts the days nothing was read too.
  const int32_t span = refDay - minDay + 1;
  const int startWeekday = weekdayMondayFirst(minDay);
  uint32_t bestAvg = 0;
  for (int w = 0; w < 7; w++) {
    const int offset = (w - startWeekday + 7) % 7;
    const int32_t occurrences = span / 7 + (offset < span % 7 ? 1 : 0);
    s.weekdayAvgSec[w] = occurrences > 0 ? weekdaySum[w] / static_cast<uint32_t>(occurrences) : 0;
    if (s.weekdayAvgSec[w] > bestAvg) {
      bestAvg = s.weekdayAvgSec[w];
      s.bestWeekday = static_cast<int8_t>(w);
    }
  }

  const auto isActive = [&active](const int age) { return (active[age / 8] >> (age % 8)) & 1; };
  int age = isActive(0) ? 0 : 1;  // today can still extend the streak
  while (age < MAX_DAYS && isActive(age)) {
    s.streakDays++;
    age++;
  }
}

}  // namespace

void formatDuration(const uint32_t sec, char* buf, const size_t len) {
  if (sec < 60) {
    snprintf(buf, len, "%s", tr(STR_DURATION_UNDER_MIN));
    return;
  }
  const unsigned mins = (sec + 30) / 60;
  if (mins < 60) {
    snprintf(buf, len, tr(STR_DURATION_MIN), mins);
  } else if (mins % 60 == 0) {
    snprintf(buf, len, tr(STR_DURATION_H), mins / 60);
  } else {
    snprintf(buf, len, tr(STR_DURATION_H_M), mins / 60, mins % 60);
  }
}

void formatTimeLeftShort(const uint32_t chapterSec, const uint32_t bookSec, const uint8_t mode, char* buf,
                         const size_t len) {
  buf[0] = '\0';
  const bool wantChapter = (mode == 1 || mode == 3) && chapterSec > 0;
  const bool wantBook = (mode == 2 || mode == 3) && bookSec > 0;
  char chapter[16];
  char book[16];
  formatDuration(chapterSec, chapter, sizeof(chapter));
  formatDuration(bookSec, book, sizeof(book));
  if (wantChapter && wantBook) {
    snprintf(buf, len, "%s \xC2\xB7 %s", chapter, book);
  } else if (wantChapter || wantBook) {
    snprintf(buf, len, tr(STR_TIME_LEFT_SHORT), wantChapter ? chapter : book);
  }
}

void formatPagesLeftShort(const int chapterPages, const int bookPages, const uint8_t mode, char* buf,
                          const size_t len) {
  const float secPerPage = secondsPerPage();
  if (secPerPage <= 0) {
    buf[0] = '\0';
    return;
  }
  formatTimeLeftShort(static_cast<uint32_t>(std::max(0, chapterPages) * secPerPage),
                      static_cast<uint32_t>(std::max(0, bookPages) * secPerPage), mode, buf, len);
}

void formatTimeLeftLong(const uint32_t chapterSec, const uint32_t bookSec, char* buf, const size_t len) {
  buf[0] = '\0';
  char duration[16];
  size_t used = 0;
  if (chapterSec > 0) {
    formatDuration(chapterSec, duration, sizeof(duration));
    used = snprintf(buf, len, tr(STR_TIME_LEFT_CHAPTER), duration);
  }
  if (bookSec > 0 && used < len) {
    formatDuration(bookSec, duration, sizeof(duration));
    if (used > 0) used += snprintf(buf + used, len - used, " \xC2\xB7 ");
    if (used < len) snprintf(buf + used, len - used, tr(STR_TIME_LEFT_BOOK), duration);
  }
}

// --- Public: queries -----------------------------------------------------------

bool getBook(const char* path, BookStats& out) {
  FindCtx ctx{path, BookStats{}};
  scanFile(findBook, nullptr, &ctx);
  if (isOpenBook(path)) {
    out = withPending(ctx.found ? ctx.stats : BookStats{});
    return true;
  }
  out = ctx.stats;
  return ctx.found;
}

uint32_t secondsForBook(const char* path) {
  BookStats stats;
  getBook(path, stats);
  return stats.sec;
}

bool isFinished(const char* path) {
  BookStats stats;
  return getBook(path, stats) && stats.finished;
}

void forEachBook(const BookVisitor visitor, void* ctx) {
  if (!visitor) return;
  VisitCtx visit{visitor, ctx};
  scanFile(visitBook, nullptr, &visit);
  if (!visit.openSeen && !tracker.path.empty() && tracker.dirty) {
    visitor(ctx, tracker.path.c_str(), withPending(BookStats{}));
  }
}

bool loadSummary(Summary& out) {
  out = Summary{};
  auto ctx = makeUniqueNoThrow<SummaryCtx>();
  if (!ctx) {
    LOG_ERR("RST", "OOM: stats summary");
    return false;
  }
  ctx->out = &out;

  // Books: pending time of the open book joins its stored entry.
  VisitCtx visit{summaryBook, ctx.get()};
  scanFile(visitBook, nullptr, &visit);
  if (!visit.openSeen && !tracker.path.empty() && tracker.addSec > 0) {
    summaryBook(ctx.get(), tracker.path.c_str(), withPending(BookStats{}));
  }

  scanFile(nullptr, summaryDay, ctx.get());
  mergePendingDays(*ctx);

  const int32_t day = today();
  out.clockValid = day != INT32_MIN;
  int32_t refDay = day;
  if (!out.clockValid) {
    // Without a clock, anchor the charts on the most recent recorded day.
    refDay = INT32_MIN;
    for (int i = 0; i < ctx->dayCount; i++) refDay = std::max(refDay, ctx->days[i].day);
    if (refDay == INT32_MIN) return true;
  }
  computeDayFigures(*ctx, refDay);
  if (!out.clockValid) {
    out.todaySec = 0;
    out.weekSec = 0;
    out.streakDays = 0;
  }
  return true;
}

// --- Public: tracker -------------------------------------------------------------

void beginBook(const char* path) {
  if (!path || !*path) return;
  if (isOpenBook(path)) return;
  endBook();

  WpmCtx ctx;
  ctx.path = path;
  scanFile(collectWpm, nullptr, &ctx);

  tracker = Tracker{};
  tracker.path = path;
  tracker.stored = ctx.stored;
  tracker.globalWpm = ctx.seconds > 0 ? static_cast<uint16_t>(ctx.weighted / ctx.seconds) : 0;
  tracker.wpmEma = ctx.stored.wpm;
  const unsigned long now = millis();
  tracker.lastActivityMs = now;
  tracker.pageArrivedMs = now;
  tracker.lastFlushMs = now;
  LOG_DBG("RST", "Open %s: %lus read, wpm %u (global %u)", path, static_cast<unsigned long>(ctx.stored.sec),
          ctx.stored.wpm, tracker.globalWpm);
}

void onPageTurn(const bool forward) {
  if (tracker.path.empty()) return;
  const unsigned long now = millis();
  creditActivity(now);
  const unsigned long dwell = now - tracker.pageArrivedMs;
  tracker.pageArrivedMs = now;
  if (!forward) return;

  tracker.addPages++;
  tracker.dirty = true;
  if (dwell < MIN_SAMPLE_MS || dwell > IDLE_LIMIT_MS) return;
  tracker.secPerPageEma = ema(tracker.secPerPageEma, dwell / 1000.0f);
  if (tracker.shownBytes == 0) return;
  const float wpm = tracker.shownBytes / BYTES_PER_WORD / (dwell / 60000.0f);
  if (wpm < MIN_WPM || wpm > MAX_WPM) return;
  tracker.wpmEma = ema(tracker.wpmEma, wpm);
}

void onPageShown(const int chapter, const uint32_t pageStart, const uint32_t pageEnd) {
  if (tracker.path.empty()) return;
  tracker.shownChapter = chapter;
  const uint32_t bytes = pageEnd > pageStart ? pageEnd - pageStart : 0;
  if (bytes < MIN_PAGE_BYTES || bytes > MAX_PAGE_BYTES) {
    tracker.shownBytes = 0;
    return;
  }
  tracker.shownBytes = bytes;
  tracker.bytesPerPageEma = ema(tracker.bytesPerPageEma, static_cast<float>(bytes));
}

void markFinished() {
  if (tracker.path.empty() || tracker.finishedSet || tracker.stored.finished) return;
  tracker.finishedSet = true;
  tracker.dirty = true;
}

void endBook() {
  if (tracker.path.empty()) return;
  creditActivity(millis());
  flush();
  tracker = Tracker{};
}

void flush() {
  if (!tracker.dirty || tracker.path.empty()) return;
  tracker.lastFlushMs = millis();
  if (!rewrite()) {
    LOG_ERR("RST", "Failed to save reading stats");
    return;
  }
  tracker.stored = withPending(tracker.stored);
  tracker.addSec = tracker.addPages = tracker.addSessions = 0;
  tracker.firstSeen = tracker.lastSeen = 0;
  tracker.finishedSet = false;
  tracker.dayCount = 0;
  tracker.dirty = false;
}

void tick() {
  if (tracker.dirty && millis() - tracker.lastFlushMs >= FLUSH_INTERVAL_MS) flush();
}

float bytesPerPage() { return tracker.bytesPerPageEma; }

float secondsPerPage() {
  const uint16_t wpm = currentWpm() >= MIN_WPM ? currentWpm() : tracker.globalWpm;
  const float bytes = tracker.bytesPerPageEma > 0 ? tracker.bytesPerPageEma : static_cast<float>(tracker.shownBytes);
  if (wpm >= MIN_WPM && bytes > 0) return bytes / BYTES_PER_WORD / wpm * 60.0f;
  return tracker.secPerPageEma;
}

}  // namespace reading_stats
