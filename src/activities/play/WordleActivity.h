#pragma once

#include <cstdint>

#include "WordleStore.h"
#include "WordleWords.h"
#include "activities/Activity.h"

// Wordle in English and Turkish, Daily (one Rome-dated word per language) and
// Random modes, with XP levels and per-language stats.
//
// Home: Left/Right or side Up/Down pick an entry, Confirm opens, Back exits.
// Game: Left/Right walk the on-screen keyboard row (wrapping, held = repeat),
// side Up/Down change row, Confirm types the key; the ENTER key, Confirm on a
// full row, or holding Confirm submits. Back deletes; on an empty row, or
// held, it returns to the Wordle home (the board is kept).
//
// E-ink: the game screen diffs every tile, key and band against what is on the
// panel and redraws only what changed, then does one FAST refresh; presses
// that land during a refresh coalesce into the next frame. A submitted row is
// revealed in two frames (three tiles, then all five plus the keyboard with a
// HALF refresh); any press skips straight to the final state.
class WordleActivity final : public Activity {
 public:
  explicit WordleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wordle", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { HOME, GAME, RESULT, STATS };
  enum class Note : uint8_t { NONE, LEGEND, TOO_SHORT, NOT_WORD, NO_LISTS };
  static constexpr int HOME_ITEMS = 5;
  static constexpr int MAX_KEYS = 12;
  static constexpr uint16_t STALE = 0xFFFF;

  wordle::Progress progress;
  wordle::Dictionary dictionary;
  bool progressDirty = false;
  int32_t today = -1;  // Rome day number, -1 when the clock is unknown
  uint32_t rng = 1;

  Screen screen = Screen::HOME;
  int homeSelected = 0;
  wordle::Lang statsLang = wordle::LANG_EN;

  // Current game.
  wordle::Lang lang = wordle::LANG_EN;
  bool daily = false;
  uint8_t input[wordle::WORD_LEN] = {};
  uint8_t inputLength = 0;
  uint8_t keyRow = 0;
  uint8_t keyCol = 0;
  Note note = Note::NONE;
  uint8_t revealStage = 0;  // 1 while only the first tiles of the last row are shown
  unsigned long revealDueMs = 0;
  unsigned long resultDueMs = 0;  // pending switch to the result card
  uint32_t xpGained = 0;
  bool leveledUp = false;

  // Held-direction repeat.
  MappedInputManager::Button heldButton = MappedInputManager::Button::Back;
  unsigned long nextRepeatMs = 0;

  // Guarded by the render lock: what the panel currently shows.
  bool fullRedraw = true;
  bool cleanNext = true;
  uint16_t shownTiles[wordle::MAX_GUESSES][wordle::WORD_LEN] = {};
  uint8_t shownKeys[wordle::KEY_ROWS][MAX_KEYS] = {};
  uint8_t shownNote = 0xFF;
  uint8_t shownHints = 0xFF;

  wordle::Board& board() { return daily ? progress.daily[lang] : progress.random[lang]; }
  const wordle::Board& board() const { return daily ? progress.daily[lang] : progress.random[lang]; }
  wordle::LangStats& stats() { return progress.stats[lang]; }

  // Logic (WordleActivity.cpp).
  void refreshToday();
  void openHomeItem();
  void startGame(wordle::Lang language, bool isDaily);
  void newRandomBoard();
  void goHome();
  void showScreen(Screen next);
  void handleHome();
  void handleGame();
  void handleResult();
  void handleStats();
  bool repeatPress(MappedInputManager::Button button);
  void moveColumn(int delta);
  void moveRow(int delta);
  void pressKey();
  void deleteLetter();
  void submit();
  void finishReveal();
  void persist();
  Note idleNote() const;
  uint32_t nextRandom();
  bool answerOverride(wordle::Word& out) const;

  // Drawing (WordleRender.cpp).
  void renderHome() const;
  void renderGame();
  void renderResult() const;
  void renderStats() const;
  void drawTile(int row, int col, uint16_t look) const;
  void drawKey(int row, int col, uint8_t look) const;
  void drawNote() const;
  void drawGameHints() const;
  void drawHeader(const char* title, const char* subtitle) const;
  void drawLevelBar(int x, int y, int width, uint32_t xp, bool large) const;
  void drawHistogram(int x, int y, int width, const wordle::LangStats& s, int highlight) const;
  void drawMiniBoard(int x, int y, int cell) const;
  uint16_t tileLook(int row, int col) const;
  void keyMarks(uint8_t* marks) const;
  int keyX(int row, int col, int* width) const;
  int keyPitch() const;
};
