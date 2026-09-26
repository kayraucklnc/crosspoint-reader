#include "BlackjackActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* SAVE_PATH = "/.crosspoint/blackjack.json";
constexpr int32_t START_BANK = 1000;
constexpr int BET_STEPS[] = {10, 25, 50, 100, 250, 500};
constexpr int BET_STEP_COUNT = sizeof(BET_STEPS) / sizeof(BET_STEPS[0]);
constexpr int CARD_W = 118;
constexpr int CARD_H = 168;
constexpr int SIDE_GAP = 48;  // keeps cards clear of the side-button hints
constexpr unsigned long DEALER_STEP_MS = 650;
constexpr unsigned long HELP_HOLD_MS = 700;
constexpr const char* RANKS[13] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

void money(char* out, const size_t size, const int32_t value) { snprintf(out, size, "$%ld", static_cast<long>(value)); }
}  // namespace

void BlackjackActivity::onEnter() {
  Activity::onEnter();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  load();
  requestUpdate();
}

void BlackjackActivity::onExit() {
  save();
  Activity::onExit();
}

void BlackjackActivity::load() {
  bank = START_BANK;
  char buf[64] = {};
  if (Storage.exists(SAVE_PATH) && Storage.readFileToBuffer(SAVE_PATH, buf, sizeof(buf) - 1) > 0) {
    if (const char* b = strstr(buf, "\"bank\":")) bank = atol(b + 7);
    if (const char* s = strstr(buf, "\"bet\":")) betStep = static_cast<uint8_t>(std::clamp(atoi(s + 6), 0, 5));
  }
  if (bank < BET_STEPS[0]) bank = START_BANK;
}

void BlackjackActivity::save() {
  if (!bankDirty) return;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"bank\":%ld,\"bet\":%d}", static_cast<long>(bank), betStep);
  if (!Storage.writeFile(SAVE_PATH, String(buf))) LOG_ERR("BJ", "Failed to write %s", SAVE_PATH);
  bankDirty = false;
}

int BlackjackActivity::bet() const { return BET_STEPS[betStep]; }

uint8_t BlackjackActivity::draw() {
  if (deckPos >= 52) {
    for (int i = 0; i < 52; i++) deck[i] = static_cast<uint8_t>(i);
    for (int i = 51; i > 0; i--) {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      std::swap(deck[i], deck[rng % (i + 1)]);
    }
    deckPos = 0;
  }
  return deck[deckPos++];
}

int BlackjackActivity::handValue(const uint8_t* cards, const int count) {
  int total = 0;
  bool ace = false;
  for (int i = 0; i < count; i++) {
    const int rank = cards[i] % 13;
    total += rank >= 9 ? 10 : rank + 1;
    ace = ace || rank == 0;
  }
  return (ace && total + 10 <= 21) ? total + 10 : total;
}

void BlackjackActivity::changeBet(const int delta) {
  const int next = std::clamp(betStep + delta, 0, BET_STEP_COUNT - 1);
  if (BET_STEPS[next] > bank && delta > 0) return;
  betStep = static_cast<uint8_t>(next);
  bankDirty = true;
  dirty |= DIRTY_BET;
  requestUpdate();
}

void BlackjackActivity::deal() {
  if (bank < BET_STEPS[0]) bank = START_BANK;  // broke: the house stakes you again
  while (betStep > 0 && bet() > bank) betStep--;
  if (deckPos > 36) deckPos = 52;  // reshuffle a thin shoe before the hand
  wager = bet();
  bank -= wager;
  playerCount = dealerCount = 0;
  player[playerCount++] = draw();
  dealer[dealerCount++] = draw();
  player[playerCount++] = draw();
  dealer[dealerCount++] = draw();
  holeHidden = true;
  outcome = Outcome::NONE;
  state = State::PLAYER;
  fullRedraw = true;
  if (handValue(player, 2) == 21 || handValue(dealer, 2) == 21) {
    holeHidden = false;
    resolve();
  }
  requestUpdate();
}

