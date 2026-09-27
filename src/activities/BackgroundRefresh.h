#pragma once

#include <cstdint>

// A dashboard started by a deep-sleep timer wake (see src/autorefresh) runs
// without drawing: Refresh fetches its data, RenderOnly just loads the cache.
// Either way the result is drawn as the next sleep snapshot.
enum class BackgroundMode : uint8_t { Off, Refresh, RenderOnly };

// Progress of that background run, polled by main.cpp. None when the activity
// is not running in the background (never started that way, or handed over to
// the user).
enum class BackgroundRefresh : uint8_t { None, Pending, Succeeded, Failed };

// Route of a Trains snapshot for CrossPointState::lastSleepRoute; this value
// means the default board.
inline constexpr uint16_t DEFAULT_SLEEP_ROUTE = 0xFFFF;
