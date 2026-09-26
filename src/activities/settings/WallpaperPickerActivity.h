#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Browses the wallpapers in /sleep with a live preview. Left/Right step through
// the files, Confirm sets the highlighted one as the Fixed wallpaper, Back exits.
class WallpaperPickerActivity final : public Activity {
 public:
  explicit WallpaperPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WallpaperPicker", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<std::string> names;
  std::string fixedName;
  int index = 0;
  bool firstRenderDone = false;

  void choose();
};