void BlackjackActivity::hit() {
  if (playerCount >= MAX_HAND) return;
  player[playerCount++] = draw();
  dirty |= DIRTY_PLAYER;
  const int value = handValue(player, playerCount);
  if (value > 21) {
    resolve();
  } else if (value == 21) {
    stand();
  }
  requestUpdate();
}

void BlackjackActivity::stand() {
  state = State::DEALER;
  holeHidden = false;
  dealerTickMs = millis();
  dirty |= DIRTY_DEALER | DIRTY_MID;
  requestUpdate();
}

void BlackjackActivity::doubleDown() {
  if (playerCount != 2 || bank < wager) return;
  bank -= wager;
  wager *= 2;
  dirty |= DIRTY_TOP | DIRTY_BET;
  player[playerCount++] = draw();
  dirty |= DIRTY_PLAYER;
  if (handValue(player, playerCount) > 21) {
    resolve();
  } else {
    stand();
  }
  requestUpdate();
}

void BlackjackActivity::dealerTick() {
  if (millis() - dealerTickMs < DEALER_STEP_MS) return;
  dealerTickMs = millis();
  if (handValue(dealer, dealerCount) < 17 && dealerCount < MAX_HAND) {
    dealer[dealerCount++] = draw();
    dirty |= DIRTY_DEALER;
    requestUpdate();
    return;
  }
  resolve();
}

void BlackjackActivity::resolve() {
  holeHidden = false;
  const int you = handValue(player, playerCount);
  const int house = handValue(dealer, dealerCount);
  const bool youNatural = playerCount == 2 && you == 21;
  const bool houseNatural = dealerCount == 2 && house == 21;
  int32_t payout = 0;
  if (you > 21) {
    outcome = Outcome::BUST;
  } else if (youNatural && !houseNatural) {
    outcome = Outcome::BLACKJACK;
    payout = wager + wager * 3 / 2;
  } else if (houseNatural && !youNatural) {
    outcome = Outcome::LOSE;
  } else if (house > 21 || you > house) {
    outcome = Outcome::WIN;
    payout = wager * 2;
  } else if (you == house) {
    outcome = Outcome::PUSH;
    payout = wager;
  } else {
    outcome = Outcome::LOSE;
  }
  bank += payout;
  lastPayout = payout - wager;
  bankDirty = true;
  save();
  state = State::RESULT;
  dirty = DIRTY_ALL;
  requestUpdate();
}

void BlackjackActivity::loop() {
  using B = MappedInputManager::Button;
  RenderLock lock(*this);
  if (helpOpen) {
    if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm) || mappedInput.wasReleased(B::Left) ||
        mappedInput.wasReleased(B::Right) || mappedInput.wasReleased(B::Up) || mappedInput.wasReleased(B::Down)) {
      helpOpen = false;
      fullRedraw = true;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(B::Back)) {
    lock.unlock();
    finish();
    return;
  }
  if (mappedInput.wasLongPressed(B::Confirm, HELP_HOLD_MS)) {
    helpOpen = true;
    requestUpdate();
    return;
  }
  switch (state) {
    case State::BETTING:
    case State::RESULT:
      if (mappedInput.wasReleased(B::Confirm)) deal();
      if (mappedInput.wasPressed(B::Up)) changeBet(+1);
      if (mappedInput.wasPressed(B::Down)) changeBet(-1);
      break;
    case State::PLAYER:
      if (mappedInput.wasPressed(B::Left)) {
        hit();
      } else if (mappedInput.wasPressed(B::Right)) {
        stand();
      } else if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down)) {
        doubleDown();
      }
      break;
    case State::DEALER:
      dealerTick();
      break;
  }
}

// ---- drawing -----------------------------------------------------------------

int BlackjackActivity::bottom() const {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 4;
}

int BlackjackActivity::playerY() const { return bottom() - CARD_H - 120; }

// A calm white table: bands are simply cleared before being redrawn.
void BlackjackActivity::clearBand(const int y0, const int y1) const {
  const int top = std::max(y0, 0), low = std::min(y1, bottom());
  if (top <= low) renderer.fillRect(0, top, renderer.getScreenWidth(), low - top + 1, false);
}

