#pragma once

#include <memory>

#include "PlayCommon.h"
#include "activities/Activity.h"

// Snake on a coarse grid, paced for e-ink. Left/Right turn the snake relative
// to its heading; with an IMU, the Down side button toggles tilt steering.
// Confirm pauses (or restarts after a crash), Back exits.
class SnakeActivity final : public Activity {
 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Snake", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State { RUNNING, PAUSED, OVER };

  play::Tilt tilt;
  play::FramePacer pacer;
  std::unique_ptr<uint16_t[]> body;  // ring buffer of cell indices, head at `head`
  int cols = 0;
  int rows = 0;
  int boardX = 0;
  int boardY = 0;
  int capacity = 0;
  int head = 0;
  int length = 0;
  int dir = 0;         // 0 right, 1 down, 2 left, 3 up
  int pendingDir = 0;  // applied on the next step so two quick turns can't reverse
  int food = 0;
  int score = 0;
  bool tiltSteer = false;
  State state = State::RUNNING;
  unsigned long lastStepMs = 0;
  uint32_t rng = 0;
  bool cleanRequested = false;

  void reset();
  void placeFood();
  bool occupied(int cell) const;
  void advance();
  int stepInterval() const;
  void steerByTilt();
  void drawCell(int cell, bool isHead) const;
};
