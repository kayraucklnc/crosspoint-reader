#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// Pure scheduling rules for refreshing the Income/Trains sleep snapshot from a
// deep-sleep timer wake. No hardware or settings access, so it is host-testable.
// Local time comes from the process TZ (the device timezone).
namespace auto_refresh {

// Trains choices in minutes; choice 0 is Off.
inline constexpr uint16_t TRAINS_MINUTES[] = {0, 5, 10, 15, 20, 30, 45, 60, 90, 120, 150};
inline constexpr uint8_t TRAINS_CHOICE_COUNT = sizeof(TRAINS_MINUTES) / sizeof(TRAINS_MINUTES[0]);
inline constexpr uint8_t TRAINS_DEFAULT_CHOICE = 2;  // 10 min

// Income choices: Off, then 30-minute steps up to 12 h.
inline constexpr uint16_t INCOME_STEP_MINUTES = 30;
inline constexpr uint8_t INCOME_CHOICE_COUNT = 25;
inline constexpr uint8_t INCOME_DEFAULT_CHOICE = 6;  // 3 h

inline constexpr uint8_t QUIET_START_DEFAULT_HOUR = 1;
inline constexpr uint8_t QUIET_END_DEFAULT_HOUR = 7;

// After this many failed refreshes in a row the interval doubles.
inline constexpr uint8_t FAILURES_BEFORE_BACKOFF = 6;
inline constexpr int LOW_BATTERY_PERCENT = 15;
// Shortest timer ever armed, so a clock hiccup cannot turn into a wake loop.
inline constexpr long MIN_SLEEP_SECONDS = 60;
// A timer wake this close to the planned time counts as on time. The sleep
// timer runs from the internal RC oscillator and drifts by a few percent.
inline constexpr long EARLY_WAKE_SLACK_SECONDS = 60;

uint16_t trainsMinutes(uint8_t choice);
uint16_t incomeMinutes(uint8_t choice);

// Interval to use after `failures` refreshes failed in a row.
long effectiveIntervalSeconds(uint16_t minutes, uint8_t failures);

// Whether the local minute of day falls in [startHour, endHour), wrapping past
// midnight when start > end. start == end means no quiet hours.
bool inQuietHours(int minuteOfDay, uint8_t startHour, uint8_t endHour);

// Next wake `intervalSeconds` after `now` (UTC). When that lands inside quiet
// hours it moves to the exact end of that quiet window instead.
time_t nextWake(time_t now, long intervalSeconds, uint8_t quietStartHour, uint8_t quietEndHour);

// Seconds to sleep until `wakeAt`, never below MIN_SLEEP_SECONDS.
long sleepSecondsUntil(time_t now, time_t wakeAt);

// printf formats for a duration, each taking unsigned values: minutes only
// ("%u min"), whole hours ("%u h"), hours and minutes ("%u h %02u").
struct DurationFormats {
  const char* minutes;
  const char* hours;
  const char* hoursMinutes;
};

// Formats a non-zero duration; 0 ("Off") is left to the caller.
void formatMinutes(uint16_t minutes, const DurationFormats& formats, char* out, size_t size);

}  // namespace auto_refresh