void BlackjackActivity::drawTop() const {
  const int width = renderer.getScreenWidth();
  clearBand(0, dealerY() - 7);
  renderer.drawText(UI_12_FONT_ID, 20, 16, tr(STR_BJ_TITLE), true, EpdFontFamily::BOLD);
  char text[32], amount[16];
  money(amount, sizeof(amount), bank);
  snprintf(text, sizeof(text), "%s  %s", tr(STR_BJ_BANK), amount);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, width - w - 20, 16, text, true, EpdFontFamily::BOLD);
  renderer.drawLine(20, 50, width - 21, 50, 2, true);
  renderer.drawLine(20, 54, width - 21, 54, true);
}

void BlackjackActivity::drawSuitShape(const int suit, const int cx, const int cy, const int r, const bool black) const {
  const Color color = black ? Color::Black : Color::White;
  const auto disc = [&](const int x, const int y, const int radius) {
    renderer.fillRoundedRect(x - radius, y - radius, radius * 2, radius * 2, radius, color);
  };
  switch (suit) {
    case 0: {  // spade
      disc(cx - r / 2, cy + r / 5, r / 2 + 1);
      disc(cx + r / 2, cy + r / 5, r / 2 + 1);
      const int xs[3] = {cx - r, cx + r, cx}, ys[3] = {cy + r / 6, cy + r / 6, cy - r};
      renderer.fillPolygon(xs, ys, 3, black);
      const int sx[3] = {cx, cx - r / 3, cx + r / 3}, sy[3] = {cy + r / 4, cy + r, cy + r};
      renderer.fillPolygon(sx, sy, 3, black);
      break;
    }
    case 1: {  // heart
      disc(cx - r / 2, cy - r / 4, r / 2 + 1);
      disc(cx + r / 2, cy - r / 4, r / 2 + 1);
      const int xs[3] = {cx - r, cx + r, cx}, ys[3] = {cy - r / 8, cy - r / 8, cy + r};
      renderer.fillPolygon(xs, ys, 3, black);
      break;
    }
    case 2: {  // club
      const int lobe = r * 9 / 20;
      disc(cx, cy - r / 2, lobe);
      disc(cx - r / 2, cy + r / 8, lobe);
      disc(cx + r / 2, cy + r / 8, lobe);
      const int sx[3] = {cx, cx - r / 3, cx + r / 3}, sy[3] = {cy - r / 8, cy + r, cy + r};
      renderer.fillPolygon(sx, sy, 3, black);
      break;
    }
    default: {  // diamond
      const int xs[4] = {cx, cx + r * 3 / 4, cx, cx - r * 3 / 4}, ys[4] = {cy - r, cy, cy + r, cy};
      renderer.fillPolygon(xs, ys, 4, black);
      break;
    }
  }
}

// Hearts and diamonds are "red": drawn as outlines, black suits solid.
void BlackjackActivity::drawSuit(const int suit, const int cx, const int cy, const int r, const bool black) const {
  drawSuitShape(suit, cx, cy, r, black);
  if ((suit == 1 || suit == 3) && r >= 6) drawSuitShape(suit, cx, cy + (suit == 1 ? -1 : 0), r - 3, !black);
}

void BlackjackActivity::drawCard(const int x, const int y, const uint8_t card) const {
  const int rank = card % 13, suit = card / 13;
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 10, Color::White);
  renderer.drawRoundedRect(x, y, CARD_W, CARD_H, 3, 10, true);
  renderer.drawText(UI_12_FONT_ID, x + 11, y + 8, RANKS[rank], true, EpdFontFamily::BOLD);
  drawSuit(suit, x + 21, y + 50, 10, true);
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, RANKS[rank], EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + CARD_W - 11 - tw, y + CARD_H - 38, RANKS[rank], true, EpdFontFamily::BOLD);
  drawSuit(suit, x + CARD_W - 21, y + CARD_H - 50, 10, true);
  if (rank >= 10) {
    // Court cards: a double frame around the pip.
    renderer.drawRect(x + 36, y + 34, CARD_W - 72, CARD_H - 68, 2, true);
    renderer.drawRect(x + 40, y + 38, CARD_W - 80, CARD_H - 76, 1, true);
  }
  drawSuit(suit, x + CARD_W / 2, y + CARD_H / 2, rank >= 10 ? 14 : 26, true);
}

