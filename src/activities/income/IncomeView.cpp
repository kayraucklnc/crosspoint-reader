#include "IncomeView.h"

#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "network/IncomeCalendar.h"
#include "network/IncomeHistory.h"

namespace cal = income_calendar;

namespace {
constexpr int MAX_MONTHS_BACK = 36;
constexpr const char* EN_DASH = "\xE2\x80\x93";

struct Date {
  int year;
  int month;
  int day;
  struct tm fields;
};

// The income-calendar date `back` days before year/month/day.
Date dateBefore(const int year, const int month, const int day, const int back) {
  Date date;
  // An hour past midnight stays on the same date across DST changes.
  cal::calendarOf(cal::dayStart(year, month, day - back) + 3600, date.fields);
  date.year = date.fields.tm_year + 1900;
  date.month = date.fields.tm_mon + 1;
  date.day = date.fields.tm_mday;
  return date;
}

// Loads each month once while the period walks through it.
class MonthCursor {
 public:
  MonthCursor(const IncomeMonthLoader load, void* context, IncomeMonth& scratch)
      : load(load), context(context), scratch(scratch) {}

  const IncomeMonth& get(const int year, const int month) {
    const int key = cal::monthKey(year, month);
    if (key != loadedKey) {
      scratch = IncomeMonth{};
      load(context, year, month, scratch);
      loadedKey = key;
    }
    return scratch;
  }

 private:
  IncomeMonthLoader load;
  void* context;
  IncomeMonth& scratch;
  int loadedKey = 0;
};

// Sums days from..to; false when any of them is not on the device.
bool sumDays(const IncomeMonth& month, const int from, const int to, int64_t& cents, int& nonUsd) {
  cents = 0;
  nonUsd = 0;
  for (int day = from; day <= to; day++) {
    if (!month.isKnown(day)) return false;
    cents += month.cents[day];
    nonUsd += month.nonUsd[day];
  }
  return true;
}

void noteMissing(IncomeView& out, const int key) {
  for (int i = 0; i < out.missingCount; i++) {
    if (out.missing[i] == key) return;
  }
  if (out.missingCount < IncomeView::MAX_MISSING) out.missing[out.missingCount++] = key;
}

// A bucket from a month's days: DATA, or MISSING (a closed month is queued for fetching).
void setBucket(IncomeView& out, const int index, const bool known, const int64_t cents, const int nonUsd, const int key,
               const int anchorKey) {
  if (!known) {
    out.state[index] = IncomeView::State::MISSING;
    if (key < anchorKey) noteMissing(out, key);
    return;
  }
  out.state[index] = IncomeView::State::DATA;
  out.cents[index] = cents;
  out.nonUsd += nonUsd;
}

void formatMonth(const int year, const int month, const char* format, char* out, const size_t size) {
  struct tm fields = {};
  fields.tm_year = year - 1900;
  fields.tm_mon = month - 1;
  fields.tm_mday = 1;
  strftime(out, size, format, &fields);
}

void buildDays(const int days, const int offset, const Date& anchor, const int anchorKey, MonthCursor& months,
               IncomeView& out) {
  out.count = days;
  Date first{};
  Date last{};
  for (int i = 0; i < days; i++) {
    const Date date = dateBefore(anchor.year, anchor.month, anchor.day, offset * days + (days - 1 - i));
    if (i == 0) first = date;
    last = date;
    const IncomeMonth& month = months.get(date.year, date.month);
    const bool known = month.isKnown(date.day);
    setBucket(out, i, known, known ? month.cents[date.day] : 0, known ? month.nonUsd[date.day] : 0,
              cal::monthKey(date.year, date.month), anchorKey);
    strftime(out.labels[i], sizeof(out.labels[i]), "%a", &date.fields);
  }
  if (offset == 0) {
    out.current = days - 1;
    snprintf(out.labels[days - 1], sizeof(out.labels[0]), "%s", tr(STR_INCOME_TODAY));
  }
  char fromMonth[8];
  char toMonth[8];
  strftime(fromMonth, sizeof(fromMonth), "%b", &first.fields);
  strftime(toMonth, sizeof(toMonth), "%b", &last.fields);
  // "Sep 22–28" within a month, "Aug 29–Sep 4" across two.
  if (first.month == last.month) {
    snprintf(out.period, sizeof(out.period), "%s %d%s%d", fromMonth, first.day, EN_DASH, last.day);
  } else {
    snprintf(out.period, sizeof(out.period), "%s %d%s%s %d", fromMonth, first.day, EN_DASH, toMonth, last.day);
  }
}

void buildWeeks(const int offset, const Date& anchor, const int anchorKey, MonthCursor& months, IncomeView& out) {
  const int monthIndex = anchor.year * 12 + (anchor.month - 1) - offset;
  const int year = monthIndex / 12;
  const int month = monthIndex % 12 + 1;
  const int key = cal::monthKey(year, month);
  const bool present = key == anchorKey;
  const int days = cal::daysInMonth(year, month);
  const IncomeMonth& data = months.get(year, month);

  // Calendar weeks (Monday first), clipped to the month: 4 to 6 buckets.
  int start = 1;
  while (start <= days && out.count < IncomeView::MAX_BUCKETS) {
    int end = start;
    while (end < days && cal::weekdayMondayFirst(year, month, end + 1) != 0) end++;
    const int index = out.count++;
    if (start == end) {
      snprintf(out.labels[index], sizeof(out.labels[index]), "%d", start);
    } else {
      snprintf(out.labels[index], sizeof(out.labels[index]), "%d%s%d", start, EN_DASH, end);
    }
    if (present && start > anchor.day) {
      out.state[index] = IncomeView::State::FUTURE;
    } else {
      int64_t cents = 0;
      int nonUsd = 0;
      const bool known = sumDays(data, start, present ? std::min(end, anchor.day) : end, cents, nonUsd);
      setBucket(out, index, known, cents, nonUsd, key, anchorKey);
      if (present && anchor.day <= end) out.current = index;
    }
    start = end + 1;
  }
  formatMonth(year, month, "%b %Y", out.period, sizeof(out.period));
}

void buildYear(const int offset, const Date& anchor, const int anchorKey, MonthCursor& months, IncomeView& out) {
  const int year = anchor.year - offset;
  out.count = 12;
  for (int month = 1; month <= 12; month++) {
    const int index = month - 1;
    const int key = cal::monthKey(year, month);
    formatMonth(year, month, "%b", out.labels[index], sizeof(out.labels[index]));
    if (key > anchorKey) {
      out.state[index] = IncomeView::State::FUTURE;
      continue;
    }
    const int lastDay = key == anchorKey ? anchor.day : cal::daysInMonth(year, month);
    int64_t cents = 0;
    int nonUsd = 0;
    const bool known = sumDays(months.get(year, month), 1, lastDay, cents, nonUsd);
    setBucket(out, index, known, cents, nonUsd, key, anchorKey);
    if (key == anchorKey) out.current = index;
  }
  snprintf(out.period, sizeof(out.period), "%d", year);
}
}  // namespace

