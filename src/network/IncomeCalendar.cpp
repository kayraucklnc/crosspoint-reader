#include "IncomeCalendar.h"

#include <cstdlib>

#include "CrossPointSettings.h"

namespace income_calendar {
namespace {
// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int year, const int month, const int day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + doe - 719468;
}
}  // namespace

bool utcDays() { return SETTINGS.incomeDayUtc != 0; }

void calendarOf(const time_t t, struct tm& out) {
  if (utcDays()) {
    gmtime_r(&t, &out);
  } else {
    localtime_r(&t, &out);
  }
}

time_t dayStart(int year, int month, const int day) {
  // Normalize the month first so daysFromCivil sees 1-12.
  year += (month - 1) >= 0 ? (month - 1) / 12 : -((12 - month) / 12);
  month = ((month - 1) % 12 + 12) % 12 + 1;
  if (utcDays()) {
    // UTC has no DST: plain arithmetic, no mktime (which is local).
    return static_cast<time_t>((daysFromCivil(year, month, 1) + day - 1) * 86400);
  }
  struct tm local = {};
  local.tm_year = year - 1900;
  local.tm_mon = month - 1;
  local.tm_mday = day;  // mktime normalizes rollover and DST
  local.tm_isdst = -1;
  return mktime(&local);
}

int daysInMonth(const int year, const int month) {
  static constexpr int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : DAYS[month - 1];
}

int weekdayMondayFirst(const int year, const int month, const int day) {
  // 1970-01-01 was a Thursday (Monday-first index 3).
  const int64_t days = daysFromCivil(year, month, day);
  return static_cast<int>(((days + 3) % 7 + 7) % 7);
}

uint32_t boundaryId() {
  if (utcDays()) return 0;
  const char* tz = getenv("TZ");
  uint32_t hash = 2166136261u;  // FNV-1a
  for (const char* p = tz ? tz : ""; *p; p++) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  return hash == 0 ? 1 : hash;
}

}  // namespace income_calendar
