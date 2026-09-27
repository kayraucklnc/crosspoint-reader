#include "NetworkTime.h"

#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "HttpDownloader.h"

namespace network_time {
namespace {
bool systemClockTrusted = false;  // set from NTP or the RTC during this boot
bool synced = false;              // NTP or HTTP Date succeeded this boot
}  // namespace

// Last successful network sync. Kept in RTC memory through deep sleep, so the
// auto-refresh timer wakes only resync every RESYNC_SECONDS (the RTC holds the
// time in between) instead of paying for NTP on every wake.
#ifdef SIMULATOR
static time_t lastSync = 0;
#else
static RTC_DATA_ATTR time_t lastSync = 0;
#endif

namespace {

time_t systemTime() {
#ifdef SIMULATOR
  // Until NTP runs, the system clock is whatever the (possibly wrong) RTC said.
  if (!synced) {
    if (const char* sim = getenv("CROSSPOINT_SIM_CLOCK")) {
      if (strcmp(sim, "unset") != 0) return time(nullptr) + atol(sim);
    }
  }
#endif
  return time(nullptr);
}

void setSystemTime(const time_t utc) {
#ifndef SIMULATOR
  const timeval tv{utc, 0};
  settimeofday(&tv, nullptr);
#endif
  systemClockTrusted = true;
}

// SNTP into the system clock, keeping the display TZ.
bool syncSystemClockFromNtp() {
#ifdef SIMULATOR
  return true;  // the host clock is already NTP-disciplined
#else
  const char* tzBefore = getenv("TZ");
  char savedTz[64] = {0};
  if (tzBefore) snprintf(savedTz, sizeof(savedTz), "%s", tzBefore);
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  bool ok = false;
  for (int i = 0; i < 50 && !ok; i++) {
    ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
    if (!ok) delay(100);
  }
  setenv("TZ", savedTz[0] != '\0' ? savedTz : "UTC0", 1);
  tzset();
  return ok;
#endif
}

// "Sat, 26 Sep 2026 12:34:56 GMT" -> epoch, or 0.
time_t parseHttpDate(const char* s) {
  static constexpr const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {};
  int day = 0, year = 0, h = 0, m = 0, sec = 0;
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &h, &m, &sec) != 6) return 0;
  const char* found = strstr(MONTHS, mon);
  if (!found || mon[0] == '\0') return 0;
  const int month = static_cast<int>(found - MONTHS) / 3 + 1;
  int y = year - (month <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
  return static_cast<time_t>(days) * 86400 + h * 3600 + m * 60 + sec;
}

// NTP (UDP 123) is blocked on some hotspots and captive networks; any HTTPS
// server's Date header is accurate to a second or two, which is plenty here.
bool syncFromHttpDate() {
  std::string date;
  if (!HttpDownloader::fetchDateHeader("https://www.google.com/generate_204", date)) return false;
  const time_t t = parseHttpDate(date.c_str());
  if (t < MIN_VALID_EPOCH) return false;
  setSystemTime(t);
  synced = true;
  LOG_INF("TIME", "Clock set from HTTP Date header");
  return true;
}
}  // namespace

time_t current() {
  if (systemClockTrusted) return systemTime();
  time_t rtc = 0;
#ifdef SIMULATOR
  // The simulator's HAL has no RTC read-back: the RTC reads host time, shifted
  // by CROSSPOINT_SIM_CLOCK seconds, or is missing when it is "unset".
  const char* sim = getenv("CROSSPOINT_SIM_CLOCK");
  const bool haveRtc = !(sim && strcmp(sim, "unset") == 0);
  rtc = time(nullptr) + (sim && haveRtc ? atol(sim) : 0);
#else
  const bool haveRtc = halClock.utcNow(rtc);
#endif
  if (haveRtc && rtc >= MIN_VALID_EPOCH) {
    setSystemTime(rtc);
    return rtc;
  }
  return 0;
}

bool ensure(time_t& outNow) {
  const time_t known = current();
  if (WiFi.status() == WL_CONNECTED && (known == 0 || lastSync == 0 || known - lastSync >= RESYNC_SECONDS)) {
    // HalClock::syncFromNTP also sets the system clock and writes UTC to the RTC.
    const bool ok = halClock.isAvailable() ? halClock.syncFromNTP() : syncSystemClockFromNtp();
    if (ok && time(nullptr) >= MIN_VALID_EPOCH) {
      systemClockTrusted = synced = true;
      lastSync = systemTime();
      if (known != 0 && std::llabs(static_cast<long long>(lastSync - known)) > 120) {
        LOG_INF("TIME", "Clock corrected by %lld s", static_cast<long long>(lastSync - known));
      }
    } else if (syncFromHttpDate()) {
      lastSync = systemTime();
    } else {
      LOG_ERR("TIME", "NTP and HTTP Date sync failed");
    }
  }
  outNow = current();
  return outNow >= MIN_VALID_EPOCH;
}

}  // namespace network_time
