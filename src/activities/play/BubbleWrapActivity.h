#pragma once

#include <cstdint>

#include "PlayCommon.h"
#include "activities/Activity.h"

// Bubble wrap: move the cursor with Left/Right (and the side buttons for rows),
// Confirm pops. Each pop flashes a burst frame, then settles flat. Holding
// Confirm lays a fresh sheet, which also arrives by itself once all are popped.
class BubbleWrapActivity final : public Activity {
 public:
  explicit BubbleWrapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BubbleWrap", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int MAX_BUBBLES = 128;

  play::FramePacer pacer;
  uint8_t popped[MAX_BUBBLES / 8] = {};
  int cols = 0;
  int rows = 0;
  int gridX = 0;
  int gridY = 0;
  int cursor = 0;
  int remaining = 0;
  int bursting = -1;  // bubble drawn as a burst this frame
  unsigned long burstMs = 0;
  unsigned long sheetDoneMs = 0;
  bool cleanRequested = false;

  bool isPopped(int index) const { return popped[index >> 3] & (1 << (index & 7)); }
  void newSheet();
  void pop();
  void moveCursor(int delta);
  void bubbleCenter(int index, int& x, int& y) const;
  void drawBubble(int index) const;
};
