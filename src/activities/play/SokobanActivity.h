#pragma once

#include <cstdint>
#include <memory>

#include "SokobanBoard.h"
#include "SokobanHint.h"
#include "activities/Activity.h"

// Sokoban: push every crate onto a goal. Left/Right and the side Up/Down
// buttons walk, Back undoes (hold Back exits), Confirm opens the menu and
// holding Confirm shows a hint: the next push of the stored solution.
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
  // OFF_PATH asks to undo back onto the solution; SOLUTION steps through it.
  enum class Mode : uint8_t { PLAY, MENU, SELECT, SOLVED, OFF_PATH, SOLUTION };
  enum MenuItem : uint8_t { MENU_RESUME, MENU_HINT, MENU_SOLUTION, MENU_RESTART, MENU_LEVELS, MENU_EXIT, MENU_COUNT };

  sokoban::LevelPack pack;
  std::unique_ptr<sokoban::Board> board;
  std::unique_ptr<uint16_t[]> best;   // fewest moves per level, 0 = unsolved
  std::unique_ptr<uint8_t[]> shown;   // tileLook() of each tile as last drawn
  std::unique_ptr<uint8_t[]> helped;  // bit per level: solved only with hints
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
  bool transposed = false;  // board shown with rows and columns swapped

  // Hints: built on first use per level (~8 bytes per solution push).
  sokoban::SolutionGuide guide;
  bool guideTried = false;
  sokoban::Hint hint;
  uint8_t route[sokoban::MAX_CELLS / 8] = {};
  bool hintShown = false;
  bool solutionAfterRewind = false;
  int hintsUsed = 0;  // this attempt
  bool stuck = false;

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
  bool isHelped(int index) const { return helped[index / 8] & (1 << (index % 8)); }
  void setHelped(int index, bool on);

  // Hints (SokobanActivityHint.cpp).
  bool ensureGuide();
  bool locateHint();
  void requestHint(bool stepThrough);
  void rewindToPath();
  void solutionStep(int direction);
  void clearHint();
  uint8_t tileLook(int cell) const;
  void handleOffPath();
  void handleSolution();
  void drawOffPathCard() const;

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
