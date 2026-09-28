#pragma once

#include <cstdint>
#include <ctime>

struct IncomeMonth;

// Chart ranges, persisted as SETTINGS.incomeRange.
enum class IncomeRange : uint8_t { LAST_5_DAYS = 0, LAST_7_DAYS, MONTH_WEEKS, YEAR_MONTHS, COUNT };

// One period of a range, bucketed for the chart. offset counts periods back
// from the one holding the anchor day: 5 or 7 days, a month, or a year.
struct IncomeView {
  static constexpr int MAX_BUCKETS = 12;
  static constexpr int MAX_MISSING = 12;
  // FUTURE: after the anchor day. MISSING: a closed month not on the device yet.
  enum class State : uint8_t { DATA, FUTURE, MISSING };

  int64_t cents[MAX_BUCKETS] = {};
  State state[MAX_BUCKETS] = {};
  char labels[MAX_BUCKETS][12] = {};
  int count = 0;
  int current = -1;      // the bucket holding the anchor day; -1 for a past period
  int64_t total = 0;     // of the DATA buckets
  int nonUsd = 0;        // non-USD charges left out of the period
  char period[24] = "";  // "Sep 22–28", "Sep 2026", "2026"
  // Closed months (YYYYMM) the period needs but the device has not cached.
  int missing[MAX_MISSING] = {};
  int missingCount = 0;
};

// Daily income of one month from whatever is cached; `known` marks the days it has.
using IncomeMonthLoader = void (*)(void* context, int year, int month, IncomeMonth& out);

// Buckets `range` `offset` periods before the one holding `anchor`. `scratch`
// holds one month at a time; months are loaded as the period needs them.
void buildIncomeView(IncomeRange range, int offset, time_t anchor, IncomeMonthLoader load, void* context,
                     IncomeMonth& scratch, IncomeView& out);

// Offsets beyond this reach back more than MAX_MONTHS_BACK months.
int maxIncomeOffset(IncomeRange range);