int maxIncomeOffset(const IncomeRange range) {
  switch (range) {
    case IncomeRange::LAST_5_DAYS:
      return MAX_MONTHS_BACK * 30 / 5;
    case IncomeRange::LAST_7_DAYS:
      return MAX_MONTHS_BACK * 30 / 7;
    case IncomeRange::MONTH_WEEKS:
      return MAX_MONTHS_BACK;
    default:
      return MAX_MONTHS_BACK / 12;
  }
}

void buildIncomeView(const IncomeRange range, const int offset, const time_t anchor, const IncomeMonthLoader load,
                     void* context, IncomeMonth& scratch, IncomeView& out) {
  out = IncomeView{};
  Date today;
  cal::calendarOf(anchor, today.fields);
  today.year = today.fields.tm_year + 1900;
  today.month = today.fields.tm_mon + 1;
  today.day = today.fields.tm_mday;
  const int anchorKey = cal::monthKey(today.year, today.month);
  MonthCursor months(load, context, scratch);

  switch (range) {
    case IncomeRange::LAST_7_DAYS:
      buildDays(7, offset, today, anchorKey, months, out);
      break;
    case IncomeRange::MONTH_WEEKS:
      buildWeeks(offset, today, anchorKey, months, out);
      break;
    case IncomeRange::YEAR_MONTHS:
      buildYear(offset, today, anchorKey, months, out);
      break;
    default:
      buildDays(5, offset, today, anchorKey, months, out);
      break;
  }
  for (int i = 0; i < out.count; i++) {
    if (out.state[i] == IncomeView::State::DATA) out.total += out.cents[i];
  }
}
