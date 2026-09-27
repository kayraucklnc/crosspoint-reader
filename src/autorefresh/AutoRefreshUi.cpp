#include "AutoRefreshUi.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "AutoRefresh.h"
#include "AutoRefreshSchedule.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/util/IntervalSelectionActivity.h"

namespace auto_refresh_ui {
namespace {

constexpr int TRAINS_LARGE_STEP = 3;
constexpr int INCOME_LARGE_STEP = 4;  // 2 h
constexpr int HOUR_LARGE_STEP = 6;

void formatTrainsChoice(const int choice, char* out, const size_t size) {
  auto_refresh::formatInterval(auto_refresh::trainsMinutes(static_cast<uint8_t>(choice)), out, size, false);
}

void formatIncomeChoice(const int choice, char* out, const size_t size) {
  auto_refresh::formatInterval(auto_refresh::incomeMinutes(static_cast<uint8_t>(choice)), out, size, false);
}

void formatHourValue(const int hour, char* out, const size_t size) { formatHour(hour, out, size); }

}  // namespace

void formatHour(const int hour, char* out, const size_t size) { snprintf(out, size, "%02d:00", hour); }

std::unique_ptr<Activity> makeIntervalPicker(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const uint8_t app) {
  const bool trains = app == CrossPointState::SLEEP_APP_TRAINS;
  const int current = trains ? SETTINGS.trainsAutoRefresh : SETTINGS.incomeAutoRefresh;
  const int maxChoice = trains ? auto_refresh::TRAINS_CHOICE_COUNT - 1 : auto_refresh::INCOME_CHOICE_COUNT - 1;
  auto picker = makeUniqueNoThrow<IntervalSelectionActivity>(
      renderer, mappedInput, "AutoRefreshInterval", trains ? StrId::STR_AR_TRAINS : StrId::STR_AR_INCOME, current, 0,
      maxChoice, 1, trains ? TRAINS_LARGE_STEP : INCOME_LARGE_STEP);
  if (!picker) {
    LOG_ERR("AUTO", "OOM: interval picker");
    return nullptr;
  }

  char quiet[64] = {};
  if (SETTINGS.autoRefreshQuietStart != SETTINGS.autoRefreshQuietEnd) {
    char from[8];
    char to[8];
    formatHour(SETTINGS.autoRefreshQuietStart, from, sizeof(from));
    formatHour(SETTINGS.autoRefreshQuietEnd, to, sizeof(to));
    snprintf(quiet, sizeof(quiet), tr(STR_AR_QUIET_NOTE), from, to);
  }
  picker->setFormatter(trains ? &formatTrainsChoice : &formatIncomeChoice, tr(STR_AR_PICKER_NOTE), quiet);
  return picker;
}

std::unique_ptr<Activity> makeQuietHourPicker(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool end) {
  const int current = end ? SETTINGS.autoRefreshQuietEnd : SETTINGS.autoRefreshQuietStart;
  auto picker = makeUniqueNoThrow<IntervalSelectionActivity>(renderer, mappedInput, "AutoRefreshQuietHour",
                                                             end ? StrId::STR_AR_QUIET_END : StrId::STR_AR_QUIET_START,
                                                             current, 0, 23, 1, HOUR_LARGE_STEP);
  if (!picker) {
    LOG_ERR("AUTO", "OOM: quiet hour picker");
    return nullptr;
  }
  picker->setFormatter(&formatHourValue, nullptr, nullptr);
  return picker;
}

void formatAppInterval(const uint8_t app, char* out, const size_t size, const bool compact) {
  const uint16_t minutes = app == CrossPointState::SLEEP_APP_TRAINS
                               ? auto_refresh::trainsMinutes(SETTINGS.trainsAutoRefresh)
                               : auto_refresh::incomeMinutes(SETTINGS.incomeAutoRefresh);
  auto_refresh::formatInterval(minutes, out, size, compact);
}

void formatHint(const uint8_t app, char* out, const size_t size) {
  char interval[16];
  formatAppInterval(app, interval, sizeof(interval), true);
  snprintf(out, size, tr(STR_AR_HINT_FMT), interval);
}

}  // namespace auto_refresh_ui
