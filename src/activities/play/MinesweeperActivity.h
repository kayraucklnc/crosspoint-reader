#pragma once

#include <cstdint>

#include "MinesweeperBoard.h"
#include "activities/Activity.h"

// Minesweeper tuned for ~480 ms e-ink refreshes and six buttons.
// Left/Right and the side buttons move the cursor (a tap wraps at the edge;
// holding glides over opened blank cells and stops at the edge). Confirm opens
// a tile, or on a number opens/flags its neighbours when that is certain.
// Holding Confirm toggles a flag. Back opens the menu (resume, new game per
// difficulty, exit). Input keeps flowing while the panel refreshes, so quick
// taps coalesce into one redraw of only the changed tiles.
class MinesweeperActivity final : public Activity {
 public:
  explicit MinesweeperActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Minesweeper", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

 private:
  static constexpr int DIFFICULTY_COUNT = 3;
  static constexpr int MENU_ITEMS = DIFFICULTY_COUNT + 2;  // Resume, difficulties, Exit
  enum class Overlay : uint8_t { NONE, MENU, RESULT };

  mines::Board board;
  int difficulty = 0;
  uint16_t bestSeconds[DIFFICULTY_COUNT] = {};
  bool saveNeeded = false;
  bool newBest = false;
  uint32_t rng = 1;
  uint32_t fixedSeed = 0;  // simulator QA hook: same board for the same first move

  int cursor = 0;
  int heldDirection = -1;
  unsigned long nextRepeatMs = 0;

  unsigned long timerStartMs = 0;
  uint32_t timerBankedMs = 0;
  bool timerRunning = false;

  Overlay overlay = Overlay::NONE;
  int menuIndex = 0;
  // Step frames after a game ends (loss: tripped mine, all mines, card; win:
  // flagged board, card), one per panel refresh. 0 when idle; any key skips.
  uint8_t finaleStage = 0;
  unsigned long finaleStepMs = 0;

  // Layout, fixed per game.
  int tile = 0;
  int boardX = 0;
  int boardY = 0;

  // Guarded by the render lock.
  bool fullRedraw = true;
  bool cleanRefresh = true;
  int framesSinceClean = 0;
  int drawnCursor = -1;

  void newGame(int level);
  void layout();
  void load();
  void save();

  void handleBoardInput();
  void handleMenuInput(RenderLock& lock);
  void moveCursor(int dx, int dy, bool glide);
  void press();
  void toggleFlag();
  void afterMove(mines::Outcome outcome, bool wasReady);
  void openMenu();
  void closeOverlay();
  void handleFinale();
  void advanceFinale(bool skip);

  void startTimer();
  void stopTimer();
  uint32_t elapsedSeconds() const;

  void drawCell(int cell) const;
  void drawFrame() const;
  void drawHud() const;
  void drawHints() const;
  void drawMenu() const;
  void drawResult() const;
};
