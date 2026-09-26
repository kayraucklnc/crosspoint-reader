#pragma once

#include <memory>

#include "PlayCommon.h"
#include "activities/Activity.h"

// Zen Garden: tilt the reader to roll a ball through sand; it leaves a trail in
// the current brush (stippled sand, a rake that follows the ball, or smoothing).
// Without an IMU (and in the simulator) the four direction buttons push the ball.
// Confirm cycles the brush, holding Confirm clears the garden, Back exits.
// With tilt, side buttons also step the brush and Left forces a clean refresh.
//
// Each frame redraws only the rectangle the ball and its trail touched into the
// persistent framebuffer, then a FAST refresh; the panel's differential waveform
// drives just the pixels that changed.
class ZenGardenActivity final : public Activity {
 public:
  explicit ZenGardenActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ZenGarden", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return tilt.available(); }

 private:
  enum Brush : uint8_t { SAND, RAKE, SMOOTH, BRUSH_COUNT };
  static constexpr int CELL = 4;  // sand resolution in pixels

  play::Tilt tilt;
  play::FramePacer pacer;
  std::unique_ptr<uint8_t[]> sand;  // 2 bits per cell: 0 smooth, 1 grain, 2/3 raked across/along
  int cols = 0;
  int rows = 0;
  int fieldTop = 0;
  int fieldBottom = 0;
  int fieldWidth = 0;

  float ballX = 0, ballY = 0, velX = 0, velY = 0;
  Brush brush = RAKE;
  unsigned long lastStepMs = 0;

  // Guarded by the render lock: what the next frame must repaint.
  bool fullRedraw = true;
  bool cleanRequested = false;
  bool hasDirty = false;
  int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
  int drawnBallX = -1, drawnBallY = -1;

  uint8_t cellAt(int col, int row) const;
  void setCell(int col, int row, uint8_t value);
  void markDirty(int x0, int y0, int x1, int y1);
  void stamp();
  void step(float dt, float pushX, float pushY);
  void clearGarden();

  void drawHeader() const;
  void drawSand(int x0, int y0, int x1, int y1) const;
  void drawBall(int x, int y) const;
};