void BlackjackActivity::drawCardBack(const int x, const int y) const {
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 10, Color::Black);
  renderer.fillRect(x + 6, y + 6, CARD_W - 12, CARD_H - 12, false);
  renderer.fillRect(x + 8, y + 8, CARD_W - 16, CARD_H - 16, true);
  const auto clip = renderer.getClipRect();
  renderer.setClipRect(x + 9, y + 9, CARD_W - 18, CARD_H - 18);
  for (int d = -CARD_H; d < CARD_W; d += 10) {
    renderer.drawLine(x + d, y, x + d + CARD_H, y + CARD_H, false);
    renderer.drawLine(x + d + CARD_H, y, x + d, y + CARD_H, false);
  }
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  renderer.drawRect(x + 9, y + 9, CARD_W - 18, CARD_H - 18, 1, false);
  renderer.fillRect(x + CARD_W / 2 - 12, y + CARD_H / 2 - 12, 24, 24, true);
  drawSuit(0, x + CARD_W / 2, y + CARD_H / 2, 8, false);
}

void BlackjackActivity::drawHand(const uint8_t* cards, const int count, const int y, const bool hideSecond,
                                 const char* label, const int value) const {
  const int width = renderer.getScreenWidth();
  clearBand(y - 6, y + CARD_H + 33);
  const int avail = width - 2 * SIDE_GAP - CARD_W;
  const int step = count > 1 ? std::min(CARD_W + 10, avail / (count - 1)) : 0;
  const int x0 = (width - (CARD_W + step * (count - 1))) / 2;
  for (int i = 0; i < count; i++) {
    if (hideSecond && i == 1) {
      drawCardBack(x0 + i * step, y);
    } else {
      drawCard(x0 + i * step, y, cards[i]);
    }
  }
  if (count == 0) return;
  char text[24];
  if (value > 0) {
    snprintf(text, sizeof(text), "%s  %d", label, value);
  } else {
    snprintf(text, sizeof(text), "%s", label);
  }
  const int w = renderer.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, (width - w) / 2, y + CARD_H + 6, text, true, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawMid() const {
  const int width = renderer.getScreenWidth();
  const int top = dealerY() + CARD_H + 34, low = playerY() - 7;
  clearBand(top, low);
  // Table print: one thin rail arc with the house rules beneath it.
  const int arcY = top + 10;
  int prevX = 40, prevY = arcY;
  for (int x = 40; x <= width - 40; x += 8) {
    const int dx = x - width / 2;
    const int y = arcY + (width / 2 * width / 2 - dx * dx) / (width * 3);
    if (x > 40) renderer.drawLine(prevX, prevY, x, y, true);
    prevX = x, prevY = y;
  }
  renderer.drawCenteredText(SMALL_FONT_ID, arcY + 56, tr(STR_BJ_TABLE_PAYS));
  renderer.drawCenteredText(SMALL_FONT_ID, arcY + 76, tr(STR_BJ_TABLE_STANDS));

  char line[40];
  const char* message = nullptr;
  char amount[16];
  switch (outcome) {
    case Outcome::WIN:
    case Outcome::BLACKJACK:
      money(amount, sizeof(amount), lastPayout);
      snprintf(line, sizeof(line), "%s  +%s", outcome == Outcome::WIN ? tr(STR_BJ_WIN) : tr(STR_BJ_BLACKJACK), amount);
      message = line;
      break;
    case Outcome::PUSH:
      message = tr(STR_BJ_PUSH);
      break;
    case Outcome::LOSE:
      message = tr(STR_BJ_LOSE);
      break;
    case Outcome::BUST:
      message = tr(STR_BJ_BUST);
      break;
    case Outcome::NONE:
      if (state == State::BETTING) message = tr(STR_BJ_PLACE_BET);
      break;
  }
  if (!message) return;
  const int w = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD) + 48;
  const int boxY = (arcY + 100 + low) / 2 - 22;
  renderer.fillRect((width - w) / 2, boxY, w, 44, true);
  renderer.drawRect((width - w) / 2 + 4, boxY + 4, w - 8, 36, 1, false);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 9, message, false, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawBet() const {
  const int width = renderer.getScreenWidth();
  const int top = playerY() + CARD_H + 34;
  clearBand(top, bottom());
  const int amount = state == State::BETTING || state == State::RESULT ? bet() : wager;
  // A side-on chip stack, one chip per 25.
  const int chips = std::clamp(amount / 50 + 1, 1, 7);
  const int baseY = bottom() - 16;
  const int cx = width / 2;
  for (int i = 0; i < chips; i++) {
    const int y = baseY - i * 8;
    renderer.fillRoundedRect(cx - 30, y - 8, 60, 12, 5, Color::White);
    renderer.drawRoundedRect(cx - 30, y - 8, 60, 12, 1, 5, true);
    for (int s = -20; s <= 20; s += 20) renderer.fillRect(cx + s - 3, y - 7, 6, 10, true);
  }
  char text[24], value[16];
  money(value, sizeof(value), amount);
  snprintf(text, sizeof(text), "%s %s", tr(STR_BJ_BET), value);
  renderer.drawText(UI_12_FONT_ID, cx + 46, baseY - 26, text, true, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawHints() const {
  const int width = renderer.getScreenWidth();
  const bool playing = state == State::PLAYER;
  const bool betting = state == State::BETTING || state == State::RESULT;
  renderer.fillRect(0, bottom() + 1, width, renderer.getScreenHeight() - bottom() - 1, false);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), betting ? tr(STR_BJ_DEAL) : "", playing ? tr(STR_BJ_HIT) : "",
                                            playing ? tr(STR_BJ_STAND) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const bool canDouble = playing && playerCount == 2 && bank >= wager;
  const char* up = betting ? tr(STR_BJ_BET_UP) : (canDouble ? tr(STR_BJ_DOUBLE) : "");
  const char* down = betting ? tr(STR_BJ_BET_DOWN) : (canDouble ? tr(STR_BJ_DOUBLE) : "");
  GUI.drawSideButtonHints(renderer, up, down);
}

void BlackjackActivity::drawHelp() const {
  static constexpr StrId LINES[] = {StrId::STR_BJ_HELP1, StrId::STR_BJ_HELP2, StrId::STR_BJ_HELP3,
                                    StrId::STR_BJ_HELP4, StrId::STR_BJ_HELP5};
  const int width = renderer.getScreenWidth();
  const int boxX = 24, boxW = width - 48, boxY = 120;
  constexpr int LINE_H = 58;
  const int boxH = 70 + LINE_H * static_cast<int>(sizeof(LINES) / sizeof(LINES[0]));
  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, 12, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 2, 12, true);
  renderer.drawRoundedRect(boxX + 5, boxY + 5, boxW - 10, boxH - 10, 2, 9, true);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 20, tr(STR_BJ_HELP_TITLE), true, EpdFontFamily::BOLD);
  int y = boxY + 64;
  for (const StrId id : LINES) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(id), boxW - 60, 2);
    for (const auto& text : lines) {
      renderer.drawText(UI_10_FONT_ID, boxX + 30, y, text.c_str());
      y += 24;
    }
    y += LINE_H - 24 * static_cast<int>(lines.size());
  }
}

void BlackjackActivity::render(RenderLock&& lock) {
  const bool clean = fullRedraw;
  if (fullRedraw) {
    renderer.clearScreen();
    dirty = DIRTY_ALL;
    fullRedraw = false;
  }
  if (helpOpen) {
    drawHelp();
  } else {
    const int value = holeHidden ? 0 : handValue(dealer, dealerCount);
    if (dirty & DIRTY_TOP) drawTop();
    if (dirty & DIRTY_DEALER) drawHand(dealer, dealerCount, dealerY(), holeHidden, tr(STR_BJ_DEALER), value);
    if (dirty & DIRTY_MID) drawMid();
    if (dirty & DIRTY_PLAYER) {
      drawHand(player, playerCount, playerY(), false, tr(STR_BJ_YOU), handValue(player, playerCount));
    }
    if (dirty & DIRTY_BET) drawBet();
    drawHints();
    dirty = 0;
  }
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
