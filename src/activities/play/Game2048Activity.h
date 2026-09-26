#pragma once

#include <cstdint>

#include "Game2048Board.h"
#include "activities/Activity.h"

// 2048 for e-ink. Front Left/Right and the side Up/Down buttons slide the tiles,
// Confirm undoes the last move, Back opens the menu (resume, new game, exit).
// Presses that land while the panel refreshes are all applied and only the final
// board is drawn; each frame repaints just the cells that changed. The game
// persists to /.crosspoint/games/2048.json and resumes after sleep or reboot.
class Game2048Activity final : public Activity {
 public:
  explicit Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("2048", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode : uint8_t { PLAY, MENU, WON, OVER };
  static constexpr int MENU_ITEMS = 3;

  g2048::Board board;
  g2048::Board undoBoard;
  uint32_t score = 0;
  uint32_t best = 0;
  uint32_t moves = 0;
  uint32_t undoScore = 0;
  uint32_t undoMoves = 0;
  uint32_t lastGain = 0;
  uint32_t rng = 1;
  int8_t newCell = -1;
  uint16_t mergedCells = 0;  // cells the last move merged into
  bool hasUndo = false;
  bool won = false;  // the 2048 card has been shown for this game
  Mode mode = Mode::PLAY;
  Mode menuReturn = Mode::PLAY;
  uint8_t menuIndex = 0;
  bool saveDirty = false;
  unsigned long lastChangeMs = 0;

  // Render bookkeeping, guarded by the render lock.
  uint8_t shownCells[g2048::CELLS] = {};
  int8_t shownNew = -1;
  uint16_t shownMerged = 0;
  bool gainShown = false;
  uint8_t shownMax = 0;
  bool fullRedraw = true;
  bool cleanRequested = true;
  int movesSinceClean = 0;

  // Layout, derived from the screen in onEnter().
  int boardX = 0;
  int boardY = 0;
  int boardSize = 0;
  int tile = 0;

  void newGame();
  void move(g2048::Dir dir);
  void undo();
  void setMode(Mode next);
  void openMenu();
  void loopMenu();
  void markChanged();
  void load();
  void save();

  int cellX(int cell) const;
  int cellY(int cell) const;
  void drawHeader() const;
  void drawLadder(uint8_t top) const;
  void drawBoardFrame() const;
  void drawCell(int cell, bool isNew, bool isMerged) const;
  void drawHints() const;
  void drawCard() const;
};
