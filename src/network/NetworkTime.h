#pragma once

#include <ctime>

// Wall-clock time for network features. The ESP32 system clock runs from the
// internal RC oscillator, is 1970 after a cold boot and drifts across deep
// sleep, so it is never trusted on its own: the RTC (when fitted) and NTP are.
namespace network_time {

// Best current UTC without the network: the system clock when it was set from
// NTP or the RTC this boot, else the RTC, else 0 (unknown). Cheap; use for display.
time_t current();

// Before a fetch (WiFi up): resyncs from NTP at most every RESYNC_SECONDS,
// writing the result to the RTC and the system clock. Falls back to current().
bool ensure(time_t& outNow);

constexpr time_t MIN_VALID_EPOCH = 1700000000;  // Nov 2023; anything earlier means "never set"
constexpr time_t RESYNC_SECONDS = 6 * 3600;

}  // namespace network_time
