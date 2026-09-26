#pragma once

#include <cstdint>

#include "PlayCommon.h"
#include "activities/Activity.h"

// Noir Blackjack: one deck, dealer stands on all 17s, blackjack pays 3:2,
// double on the first two cards. The bankroll persists on SD.
// Left = Hit, Right = Stand, side Up/Down = bet +/- between hands or Double in
// a hand, Confirm = Deal, hold Confirm = rules, Back = exit.
//
// The felt is a pure function of (x, y), so any region can be repainted in
// place: each change redraws only the dirty bands and FAST-refreshes; every new
// hand starts with a HALF refresh to clear ghosting of the dark table.
class BlackjackActivity final : public Activity {
 public:
  explicit BlackjackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Blackjack", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { BETTING, PLAYER, DEALER, RESULT };
  enum class Outcome : uint8_t { NONE, WIN, BLACKJACK, PUSH, LOSE, BUST };
  static constexpr int MAX_HAND = 11;
  static constexpr uint8_t DIRTY_TOP = 1, DIRTY_DEALER = 2, DIRTY_MID = 4, DIRTY_PLAYER = 8, DIRTY_BET = 16;
  static constexpr uint8_t DIRTY_ALL = 31;

  State state = State::BETTING;
  Outcome outcome = Outcome::NONE;
  uint8_t deck[52] = {};
  uint8_t player[MAX_HAND] = {};
  uint8_t dealer[MAX_HAND] = {};
  uint8_t deckPos = 52;
  uint8_t playerCount = 0;
  uint8_t dealerCount = 0;
  bool holeHidden = true;
  int32_t bank = 0;
  int32_t wager = 0;
  int32_t lastPayout = 0;
  uint8_t betStep = 1;
  uint32_t rng = 1;
  unsigned long dealerTickMs = 0;
  bool bankDirty = false;

  // Guarded by the render lock.
  uint8_t dirty = DIRTY_ALL;
  bool fullRedraw = true;
  bool helpOpen = false;

  int bet() const;
  uint8_t draw();
  static int handValue(const uint8_t* cards, int count);
  void deal();
  void hit();
  void stand();
  void doubleDown();
  void resolve();
  void dealerTick();
  void changeBet(int delta);
  void load();
  void save();

  // Layout (portrait-first, derived from the screen height).
  int dealerY() const { return 74; }
  int playerY() const;
  int bottom() const;

  void paintFelt(int x0, int y0, int x1, int y1) const;
  void drawTop() const;
  void drawHand(const uint8_t* cards, int count, int y, bool hideSecond, const char* label, int value) const;
  void drawMid() const;
  void drawBet() const;
  void drawHints() const;
  void drawHelp() const;
  void drawCard(int x, int y, uint8_t card) const;
  void drawCardBack(int x, int y) const;
  void drawSuit(int suit, int cx, int cy, int r, bool black) const;
  void drawSuitShape(int suit, int cx, int cy, int r, bool black) const;
};
