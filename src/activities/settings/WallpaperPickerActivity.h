#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/WallpaperThumb.h"

// Lists the wallpapers in /sleep. Side buttons move, Confirm sets the focused
// one as the Fixed wallpaper, Left toggles a split view with a dithered
// thumbnail of the focused file, Right cycles the wallpaper mode, Back exits.
class WallpaperPickerActivity final : public Activity {
 public:
  explicit WallpaperPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WallpaperPicker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Layout {
    int top;
    int rowHeight;
    int rows;
    int listX;
    int listWidth;
    int thumbX;
    int thumbWidth;
    int thumbHeight;
  };

  std::vector<std::string> names;
  std::string fixedName;
  ButtonNavigator nav{150, 400};
  wallpaper::ThumbLoader thumbs;
  // Written by loop() and read by render() without the render lock, so a
  // press is never blocked behind a thumbnail decode and panel refresh.
  std::atomic<int> index{0};
  std::atomic<bool> preview{false};
  bool firstRenderDone = false;
  // State of the last frame: a focus move only repaints the list and preview.
  bool shownPreview = false;
  uint8_t shownMode = 0xFF;
  // Index whose thumbnail the loader holds; thumbOk is false when it failed to load.
  int thumbIndex = -1;
  bool thumbOk = false;

  Layout layout(bool withPreview) const;
  void move(int delta);
  void choose();
  void cycleMode();
  void drawHeaderAndHints(bool withPreview) const;
  void drawList(const Layout& lay, int focus) const;
  void drawPreview(const Layout& lay, int focus);
};
