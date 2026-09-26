#include "WallpaperStore.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"

namespace wallpaper {
namespace {
constexpr size_t MAX_NAME = 128;
constexpr size_t MAX_FILES = 64;
}  // namespace

std::vector<std::string> list() {
  std::vector<std::string> names;
  auto dir = Storage.open(DIR);
  if (!dir || !dir.isDirectory()) return names;
  names.reserve(16);
  char name[MAX_NAME];
  for (auto file = dir.openNextFile(); file && names.size() < MAX_FILES; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '\0' || name[0] == '.' || !FsHelpers::hasBmpExtension(name)) continue;
    names.emplace_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

State loadState() {
  State state;
  if (!Storage.exists(STATE_PATH)) return state;
  const String raw = Storage.readFile(STATE_PATH);
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok) {
    LOG_ERR("WALL", "Ignoring unreadable %s", STATE_PATH);
    return state;
  }
  state.cycleIndex = doc["cycle"] | 0;
  state.fixedName = doc["fixed"] | "";
  return state;
}

bool saveState(const State& state) {
  JsonDocument doc;
  doc["cycle"] = state.cycleIndex;
  doc["fixed"] = state.fixedName;
  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(STATE_PATH, out)) {
    LOG_ERR("WALL", "Failed to write %s", STATE_PATH);
    return false;
  }
  return true;
}

std::string pathForNextSleep() {
  const uint8_t mode = SETTINGS.wallpaperMode;
  if (mode == CrossPointSettings::WALLPAPER_RANDOM) return {};
  const std::vector<std::string> names = list();
  if (names.empty()) return {};
  State state = loadState();

  if (mode == CrossPointSettings::WALLPAPER_FIXED) {
    // A deleted fixed file falls back to the first wallpaper rather than to Random.
    const bool present = std::find(names.begin(), names.end(), state.fixedName) != names.end();
    return std::string(DIR) + "/" + (present ? state.fixedName : names.front());
  }

  const int index = ((state.cycleIndex % static_cast<int>(names.size())) + static_cast<int>(names.size())) %
                    static_cast<int>(names.size());
  state.cycleIndex = index + 1;
  saveState(state);
  return std::string(DIR) + "/" + names[index];
}

}  // namespace wallpaper
