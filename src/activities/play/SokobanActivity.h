#pragma once

#include <cstdint>
#include <memory>

#include "SokobanBoard.h"
#include "activities/Activity.h"

// Sokoban: push every crate onto a goal. Left/Right and the side Up/Down
// buttons walk, Back undoes (hold Back exits), Confirm opens the menu.
//
// E-ink: the framebuffer keeps the last frame, and each render redraws only
// the tiles whose look changed since then, so presses made during a refresh
// are applied at once and the next frame shows the latest position. A HALF
// refresh starts every level and scrubs ghosting every CLEAN_EVERY_MOVES.
class SokobanActivity final : public Activity {
 public:
  explicit SokobanActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sokoban", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode : uint8_t { PLAY, MENU, SELECT, SOLVED };
  enum MenuItem : uint8_t { MENU_RESUME, MENU_RESTART, MENU_LEVELS, MENU_EXIT, MENU_COUNT };

  sokoban::LevelPack pack;
  std::unique_ptr<sokoban::Board> board;
  std::unique_ptr<uint16_t[]> best;  // fewest moves per level, 0 = unsolved
  std::unique_ptr<uint8_t[]> shown;  // Board::visual() of each tile as last drawn
  int level = 0;
  int savedLevel = -1;  // level recorded in the save file
  uint16_t parMoves = 0;
  uint16_t parPushes = 0;
  bool newBest = false;
  // Level-complete reveal: stars light up one per frame; any press skips.
  int starsShown = 0;
  unsigned long revealMs = 0;
  bool swallowRelease = false;
  bool progressDirty = false;

  // Layout for the current level.
  int tile = 0;
  int boardX = 0;
  int boardY = 0;

  // Guarded by the render lock.
  Mode mode = Mode::PLAY;
  int menuIndex = 0;
  int selectIndex = 0;
  bool fullRedraw = true;
  bool cleanRequested = true;
  int movesSinceClean = 0;

  void startLevel(int index);
  void layoutBoard();
  void step(sokoban::Dir dir);
  void undo();
  void completeLevel();
  int nextLevel() const;
  int solvedCount() const;
  int stars(int moves) const;
  void loadProgress();
  void saveProgress();

  void handlePlay();
  void handleMenu();
  void handleSelect();
  void handleSolved();

  void drawHeader() const;
  void drawBoard(bool all);
  void drawPlayHints() const;
  void drawMenu() const;
  void drawSelect() const;
  void drawSolvedCard() const;
  void drawCardFrame(int x, int y, int width, int height) const;
};
