#include "AutoRefreshSchedule.h"

#include <cstdio>

namespace auto_refresh {

uint16_t trainsMinutes(const uint8_t choice) { return choice < TRAINS_CHOICE_COUNT ? TRAINS_MINUTES[choice] : 0; }

uint16_t incomeMinutes(const uint8_t choice) {
  return choice < INCOME_CHOICE_COUNT ? static_cast<uint16_t>(choice * INCOME_STEP_MINUTES) : 0;
}

long effectiveIntervalSeconds(const uint16_t minutes, const uint8_t failures) {
  const long seconds = static_cast<long>(minutes) * 60;
  return failures >= FAILURES_BEFORE_BACKOFF ? seconds * 2 : seconds;
}

bool inQuietHours(const int minuteOfDay, const uint8_t startHour, const uint8_t endHour) {
  if (startHour == endHour) return false;
  const int start = startHour * 60;
  const int end = endHour * 60;
  if (start < end) return minuteOfDay >= start && minuteOfDay < end;
  return minuteOfDay >= start || minuteOfDay < end;
}

time_t nextWake(const time_t now, const long intervalSeconds, const uint8_t quietStartHour,
                const uint8_t quietEndHour) {
  const time_t candidate = now + intervalSeconds;
  struct tm local;
  if (!localtime_r(&candidate, &local)) return candidate;
  if (!inQuietHours(local.tm_hour * 60 + local.tm_min, quietStartHour, quietEndHour)) return candidate;

  // The window ends on the candidate's local day, or the next one when it wraps
  // past midnight and the candidate is still before midnight.
  struct tm end = local;
  end.tm_hour = quietEndHour;
  end.tm_min = 0;
  end.tm_sec = 0;
  end.tm_isdst = -1;
  if (quietStartHour > quietEndHour && local.tm_hour >= quietStartHour) end.tm_mday += 1;
  const time_t wake = mktime(&end);
  return wake > now ? wake : candidate;
}

long sleepSecondsUntil(const time_t now, const time_t wakeAt) {
  const long seconds = static_cast<long>(wakeAt - now);
  return seconds < MIN_SLEEP_SECONDS ? MIN_SLEEP_SECONDS : seconds;
}

void formatMinutes(const uint16_t minutes, const DurationFormats& formats, char* out, const size_t size) {
  const unsigned hours = minutes / 60;
  const unsigned rest = minutes % 60;
  if (hours == 0) {
    snprintf(out, size, formats.minutes, static_cast<unsigned>(minutes));
  } else if (rest == 0) {
    snprintf(out, size, formats.hours, hours);
  } else {
    snprintf(out, size, formats.hoursMinutes, hours, rest);
  }
}

}  // namespace auto_refresh
