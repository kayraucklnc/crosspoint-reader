#pragma once

#include <cstdint>
#include <ctime>

// The calendar income is bucketed in: the device timezone, or UTC when the
// Income Days setting says so (Stripe's dashboard buckets by UTC day).
namespace income_calendar {

bool utcDays();

// Broken-down date of `t` in the income calendar.
void calendarOf(time_t t, struct tm& out);

// Midnight starting year/month/day in the income calendar. month is 1-12;
// out-of-range days and months roll over (day 0 = last day of the previous month).
time_t dayStart(int year, int month, int day);

int daysInMonth(int year, int month);

// 0 = Monday ... 6 = Sunday.
int weekdayMondayFirst(int year, int month, int day);

// YYYYMM
inline int monthKey(const int year, const int month) { return year * 100 + month; }

// Identifies the day boundaries cached numbers were bucketed with: 0 for UTC
// days, else a hash of the POSIX TZ rule (a timezone change rebuckets them).
uint32_t boundaryId();

}  // namespace income_calendar
