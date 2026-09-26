#pragma once

#include <cstdint>

#include "PlayCommon.h"
#include "activities/Activity.h"

// Tilt Maze: every run carves a new perfect maze (recursive backtracker, so
// every cell is reachable) over the whole screen. The ball starts in the top
// row; tilt it into one of the two exits in the bottom row. Held direction
// buttons push the ball without an IMU (and in the simulator). Confirm starts a
// new maze after a win, Back exits.
//
// The maze is drawn once with a HALF refresh; each frame repaints only the
// ball's old and new rectangles (plus the walls under them) and FAST-refreshes.
class TiltMazeActivity final : public Activity {
 public:
  explicit TiltMazeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TiltMaze", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return tilt.available() && !won; }

 private:
  static constexpr int MAX_CELLS = 256;
  static constexpr int EXIT_COUNT = 2;

  play::Tilt tilt;
  play::FramePacer pacer;
  // Bit 2c: wall on the right of cell c, bit 2c+1: wall below it.
  uint8_t walls[MAX_CELLS / 4] = {};
  int cols = 0;
  int rows = 0;
  int mazeX = 0;
  int mazeY = 0;
  int exits[EXIT_COUNT] = {};
  uint32_t rng = 1;

  float ballX = 0, ballY = 0, velX = 0, velY = 0;
  unsigned long startMs = 0;
  unsigned long lastStepMs = 0;
  unsigned long solvedMs = 0;
  bool won = false;

  // Guarded by the render lock.
  bool fullRedraw = true;
  int drawnBallX = -1, drawnBallY = -1;
  int drawnSeconds = -1;

  uint32_t nextRandom();
  bool rightWall(int col, int row) const;
  bool bottomWall(int col, int row) const;
  void clearWall(int cell, int bit);
  void generate();
  void newMaze();
  bool inExit() const;
  void step(float dt, float pushX, float pushY);
  void collide();

  void drawHeader(bool force);
  void drawMazeArea(int x0, int y0, int x1, int y1) const;
  void drawBall(int x, int y) const;
  void drawWin() const;
};
