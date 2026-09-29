#include "StripeGross.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "IncomeCache.h"
#include "IncomeCalendar.h"
#include "IncomeHistory.h"
#include "StripeCharges.h"

namespace stripe_gross {
namespace {
constexpr int WINDOW = IncomeSummary::DAY_COUNT;
constexpr time_t FUTURE_SLACK = 600;
constexpr int MAX_REFUNDED = 4;
constexpr int MAX_RUNS = 12;

int saved = 0;

struct Date {
  int year = 0;
  int month = 0;
  int day = 0;
};

// A stretch of consecutive days of one month to fetch.
struct Run {
  int year;
  int month;
  int first;
  int last;
};

struct RunList {
  Run runs[MAX_RUNS];
  int count = 0;
  bool overflow = false;

  // Adds the unknown stretches of from..to, newest first.
  template <typename Known>
  void add(const int year, const int month, const int from, const int to, const Known& known) {
    for (int day = to; day >= from;) {
      if (known(day)) {
        day--;
        continue;
      }
      const int last = day;
      while (day >= from && !known(day)) day--;
      if (count == MAX_RUNS) {
        overflow = true;
        return;
      }
      runs[count++] = Run{year, month, day + 1, last};
    }
  }
};

struct GrossWork {
  IncomeCache cache;     // this month
  IncomeMonth previous;  // the month before (window days early in a month)
  RunList runs;
};

bool deadlineReached(const unsigned long deadline) { return static_cast<long>(millis() - deadline) >= 0; }

Date dateOf(const time_t t) {
  struct tm local;
  income_calendar::calendarOf(t, local);
  return Date{local.tm_year + 1900, local.tm_mon + 1, local.tm_mday};
}

int keyOf(const int year, const int month) { return income_calendar::monthKey(year, month); }

// Routes each completed day: a final one into this month's cache or a closed
// month's history file, and a window day into the fresh window.
class DayRouter final : public stripe_charges::DaySink {
 public:
  DayRouter(const time_t now, const uint8_t mode) : now(now), mode(mode) {}

  IncomeCache* cache = nullptr;  // this month
  const char* cachePath = nullptr;
  IncomeMonth* month = nullptr;  // a closed month
  int monthKey = 0;
  const Date* window = nullptr;  // WINDOW dates, newest first
  int64_t windowCents[WINDOW] = {};
  uint16_t windowNonUsd[WINDOW] = {};
  bool windowFetched[WINDOW] = {};

  void startRun(const int year, const int monthOfYear) {
    runYear = year;
    runMonth = monthOfYear;
  }

  void day(const int day, const int64_t cents, const uint16_t nonUsd) override {
    const int key = keyOf(runYear, runMonth);
    const bool final = now >= income_calendar::dayStart(runYear, runMonth, day + 1) + SETTLE_SECONDS;
    if (final && cache && key == cache->monthKey) {
      cache->setClosed(day, cents, nonUsd);
      cacheDirty = true;
      saved++;
    } else if (final && month && key == monthKey) {
      month->set(day, cents, nonUsd);
      monthDirty = true;
      saved++;
    }
    for (int i = 0; window && i < WINDOW; i++) {
      if (window[i].year != runYear || window[i].month != runMonth || window[i].day != day) continue;
      windowCents[i] = cents;
      windowNonUsd[i] = nonUsd;
      windowFetched[i] = true;
    }
  }

  void pageDone() override { flush(); }

  void flush() {
    if (cacheDirty) cache->save(cachePath);
    if (monthDirty) income_history::save(monthKey, false, mode, *month);
    cacheDirty = false;
    monthDirty = false;
  }

