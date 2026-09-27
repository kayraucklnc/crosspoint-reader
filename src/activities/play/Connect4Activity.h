#pragma once

#include <atomic>
#include <cstdint>

#include "Connect4Board.h"
#include "Connect4Brain.h"
#include "Connect4Worker.h"
#include "activities/Activity.h"

// Connect Four against the computer, tuned for ~480 ms e-ink refreshes.
// Left/Right move the drop column (wrapping, skipping full columns) and
// Confirm drops; side Up takes back your last move and the computer's reply,
// side Down asks the computer for a hint. Back opens the menu (new game,
// difficulty, who starts, exit). The computer searches on a background task,
// so buttons keep working while it thinks; its disc hovers over the chosen
// column for one frame before it lands (any key lands it at once).
// Settings, per-difficulty results and the game in progress persist to
// /.crosspoint/games/connect4.json.
class Connect4Activity final : public Activity {
 public:
  explicit Connect4Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Connect4", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return worker.busy(); }

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

 private:
  enum class Phase : uint8_t { HUMAN, THINKING, AI_HOVER, OVER };
  enum class Job : uint8_t { NONE, MOVE, HINT };
  enum Starter : uint8_t { START_HUMAN, START_COMPUTER, START_ALTERNATE, STARTER_COUNT };
  enum MenuItem : uint8_t { ITEM_NEW, ITEM_LEVEL, ITEM_STARTER, ITEM_EXIT, MENU_ITEMS };
  enum Result : uint8_t { WON, LOST, DRAWN, RESULT_KINDS };

  c4::Board board;
  Connect4Worker worker;
  Phase phase = Phase::HUMAN;
  int cursor = 3;
  int hintColumn = -1;
  int aiColumn = -1;

  // Settings (menu) and the level this game was started at.
  int level = 1;
  int gameLevel = 1;
  uint8_t starter = START_HUMAN;
  bool computerOpensNext = false;
  uint16_t stats[c4::LEVEL_COUNT][RESULT_KINDS] = {};
  bool resultRecorded = false;
  bool saveNeeded = false;
  uint32_t rng = 1;

  Job pendingJob = Job::NONE;
  Job runningJob = Job::NONE;
  uint32_t ticket = 0;

  bool menuOpen = false;
  int menuIndex = 0;
  bool settingsChanged = false;

  // The hover frame's id, and the id/time the render task last put on the panel.
  uint32_t hoverId = 0;
  unsigned long hoverStartMs = 0;
  std::atomic<uint32_t> presentedHover{0};
  std::atomic<uint32_t> presentedAtMs{0};

  // Layout, fixed in onEnter().
  int cell = 0;
  int boardX = 0;
  int boardY = 0;
  int laneY = 0;
  int headerY = 0;
  int headerH = 0;

  // Guarded by the render lock.
  bool cleanRefresh = true;
  int framesSinceClean = 0;

  void layout();
  void load();
  void save();

  void startGame();
  void restoreGame(const char* moves, c4::Side first);
  void afterMove();
  void recordResult();
  void queueJob(Job job);
  void cancelJob();
  void pumpWorker();
  void onSearchDone(int column);
  void landComputerDisc();

  void handleMenu(RenderLock& lock);
  void handleBoard();
  void moveCursor(int direction);
  void dropHuman();
  void undo();
  void requestHint();
  void openMenu();
  void closeMenu();
  int firstPlayableFrom(int col, int direction) const;
  int humanMoves() const;

  // Drawing (Connect4Render.cpp).
  int columnX(int col) const;
  int rowY(int row) const;
  void drawHeader() const;
  void drawLane() const;
  void drawBoard() const;
  void drawWinLine() const;
  void drawHints() const;
  void drawMenu() const;
};
