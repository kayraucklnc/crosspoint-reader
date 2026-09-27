#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

#include "ConnectionsCore.h"
#include "ConnectionsPack.h"
#include "ConnectionsResults.h"
#include "activities/Activity.h"

// Connections: sixteen words, four hidden groups of four, four mistakes.
// Offline, in English (the NYT archive, scripts/gen_connections.py) or Turkish
// (an authored pack, scripts/gen_connections_tr.py), each a pack on the SD card
// (/.crosspoint/games/connections) with its own daily, results and board.
//
// The daily is ours, not the NYT's: day N since the language's launch day
// deals a fixed puzzle from a shuffled pass over the pack (ConnectionsDaily.h),
// so it follows the local date, needs no network and repeats nothing until the
// pack is used up. Random and the archive replay past dailies.
//
// Home: side Up/Down or Left/Right pick an entry (the language switch sits
// under the title), Confirm opens, Back exits.
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

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

 private:
  enum class Screen : uint8_t { HOME, GAME, RESULT, ARCHIVE, STATS };
  enum class Toast : uint8_t { NONE, ONE_AWAY, WRONG, REPEAT, NEED_FOUR, FULL };
  enum HomeItem : uint8_t { HOME_LANG, HOME_TODAY, HOME_RANDOM, HOME_ARCHIVE, HOME_STATS, HOME_ITEMS };
  enum Action : uint8_t { ACT_SHUFFLE, ACT_DESELECT, ACT_SUBMIT, ACTION_COUNT };
  enum Lang : uint8_t { LANG_EN, LANG_TR, LANG_COUNT };
  // Results are one byte per day since the launch day; 4096 days is 11 years.
  static constexpr int MAX_RESULTS = 4096;

  // Pack of the current language.
  Lang lang = LANG_EN;
  connections::PackReader pack;
  HalFile indexFile;
  HalFile dataFile;
  bool packOpen = false;

  // Results of the current language, held whole in RAM (4 KB) so the home,
  // the calendar and Random never touch the card per day.
  std::unique_ptr<uint8_t[]> results;
  int resultsLength = 0;
  connections::Summary summary;

  Screen screen = Screen::HOME;
  int homeSelected = HOME_TODAY;
  uint32_t today = 0;         // YYYYMMDD: the local date, else the last one seen
  bool clockKnown = false;    // false: `today` is the last date seen, not the clock's
  uint32_t lastSeenDate = 0;  // persisted so an unset clock keeps the latest daily
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

  // Held-direction repeat.
  MappedInputManager::Button heldButton = MappedInputManager::Button::Back;
  unsigned long nextRepeatMs = 0;

  // Guarded by the render lock.
  bool cleanNext = true;
  int framesSinceClean = 0;

  // Language, pack and results (ConnectionsActivity.cpp).
  void loadPrefs();
  void savePrefs() const;
  void switchLanguage(Lang next);
  void openLanguage();
  bool openPack();
  void closePack();
  void loadResults();
  void writeResult(uint32_t date, const connections::Record& record);
  connections::Record resultFor(uint32_t date) const;
  void refreshToday();
  uint32_t launchDate() const;
  int dayIndex(uint32_t date) const;  // days since the launch day, -1 before it
  int dailyNumber(uint32_t date) const { return dayIndex(date) + 1; }
  int unplayedCount() const;

  // Game.
  bool openDaily(uint32_t date);
  bool readDaily(uint32_t date, connections::Puzzle& out) const;
  void restoreSavedGame();
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

  // Navigation.
  void showScreen(Screen next);
  void openHomeItem();
  void openRandom();
  void handleHome();
  void handleGame();
  void handleResult();
  void handleArchive();
  void handleStats();
  bool repeatPress(MappedInputManager::Button button);

  // Drawing (ConnectionsRender.cpp).
  const char* title() const;
  void renderHome() const;
  void renderGame() const;
  void renderResult() const;
  void renderArchive() const;
  void renderStats() const;
  void drawHeader(const char* title, const char* subtitle) const;
  void drawLanguageSwitch(int x, int y, bool focused) const;
  void drawGroupBar(int x, int y, int w, int h, const connections::Group& group, int groupIndex, bool solid) const;
  void drawTile(int tile, int x, int y, int w, int h) const;
  void drawActions(int y, int h) const;
  void drawGuessGrid(int x, int y, int cell) const;
  void drawStatTiles(int y) const;
  void drawRecentStrip(int x, int y, int width) const;
  void formatPuzzleLabel(uint32_t date, const char* dateFormat, char* out, int size) const;
};
