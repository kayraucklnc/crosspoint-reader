#pragma once

#include <cstddef>
#include <cstdint>

#include "JokersArt.h"
#include "JokersRules.h"
#include "PlayCommon.h"
#include "activities/Activity.h"

// Noir Jokers: a poker roguelike. Pick cards from a hand of 8 and play poker
// hands for Chips x Mult to beat three blinds per ante across 8 antes, buying
// Jokers and Planets in a shop between blinds. Stats and the run in progress
// persist to /.crosspoint/games/jokers.json.
//
// Controls are the same on every screen: Left/Right move, Confirm acts on the
// highlighted thing, side Up/Down switch rows, Back opens the menu. In a round,
// holding Confirm plays the selection.
//
// Each press costs one panel refresh: presses made during a refresh are folded
// into the next frame, the round screen redraws only its dirty bands, and a
// screen change uses a HALF refresh.
class JokersActivity final : public Activity {
 public:
  explicit JokersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Jokers", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Screen : uint8_t { TITLE, BLIND, ROUND, REVEAL, CASH_OUT, SHOP, OVER, MENU, HELP, LEVELS };
  // Round rows top to bottom; the shop reuses them as offers, owned jokers, actions.
  enum Row : uint8_t { ROW_TOP, ROW_MID, ROW_BOTTOM };
  static constexpr uint8_t D_HEADER = 1, D_JOKERS = 2, D_INFO = 4, D_HAND = 8, D_ACTIONS = 16, D_FOOTER = 32,
                           D_HINTS = 64, D_ALL = 127;
  static constexpr int HELP_PAGES = 4;
  static constexpr int REVEAL_STEPS = 3;

  struct Stats {
    int32_t bestAnte = 0;
    int32_t bestHand = 0;
    int32_t runsWon = 0;
    int32_t runsPlayed = 0;
  };

  jokers::Run run;
  bool hasRun = false;
  Stats stats;
  jokers::art::Pack pack;
  play::FramePacer pacer;

  Screen screen = Screen::TITLE;
  Screen underlay = Screen::TITLE;  // the screen beneath MENU / HELP / LEVELS
  uint8_t row = ROW_MID;
  uint8_t cursor[3] = {};  // per row
  uint8_t selected = 0;    // bits over run.hand
  bool grabbed = false;    // moving a joker
  int8_t armed = -1;       // index awaiting a second press (sell / abandon)
  uint8_t menuCursor = 0;
  uint8_t helpPage = 0;
  const char* toast = nullptr;
  bool newRecord = false;
  bool saveDirty = false;

  jokers::Breakdown reveal;
  jokers::Payout payout;
  uint8_t revealStep = 0;
  unsigned long revealAt = 0;
  int32_t scoreBefore = 0;

  // Guarded by the render lock.
  uint8_t dirty = D_ALL;
  bool redrawAll = true;
  bool cleanRefresh = true;

  // Flow (JokersActivity.cpp).
  void go(Screen next, bool clean = true);
  void newRun();
  void startBlind();
  void finishReveal();
  void endRun();
  void openMenu();
  void menuSelect();
  void handleTitle(bool confirm, int dx);
  void handleBlind(bool confirm);
  void handleRound(bool confirm, bool hold, int dx, int dy);
  void handleShop(bool confirm, int dx, int dy);
  void handleOver(bool confirm);
  void handleReveal(bool anyPress);
  void roundAction();
  void playSelection();
  void mark(uint8_t bits);
  int rowLength(uint8_t r) const;
  bool jokersFired() const;
  void load();
  void save();
  void recordStats();

  // Drawing: shared pieces, round and reveal (JokersScreens.cpp), other pages (JokersPages.cpp).
  void drawScreen(Screen which);
  void advanceBlindPreview(char* out, size_t size) const;
  void drawTitle() const;
  void drawBlindSelect() const;
  void drawRound();
  void drawReveal() const;
  void drawCashOut() const;
  void drawShop() const;
  void drawOver() const;
  void drawMenu() const;
  void drawHelp() const;
  void drawLevels() const;
  void drawTitleBand(const char* title, const char* subtitle) const;
  void drawHeader(bool showScore, int32_t shownScore) const;
  void drawJokerRow(int y, bool focused, const jokers::Breakdown* fx) const;
  void drawRoundInfo() const;
  void drawHand() const;
  void drawActions() const;
  void drawFooter() const;
  void drawHints() const;
  void drawEquation(int y, int32_t chips, int32_t mult10, int32_t score) const;
  void drawMiniEquation(int right, int y, int32_t chips, int32_t mult) const;
  void drawButton(int x, int y, int w, int h, const char* label, bool focused, bool enabled, int badge) const;
  void statLine(int y, const char* label, const char* value) const;
  void pill(int cx, int y, const char* s, bool dark, int minWidth) const;
  void ring(int x, int y, int w, int h, int thickness) const;
  void textCenter(int font, int cx, int y, const char* s, bool bold, bool black = true) const;
  void textRight(int font, int right, int y, const char* s, bool bold, bool black = true) const;
  void clearBand(int y0, int y1) const;
  int handCardX(int index) const;
  int handCardY(int index) const;
  int jokerX(int index) const;
  static void formatMult(int32_t mult10, char* out, size_t size);
};
