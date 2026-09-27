#pragma once

#include <I18n.h>

#include <cstdint>

#include "TriviaPack.h"
#include "TriviaStore.h"
#include "activities/Activity.h"

// Trivia: multiple-choice questions from an Open Trivia Database pack on the
// SD card (/.crosspoint/games/trivia, built by scripts/gen_trivia.py). Single
// player, after CrossPlay's solo mode: a Daily ten (the same ten all day, one
// go, streak across days), Quick rounds of ten with difficulty and category
// filters, and Sudden death (answer until the first miss). Questions not yet
// served come first.
//
// Home: Left/Right or side Up/Down pick an entry, Confirm opens, Back exits.
// Question: Left/Right or side Up/Down move between answers, Confirm answers,
// then Confirm again goes on. Back returns to the Trivia home (a daily keeps
// its place). Filters: side Up/Down pick the row, Left/Right change it.
//
// E-ink: one FAST refresh per press, from a full redraw; presses made during a
// refresh coalesce into the next frame. A new question every fourth one, and
// every screen change, is a HALF refresh to clear ghosting.
class TriviaActivity final : public Activity {
 public:
  explicit TriviaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Trivia", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

 private:
  enum class Screen : uint8_t { HOME, GAME, RESULT, FILTERS, STATS };
  enum class Mode : uint8_t { DAILY, QUICK, SUDDEN };
  enum HomeItem : uint8_t { HOME_DAILY, HOME_QUICK, HOME_SUDDEN, HOME_FILTERS, HOME_STATS, HOME_ITEMS };

  trivia::Pack pack;
  trivia::SeenSet seen;
  trivia::Stats stats;
  trivia::Rng rng;
  bool statsDirty = false;

  Screen screen = Screen::HOME;
  int homeSelected = HOME_DAILY;
  int filterRow = 0;
  int32_t today = -1;  // local day number, -1 when the clock is unknown

  // Current round.
  Mode mode = Mode::QUICK;
  int roundPos = 0;        // questions answered this round
  uint16_t roundMask = 0;  // bit i: question i answered right
  int runLength = 0;       // sudden death: right answers in a row
  bool newBest = false;
  int dailyPicks[trivia::ROUND_LENGTH] = {};

  // Current question.
  int current = -1;
  trivia::Question question;
  const char* options[trivia::MAX_OPTIONS] = {};
  int optionCount = 0;
  int correctOption = 0;
  int cursor = 0;
  int chosen = -1;  // -1 until answered

  // Guarded by the render lock.
  bool cleanNext = true;
  int framesSinceClean = 0;

  // Logic (TriviaActivity.cpp).
  void refreshToday();
  void persist();
  void showScreen(Screen next);
  void goHome();
  void openHomeItem();
  void startRound(Mode next);
  bool nextQuestion();
  int pickQuestion();
  void planDaily();
  bool loadQuestion(int index);
  void answer(int option);
  void finishRound();
  bool roundOver() const;
  void handleHome();
  void handleGame();
  void handleResult();
  void handleFilters();
  void handleStats();
  int dailyScore() const;

  // Drawing (TriviaRender.cpp).
  void renderHome() const;
  void renderGame() const;
  void renderResult() const;
  void renderFilters() const;
  void renderStats() const;
  void drawHeader(const char* title, const char* subtitle) const;
  void drawStatTiles(int y, const int* values, const StrId* labels) const;
  void drawPips(int y, uint16_t mask, int answered, int currentIndex) const;
  int drawWrapped(int fontId, const char* text, int x, int y, int width, int maxLines, bool black,
                  EpdFontFamily::Style style) const;
  void drawOption(int option, int x, int y, int w, int h) const;
  const char* modeName(Mode which) const;
  void filterSummary(char* out, int size) const;
};
