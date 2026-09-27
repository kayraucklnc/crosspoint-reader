#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

#include "ConnectionsCore.h"
#include "ConnectionsPack.h"
#include "ConnectionsResults.h"
#include "activities/Activity.h"

// Connections: sixteen words, four hidden groups of four, four mistakes.
// Puzzles come from a pack on the SD card (/.crosspoint/games/connections),
// seeded by scripts/gen_connections.py and refreshed on the device from the
// public archive mirror (Update puzzles). Rules, pack and importer are ported
// from CrossPlay; the screens are rebuilt for the six buttons.
//
// Home: Left/Right or side Up/Down pick an entry, Confirm opens, Back exits.
// Board: Left/Right move along a row, side Up/Down change row (the row under
// the grid holds Shuffle, Deselect and Submit), Confirm selects a word. The
// fourth pick jumps to Submit; holding Confirm submits from anywhere. Back
// returns to the Connections home with the board kept.
// Archive: a month calendar; Left/Right step a day, side Up/Down a week.
//
// E-ink: every press is one FAST refresh of a full redraw; presses made during
// a refresh coalesce into the next frame. A finished puzzle shows its final
// board for one frame before the result card (any key skips).
class ConnectionsActivity final : public Activity {
 public:
  explicit ConnectionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Connections", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return screen == Screen::UPDATE && updateStep < UpdateStep::PICKING; }

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

 private:
  enum class Screen : uint8_t { HOME, GAME, RESULT, ARCHIVE, STATS, UPDATE };
  enum class Toast : uint8_t { NONE, ONE_AWAY, WRONG, REPEAT, NEED_FOUR, FULL };
  enum class UpdateStep : uint8_t { CONNECT, DOWNLOAD, PICKING, DONE, FAILED };
  enum HomeItem : uint8_t { HOME_TODAY, HOME_RANDOM, HOME_ARCHIVE, HOME_UPDATE, HOME_STATS, HOME_ITEMS };
  enum Action : uint8_t { ACT_SHUFFLE, ACT_DESELECT, ACT_SUBMIT, ACTION_COUNT };
  // Results are one byte per day since 2023-06-12; 4096 days reach 2034.
  static constexpr int MAX_RESULTS = 4096;

  // Pack.
  connections::PackReader pack;
  HalFile indexFile;
  HalFile dataFile;
  bool packOpen = false;
  uint32_t firstDate = 0;
  uint32_t lastDate = 0;

  // Results, held whole in RAM (~1.2 KB used, 4 KB buffer) so the home, the
  // calendar and "random unplayed" never touch the card per day.
  std::unique_ptr<uint8_t[]> results;
  int resultsLength = 0;
  connections::Summary summary;

  Screen screen = Screen::HOME;
  int homeSelected = HOME_TODAY;
  uint32_t today = 0;  // YYYYMMDD, 0 when the clock is unknown
  uint32_t rng = 1;

  // Current puzzle.
  connections::Game game;
  bool gameLoaded = false;
  bool replay = false;  // a puzzle already on record: played again, not counted
  bool recorded = false;
  int cursorRow = 0;  // 0..rows-1 tiles, rows = the action row
  int cursorCol = 0;
  Toast toast = Toast::NONE;
  uint8_t finaleStage = 0;  // 1 while the final board shows before the result card
  unsigned long finaleDueMs = 0;

  // Archive.
  uint32_t archiveDate = 0;

  // Update.
  UpdateStep updateStep = UpdateStep::CONNECT;
  const char* updateError = "";
  int updateAccepted = 0;
  bool wifiStartedHere = false;

  // Held-direction repeat.
  MappedInputManager::Button heldButton = MappedInputManager::Button::Back;
  unsigned long nextRepeatMs = 0;

  // Guarded by the render lock.
  bool cleanNext = true;
  int framesSinceClean = 0;

  // Pack and results (ConnectionsActivity.cpp).
  bool openPack();
  void closePack();
  void loadResults();
  void writeResult(uint32_t date, const connections::Record& record);
  connections::Record resultFor(uint32_t date) const;
  void refreshToday();
  int todayIndex() const;
  int unplayedCount() const;

  // Game.
  bool openPuzzle(int index);
  void saveGame() const;
  bool loadGame(const connections::Puzzle& puzzle);
  void finishGame();
  void submitSelection();
  void shuffleKeepingSelection();
  void pressConfirm();
  void moveCursor(int dCol, int dRow);
  int boardRows() const { return game.tileCount() / connections::kMembers; }
  int cursorTile() const { return cursorRow < boardRows() ? cursorRow * connections::kMembers + cursorCol : -1; }
  void focusAction(Action action);

  // Update.
  void startUpdate();
  void runConnect();
  void runDownload();
  void stopWifi();

  // Navigation.
  void showScreen(Screen next);
  void openHomeItem();
  void openRandom();
  void handleHome();
  void handleGame();
  void handleResult();
  void handleArchive();
  void handleStats();
  void handleUpdate();
  bool repeatPress(MappedInputManager::Button button);

  // Drawing (ConnectionsRender.cpp).
  void renderHome() const;
  void renderGame() const;
  void renderResult() const;
  void renderArchive() const;
  void renderStats() const;
  void renderUpdate() const;
  void drawHeader(const char* title, const char* subtitle) const;
  void drawGroupBar(int x, int y, int w, int h, const connections::Group& group, int groupIndex, bool solid) const;
  void drawTile(int tile, int fontIndex, int x, int y, int w, int h) const;
  void drawActions(int y, int h) const;
  void drawGuessGrid(int x, int y, int cell) const;
  void drawStatTiles(int y) const;
  void drawRecentStrip(int x, int y, int width) const;
};
