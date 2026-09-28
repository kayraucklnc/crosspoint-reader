#include "AutoRefresh.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "AutoRefreshSchedule.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "network/NetworkTime.h"

#ifdef SIMULATOR
#include <HalDisplay.h>
#include <SimulatorLifecycle.h>

#include <cstdlib>
#include <cstring>
#else
#include <esp_sleep.h>
#include <esp_system.h>
#endif

namespace auto_refresh {
namespace {

long plannedSeconds = 0;  // timer to arm at sleep entry; 0 = none

#ifdef SIMULATOR
long simArmedSeconds = 0;
#endif

void armTimer(const long seconds) {
  if (seconds <= 0) return;
  LOG_INF("AUTO", "Timer wake armed in %ld s", seconds);
#ifdef SIMULATOR
  simArmedSeconds = seconds;
#else
  // Adds the RTC timer as a second wake source next to the power-button GPIO
  // that HalPowerManager::startDeepSleep() arms; whichever fires first wakes.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  powerManager.keepBatteryOnForTimerWake(true);
#endif
}

bool readTimerWake() {
#ifdef SIMULATOR
  const char* flag = getenv("CROSSPOINT_SIM_TIMER_WAKE");
  const bool timer = flag && strcmp(flag, "1") == 0;
  unsetenv("CROSSPOINT_SIM_TIMER_WAKE");
  return timer;
#else
  return esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
#endif
}

long plannedIntervalSeconds() {
  return effectiveIntervalSeconds(intervalMinutes(APP_STATE.lastSleepApp), APP_STATE.autoRefreshFailures);
}

}  // namespace

bool supported() { return !gpio.deviceIsX4(); }

uint16_t intervalMinutes(const uint8_t app) {
  if (!supported()) return 0;
  switch (app) {
    case CrossPointState::SLEEP_APP_INCOME:
      return incomeMinutes(SETTINGS.incomeAutoRefresh);
    case CrossPointState::SLEEP_APP_TRAINS:
      return trainsMinutes(SETTINGS.trainsAutoRefresh);
    default:
      return 0;
  }
}

bool wokeFromTimer() {
  static const bool timer = readTimerWake();
  return timer;
}

long secondsEarly() {
  if (APP_STATE.autoRefreshWakeAt == 0) return 0;
  const time_t now = network_time::current();
  if (now == 0) return 0;
  const long remaining = static_cast<long>(static_cast<time_t>(APP_STATE.autoRefreshWakeAt) - now);
  return remaining > EARLY_WAKE_SLACK_SECONDS ? remaining : 0;
}

int batteryPercent() {
#ifdef SIMULATOR
  if (const char* sim = getenv("CROSSPOINT_SIM_BATTERY")) return atoi(sim);
#endif
  return powerManager.getBatteryPercentage();
}

bool batteryTooLow() {
#ifdef SIMULATOR
  // The simulator always reports USB power; a battery override means "on battery".
  if (getenv("CROSSPOINT_SIM_BATTERY")) return batteryPercent() < LOW_BATTERY_PERCENT;
#endif
  return batteryPercent() < LOW_BATTERY_PERCENT && !gpio.isUsbConnected();
}

void planSleep(const uint8_t app, const bool timerRun) {
  if (!timerRun) {
    APP_STATE.autoRefreshFailures = 0;
    APP_STATE.autoRefreshStale = false;
  }
  APP_STATE.autoRefreshArmed = false;
  APP_STATE.autoRefreshWakeAt = 0;
  APP_STATE.autoRefreshLowBattery = false;
  plannedSeconds = 0;

  const uint16_t minutes = intervalMinutes(app);
  if (minutes == 0) return;
  if (batteryTooLow()) {
    APP_STATE.autoRefreshLowBattery = true;
    LOG_INF("AUTO", "Battery %d%%: auto-refresh paused", batteryPercent());
    return;
  }

  const long interval = effectiveIntervalSeconds(minutes, APP_STATE.autoRefreshFailures);
  const time_t now = network_time::current();
  if (now == 0) {
    // No trusted clock: sleep one interval; the wake syncs the clock before it fetches.
    plannedSeconds = interval;
  } else {
    const time_t wake = nextWake(now, interval, SETTINGS.autoRefreshQuietStart, SETTINGS.autoRefreshQuietEnd);
    APP_STATE.autoRefreshWakeAt = static_cast<uint32_t>(wake);
    plannedSeconds = sleepSecondsUntil(now, wake);
  }
  APP_STATE.autoRefreshArmed = true;
}

void recordOutcome(const bool succeeded) {
  APP_STATE.autoRefreshStale = !succeeded;
  APP_STATE.autoRefreshFailures =
      succeeded ? 0 : static_cast<uint8_t>(std::min(255, APP_STATE.autoRefreshFailures + 1));
  LOG_INF("AUTO", "Refresh %s (failures in a row: %u)", succeeded ? "succeeded" : "failed",
          APP_STATE.autoRefreshFailures);
}

void armPlannedWake() { armTimer(plannedSeconds); }

void armRemainingWake() {
  if (!APP_STATE.autoRefreshArmed) return;
  const time_t now = network_time::current();
  if (APP_STATE.autoRefreshWakeAt != 0 && now != 0) {
    armTimer(sleepSecondsUntil(now, static_cast<time_t>(APP_STATE.autoRefreshWakeAt)));
  } else {
    armTimer(plannedIntervalSeconds());
  }
}

SnapshotNote snapshotNote() {
  SnapshotNote note;
  note.stale = APP_STATE.autoRefreshStale;
  note.lowBattery = APP_STATE.autoRefreshLowBattery;
  if (APP_STATE.autoRefreshArmed && APP_STATE.autoRefreshWakeAt != 0) {
    const time_t wake = static_cast<time_t>(APP_STATE.autoRefreshWakeAt);
    struct tm local;
    localtime_r(&wake, &local);
    snprintf(note.next, sizeof(note.next), "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return note;
}

void formatInterval(const uint16_t minutes, char* out, const size_t size, const bool compact) {
  if (minutes == 0) {
    snprintf(out, size, "%s", tr(STR_STATE_OFF));
    return;
  }
  const DurationFormats formats =
      compact ? DurationFormats{tr(STR_AR_MINUTES_SHORT_FMT), tr(STR_AR_HOURS_SHORT_FMT),
                                tr(STR_AR_HOURS_MINUTES_SHORT_FMT)}
              : DurationFormats{tr(STR_AR_MINUTES_FMT), tr(STR_AR_HOURS_FMT), tr(STR_AR_HOURS_MINUTES_FMT)};
  formatMinutes(minutes, formats, out, size);
}

#ifdef SIMULATOR
void simulateTimerWake() {
  const char* delayMs = getenv("CROSSPOINT_SIM_TIMER_WAKE_MS");
  if (!delayMs) return;
  // Keep presenting for the "sleep" so scheduled screenshots of the panel land,
  // armed or not.
  const unsigned long start = millis();
  const unsigned long wait = strtoul(delayMs, nullptr, 10);
  while (millis() - start < wait) {
    display.presentIfNeeded();
    delay(20);
  }
  const char* budget = getenv("CROSSPOINT_SIM_TIMER_WAKES");
  const int remaining = budget ? atoi(budget) : 1;
  if (simArmedSeconds <= 0 || remaining <= 0) return;

  char value[24];
  snprintf(value, sizeof(value), "%d", remaining - 1);
  setenv("CROSSPOINT_SIM_TIMER_WAKES", value, 1);
  const char* clock = getenv("CROSSPOINT_SIM_CLOCK");
  const long offset = clock && strcmp(clock, "unset") != 0 ? atol(clock) : 0;
  snprintf(value, sizeof(value), "%ld", offset + simArmedSeconds);
  setenv("CROSSPOINT_SIM_CLOCK", value, 1);
  setenv("CROSSPOINT_SIM_TIMER_WAKE", "1", 1);
  LOG_INF("AUTO", "Simulated timer wake after %ld s", simArmedSeconds);
  SimulatorLifecycle::rebootAsPowerWake();
}
#endif

}  // namespace auto_refresh
