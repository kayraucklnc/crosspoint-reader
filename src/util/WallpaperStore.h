#pragma once

#include <string>
#include <vector>

// Wallpapers are the BMP files in /sleep. The Wallpaper setting picks one per
// sleep: Random (default), Cycle (next file each time) or Fixed (one chosen
// file). State lives in /.crosspoint/wallpaper.json. A root /sleep.bmp still
// overrides every mode, and /.sleep is still used by Random when present.
namespace wallpaper {

constexpr const char* DIR = "/sleep";
constexpr const char* STATE_PATH = "/.crosspoint/wallpaper.json";

// Sorted names of the BMP files in /sleep (names only, no directory).
std::vector<std::string> list();

struct State {
  int cycleIndex = 0;
  std::string fixedName;
};
State loadState();
bool saveState(const State& state);

// Full path for the Cycle/Fixed modes, advancing the cycle; empty for Random
// or when /sleep has no usable file.
std::string pathForNextSleep();

}  // namespace wallpaper