 private:
  time_t now;
  uint8_t mode;
  int runYear = 0;
  int runMonth = 0;
  bool cacheDirty = false;
  bool monthDirty = false;
};

// Fetches the runs in order until one fails or the deadline stops them.
StripeClient::Result fetchRuns(const std::string& authorization, const RunList& list, const time_t now,
                               const unsigned long deadline, DayRouter& router) {
  for (int r = 0; r < list.count; r++) {
    if (r > 0 && deadlineReached(deadline)) return StripeClient::Result::PARTIAL;
    const Run& run = list.runs[r];
    router.startRun(run.year, run.month);
    const StripeClient::Result result =
        stripe_charges::fetchDays(authorization, run.year, run.month, run.first, run.last, now, deadline, router);
    if (result != StripeClient::Result::OK) return result;
  }
  return list.overflow ? StripeClient::Result::PARTIAL : StripeClient::Result::OK;
}

// A refund changes the Gross of the day its charge was made: that day is fetched again.
void forgetDay(const time_t chargeCreated, IncomeCache& cache, IncomeMonth& previous, const int previousKey,
               const uint8_t mode) {
  const Date date = dateOf(chargeCreated);
  const int key = keyOf(date.year, date.month);
  LOG_INF("STRIPE", "Refund on a charge of %04d-%02d-%02d; that day is fetched again", date.year, date.month, date.day);
  if (key == cache.monthKey) {
    cache.forget(date.day);
    return;
  }
  if (key == previousKey) {
    previous.forget(date.day);
    income_history::save(key, false, mode, previous);
    return;
  }
  auto older = makeUniqueNoThrow<IncomeMonth>();
  if (older && income_history::load(key, false, mode, *older) && older->isKnown(date.day)) {
    older->forget(date.day);
    income_history::save(key, false, mode, *older);
  }
}

// Month to date from final days plus the window; missing counts the days neither covers.
void monthTotals(const IncomeCache& cache, const Date& today, IncomeSummary& summary, int& missing) {
  summary.monthCents = 0;
  summary.nonUsdCharges = 0;
  missing = 0;
  for (int day = 1; day <= today.day; day++) {
    if (cache.isKnown(day)) {
      summary.monthCents += cache.closedCents[day];
      summary.nonUsdCharges += cache.closedNonUsd[day];
      continue;
    }
    const time_t start = income_calendar::dayStart(today.year, today.month, day);
    const time_t* found = std::find(summary.dayStart, summary.dayStart + WINDOW, start);
    if (found == summary.dayStart + WINDOW) {
      missing++;
      continue;
    }
    const auto i = found - summary.dayStart;
    summary.monthCents += summary.cents[i];
    summary.nonUsdCharges += summary.nonUsd[i];
  }
}
}  // namespace

StripeClient::Result fetchIncome(const std::string& authorization, const time_t now, const uint8_t mode,
                                 const unsigned long deadline, const bool backfillOnly, IncomeSummary& out,
                                 int& pendingDays) {
  saved = 0;
  const Date today = dateOf(now);
  const int thisKey = keyOf(today.year, today.month);
  const int previousYear = today.month == 1 ? today.year - 1 : today.year;
  const int previousMonth = today.month == 1 ? 12 : today.month - 1;
  const int previousKey = keyOf(previousYear, previousMonth);
  const char* path = IncomeCache::GROSS_PATH;

  // ~1 KB of month state: kept off the loop task's stack, which the TLS read path also needs.
  auto work = makeUniqueNoThrow<GrossWork>();
  if (!work) {
    LOG_ERR("STRIPE", "OOM: month buffers");
    stripe_http::fail(stripe_http::Failure::LOW_MEMORY, 0);
    return StripeClient::Result::LOW_MEMORY;
  }
  IncomeCache& cache = work->cache;
  IncomeMonth& previous = work->previous;
  RunList& runs = work->runs;
  income_history::load(previousKey, false, mode, previous);

  // A cache written by a run with a wrong clock (fetched "in the future") is not trusted.
  const bool trusted =
      cache.load(path) && cache.amountMode == mode && cache.lastSummary.fetchedAt <= now + FUTURE_SLACK;
  if (trusted && cache.monthKey != thisKey && cache.monthKey < thisKey) {
    // A new month: the cached month's final days move to its history file.
    IncomeMonth* closed = &previous;
    auto older = cache.monthKey == previousKey ? nullptr : makeUniqueNoThrow<IncomeMonth>();
    if (cache.monthKey != previousKey) {
      closed = older.get();
      if (closed) income_history::load(cache.monthKey, false, mode, *closed);
    }
    bool moved = false;
    for (int day = 1; closed && day <= IncomeMonth::MAX_DAYS; day++) {
      if (!cache.isKnown(day) || closed->isKnown(day)) continue;
      closed->set(day, cache.closedCents[day], cache.closedNonUsd[day]);
      moved = true;
    }
    if (moved) income_history::save(cache.monthKey, false, mode, *closed);
  }
  if (!trusted || cache.monthKey != thisKey) {
    const time_t refundsCheckedAt = trusted ? cache.refundsCheckedAt : 0;
    cache = IncomeCache{};
    cache.monthKey = thisKey;
    cache.amountMode = mode;
    cache.refundsCheckedAt = refundsCheckedAt;
  }

  Date window[WINDOW];
  time_t windowStart[WINDOW];
  for (int i = 0; i < WINDOW; i++) {
    windowStart[i] = income_calendar::dayStart(today.year, today.month, today.day - i);
    window[i] = dateOf(windowStart[i] + 3600);
  }
  // The window was refreshed by the previous step of this refresh.
  const bool windowFresh = backfillOnly && cache.hasSummary && cache.lastSummary.dayStart[0] == windowStart[0];

  if (!windowFresh) {
    if (cache.refundsCheckedAt > 0) {
      time_t refunded[MAX_REFUNDED];
      int count = 0;
      bool truncated = false;
      const StripeClient::Result result = stripe_charges::refundedCharges(
          authorization, cache.refundsCheckedAt - FUTURE_SLACK, refunded, MAX_REFUNDED, count, truncated);
      if (result != StripeClient::Result::OK) return result;
      for (int i = 0; i < count; i++) forgetDay(refunded[i], cache, previous, previousKey, mode);
      if (truncated) {
        // Too many refunds to place one by one: this month is fetched again in full.
        LOG_INF("STRIPE", "Many refunds since the last check; refetching this month");
        cache.closedKnown = 0;
      }
    }
    cache.refundsCheckedAt = now;
  }

  const int windowFirst = std::max(1, today.day - (WINDOW - 1));
  const auto knownHere = [&cache](const int day) { return cache.isKnown(day); };
  if (!windowFresh) {
    runs.add(today.year, today.month, windowFirst, today.day, knownHere);
    const int spill = WINDOW - today.day;  // window days in the month before
    if (spill > 0) {
      const int lastDay = income_calendar::daysInMonth(previousYear, previousMonth);
      runs.add(previousYear, previousMonth, lastDay - spill + 1, lastDay,
               [&previous](const int day) { return previous.isKnown(day); });
    }
  }
  runs.add(today.year, today.month, 1, windowFirst - 1, knownHere);

  DayRouter router(now, mode);
  router.cache = &cache;
  router.cachePath = path;
  router.month = &previous;
  router.monthKey = previousKey;
  router.window = window;
  const StripeClient::Result result = fetchRuns(authorization, runs, now, deadline, router);
  router.flush();

  IncomeSummary fresh;
  fresh.fetchedAt = now;
  fresh.monthStart = income_calendar::dayStart(today.year, today.month, 1);
  bool windowComplete = true;
  for (int i = 0; i < WINDOW; i++) {
    fresh.dayStart[i] = windowStart[i];
    const Date& date = window[i];
    const int key = keyOf(date.year, date.month);
    if (router.windowFetched[i]) {
      fresh.cents[i] = router.windowCents[i];
      fresh.nonUsd[i] = router.windowNonUsd[i];
    } else if (key == thisKey && cache.isKnown(date.day)) {
      fresh.cents[i] = cache.closedCents[date.day];
      fresh.nonUsd[i] = cache.closedNonUsd[date.day];
    } else if (key == previousKey && previous.isKnown(date.day)) {
      fresh.cents[i] = previous.cents[date.day];
      fresh.nonUsd[i] = previous.nonUsd[date.day];
    } else {
      windowComplete = false;
    }
  }
  if (!windowFresh && windowComplete) {
    cache.lastSummary = fresh;
    cache.hasSummary = true;
  }
  int missing = 0;
  if (cache.hasSummary) monthTotals(cache, today, cache.lastSummary, missing);
  pendingDays = missing;
  cache.save(path);
  out = cache.hasSummary ? cache.lastSummary : fresh;
  LOG_INF("STRIPE", "Gross: %d run(s), %d day(s) of this month still missing", runs.count, missing);

  if (result != StripeClient::Result::OK && result != StripeClient::Result::PARTIAL) return result;
  if (!cache.hasSummary || (!windowFresh && !windowComplete) || missing > 0) return StripeClient::Result::PARTIAL;
  return StripeClient::Result::OK;
}

StripeClient::Result fetchMonth(const std::string& authorization, const time_t now, const uint8_t mode, const int year,
                                const int month, const unsigned long deadline, IncomeMonth& out) {
  saved = 0;
  const time_t end = income_calendar::dayStart(year, month + 1, 1);
  if (end + SETTLE_SECONDS > now) {
    LOG_ERR("STRIPE", "Month %04d-%02d has not settled; not caching it", year, month);
    return StripeClient::Result::NO_TIME;
  }
  const int key = keyOf(year, month);
  income_history::load(key, false, mode, out);  // a partial month keeps its fetched days

  RunList runs;
  const int days = income_calendar::daysInMonth(year, month);
  runs.add(year, month, 1, days, [&out](const int day) { return out.isKnown(day); });

  DayRouter router(now, mode);
  router.month = &out;
  router.monthKey = key;
  const StripeClient::Result result = fetchRuns(authorization, runs, now, deadline, router);
  router.flush();
  if (result != StripeClient::Result::OK) return result;
  for (int day = 1; day <= days; day++) {
    if (!out.isKnown(day)) return StripeClient::Result::PARTIAL;
  }
  return StripeClient::Result::OK;
}

int daysSaved() { return saved; }

}  // namespace stripe_gross
