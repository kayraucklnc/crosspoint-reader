#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class Activity;
class GfxRenderer;
class MappedInputManager;

// Pickers and labels for the auto-refresh settings, shared by the Income and
// Trains pages and Settings. The pickers return the new value as an
// IntervalResult (a choice index, or an hour for the quiet-hour pickers).
// The factories return nullptr when out of memory.
namespace auto_refresh_ui {

// Interval picker for a CrossPointState::SleepApp.
std::unique_ptr<Activity> makeIntervalPicker(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t app);

// Hour picker for the quiet-hours start (end = false) or end.
std::unique_ptr<Activity> makeQuietHourPicker(GfxRenderer& renderer, MappedInputManager& mappedInput, bool end);

// Current interval of an app as text: "10 min" / "Off", or "10m" when compact.
void formatAppInterval(uint8_t app, char* out, size_t size, bool compact);

// Button hint for the page control: "Auto 10m".
void formatHint(uint8_t app, char* out, size_t size);

// "07:00"
void formatHour(int hour, char* out, size_t size);

}  // namespace auto_refresh_ui
