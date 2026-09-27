#pragma once

#include <cstddef>
#include <cstdint>

// Keeps an Income/Trains sleep snapshot fresh. Before a snapshot sleep the next
// deep-sleep timer wake is planned; that wake boots without UI (main.cpp),
// refreshes the page's data, redraws the snapshot and plans the following one.
// Power-button wakes are unchanged. The plan lives in APP_STATE because every
// wake is a chip reset.
namespace auto_refresh {

// Deep sleep keeps the RTC timer running. The X4 cuts battery power instead.
bool supported();

// Configured interval for a CrossPointState::SleepApp; 0 when off or unsupported.
uint16_t intervalMinutes(uint8_t app);

// This boot is a deep-sleep timer wake. Stable for the whole boot.
bool wokeFromTimer();

// Seconds left until the planned wake when the timer fired early, else 0.
long secondsEarly();

// Plans the next wake before a snapshot sleep (app = CrossPointState::SleepApp;
// NONE clears the plan). Runs before the snapshot is drawn, which reports it.
// A manual sleep starts a fresh chain; a timer-wake sleep keeps its failure count.
void planSleep(uint8_t app, bool timerRun);

// How a timer-wake refresh went: drives the stale badge and the failure backoff.
void recordOutcome(bool succeeded);

// Arms the planned wake; call right before entering deep sleep.
void armPlannedWake();

// Re-arms what is left of the planned wake for a boot that goes straight back
// to sleep (ghost power wake, USB wake, early timer).
void armRemainingWake();

// Battery charge used for the low-battery pause, and whether it applies now.
int batteryPercent();
bool batteryTooLow();

// Badge details for the snapshot being drawn.
struct SnapshotNote {
  char next[8] = {};  // local "HH:MM" of the next wake; empty when none
  bool stale = false;
  bool lowBattery = false;
};
SnapshotNote snapshotNote();

// "3 h" or "Off" (compact: "3h"), with translated units.
void formatInterval(uint16_t minutes, char* out, size_t size, bool compact);

#ifdef SIMULATOR
// The simulator has no sleep timer: with CROSSPOINT_SIM_TIMER_WAKE_MS set, the
// asleep panel keeps being presented for that many ms (for screenshots), then
// an armed wake relaunches the process as a timer wake with the simulated RTC
// advanced to the planned time. CROSSPOINT_SIM_TIMER_WAKES caps the chain
// (default 1).
void simulateTimerWake();
#endif

}  // namespace auto_refresh
