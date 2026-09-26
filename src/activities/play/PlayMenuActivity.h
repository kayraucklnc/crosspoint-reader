#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Play: small e-ink games. Up/Down or Left/Right pick one, Confirm opens
// it, Back returns Home. Each toy runs on top of this menu and finishes back to it.
class PlayMenuActivity final : public Activity {
 public:
  explicit PlayMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Play", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ITEM_COUNT = 7;

  ButtonNavigator buttonNavigator;
  int selected = 0;
  bool firstRenderDone = false;

  void open();
  void renderItem(int index, int x, int y, int width, int height) const;
  void drawGlyph(int index, int x, int y, int size) const;
};
