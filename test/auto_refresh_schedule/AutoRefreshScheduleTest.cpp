#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>

#include "autorefresh/AutoRefreshSchedule.h"

using namespace auto_refresh;

namespace {

constexpr const char* ROME_TZ = "CET-1CEST,M3.5.0,M10.5.0/3";

class AutoRefreshScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    setenv("TZ", ROME_TZ, 1);
    tzset();
  }
};

// Rome wall clock -> UTC epoch through the TZ rule under test.
time_t rome(const int year, const int month, const int day, const int hour, const int minute) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_isdst = -1;
  return mktime(&t);
}

struct tm local(const time_t t) {
  struct tm out;
  localtime_r(&t, &out);
  return out;
}

}  // namespace

TEST_F(AutoRefreshScheduleTest, ChoiceTables) {
  EXPECT_EQ(trainsMinutes(0), 0);
  EXPECT_EQ(trainsMinutes(TRAINS_DEFAULT_CHOICE), 10);
  EXPECT_EQ(trainsMinutes(TRAINS_CHOICE_COUNT - 1), 150);
  EXPECT_EQ(trainsMinutes(TRAINS_CHOICE_COUNT), 0);  // out of range reads as Off
  EXPECT_EQ(incomeMinutes(0), 0);
  EXPECT_EQ(incomeMinutes(INCOME_DEFAULT_CHOICE), 180);
  EXPECT_EQ(incomeMinutes(INCOME_CHOICE_COUNT - 1), 720);
  EXPECT_EQ(incomeMinutes(INCOME_CHOICE_COUNT), 0);
}

TEST_F(AutoRefreshScheduleTest, BackoffDoublesAfterRepeatedFailures) {
  EXPECT_EQ(effectiveIntervalSeconds(10, 0), 600);
  EXPECT_EQ(effectiveIntervalSeconds(10, FAILURES_BEFORE_BACKOFF - 1), 600);
  EXPECT_EQ(effectiveIntervalSeconds(10, FAILURES_BEFORE_BACKOFF), 1200);
  EXPECT_EQ(effectiveIntervalSeconds(10, 200), 1200);
}

TEST_F(AutoRefreshScheduleTest, QuietHoursWindow) {
  EXPECT_FALSE(inQuietHours(0 * 60 + 59, 1, 7));
  EXPECT_TRUE(inQuietHours(1 * 60, 1, 7));
  EXPECT_TRUE(inQuietHours(6 * 60 + 59, 1, 7));
  EXPECT_FALSE(inQuietHours(7 * 60, 1, 7));
  // Wrapping past midnight.
  EXPECT_TRUE(inQuietHours(23 * 60 + 30, 23, 6));
  EXPECT_TRUE(inQuietHours(2 * 60, 23, 6));
  EXPECT_FALSE(inQuietHours(12 * 60, 23, 6));
  // start == end disables quiet hours.
  EXPECT_FALSE(inQuietHours(3 * 60, 4, 4));
}

TEST_F(AutoRefreshScheduleTest, PlainIntervalOutsideQuietHours) {
  const time_t now = rome(2026, 9, 27, 14, 30);
  EXPECT_EQ(nextWake(now, 600, 1, 7), now + 600);
}

TEST_F(AutoRefreshScheduleTest, WakeInsideQuietHoursMovesToQuietEnd) {
  const time_t now = rome(2026, 9, 27, 0, 55);
  const time_t wake = nextWake(now, 600, 1, 7);  // 01:05 is quiet
  EXPECT_EQ(wake, rome(2026, 9, 27, 7, 0));
  const struct tm t = local(wake);
  EXPECT_EQ(t.tm_hour, 7);
  EXPECT_EQ(t.tm_min, 0);
}

TEST_F(AutoRefreshScheduleTest, SleepingDuringQuietHoursWakesAtQuietEnd) {
  const time_t now = rome(2026, 9, 27, 3, 12);
  EXPECT_EQ(nextWake(now, 3 * 3600, 1, 7), rome(2026, 9, 27, 7, 0));
}

TEST_F(AutoRefreshScheduleTest, LongIntervalCanJumpOverQuietHours) {
  // 23:00 + 12 h = 11:00: never inside the window, so it stays.
  const time_t now = rome(2026, 9, 27, 23, 0);
  EXPECT_EQ(nextWake(now, 12 * 3600, 1, 7), rome(2026, 9, 28, 11, 0));
}

TEST_F(AutoRefreshScheduleTest, WrappingWindowBeforeMidnightEndsNextMorning) {
  const time_t now = rome(2026, 9, 27, 22, 55);
  EXPECT_EQ(nextWake(now, 600, 23, 6), rome(2026, 9, 28, 6, 0));
}

TEST_F(AutoRefreshScheduleTest, WrappingWindowAfterMidnightEndsSameMorning) {
  const time_t now = rome(2026, 9, 28, 0, 30);
  EXPECT_EQ(nextWake(now, 600, 23, 6), rome(2026, 9, 28, 6, 0));
}

TEST_F(AutoRefreshScheduleTest, QuietEndIsLocalAcrossDstChange) {
  // Clocks go back on 25 Oct 2026 at 03:00 CEST; 07:00 is CET that morning.
  const time_t now = rome(2026, 10, 25, 0, 50);
  const time_t wake = nextWake(now, 1800, 1, 7);
  const struct tm t = local(wake);
  EXPECT_EQ(t.tm_mday, 25);
  EXPECT_EQ(t.tm_hour, 7);
  EXPECT_EQ(t.tm_min, 0);
  EXPECT_EQ(wake - now, 6 * 3600 + 10 * 60 + 3600);  // 6 h 10 min of wall clock plus the repeated hour
}

TEST_F(AutoRefreshScheduleTest, NoQuietHoursWhenStartEqualsEnd) {
  const time_t now = rome(2026, 9, 27, 2, 0);
  EXPECT_EQ(nextWake(now, 600, 5, 5), now + 600);
}

TEST_F(AutoRefreshScheduleTest, SleepSecondsHaveAFloor) {
  EXPECT_EQ(sleepSecondsUntil(1000, 1600), 600);
  EXPECT_EQ(sleepSecondsUntil(1000, 1010), MIN_SLEEP_SECONDS);
  EXPECT_EQ(sleepSecondsUntil(1000, 900), MIN_SLEEP_SECONDS);
}

TEST_F(AutoRefreshScheduleTest, DurationFormatting) {
  const DurationFormats formats{"%u min", "%u h", "%u h %02u"};
  char out[16];
  formatMinutes(10, formats, out, sizeof(out));
  EXPECT_STREQ(out, "10 min");
  formatMinutes(180, formats, out, sizeof(out));
  EXPECT_STREQ(out, "3 h");
  formatMinutes(90, formats, out, sizeof(out));
  EXPECT_STREQ(out, "1 h 30");
  const DurationFormats compact{"%um", "%uh", "%uh%02u"};
  formatMinutes(150, compact, out, sizeof(out));
  EXPECT_STREQ(out, "2h30");
}
