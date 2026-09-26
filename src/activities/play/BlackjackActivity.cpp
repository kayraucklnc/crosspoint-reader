#include "BlackjackActivity.h"

#include <HalGPIO.h>
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
constexpr int CARD_W = 84;
constexpr int CARD_H = 120;
constexpr int SIDE_GAP = 48;  // keeps cards clear of the side-button hints
constexpr unsigned long DEALER_STEP_MS = 650;
constexpr unsigned long HELP_HOLD_MS = 700;
constexpr const char* RANKS[13] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
constexpr uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
constexpr int SIDE_HINT_Y = 155;  // matches BaseTheme's edge side-button hint position
constexpr int SIDE_HINT_W = 30;
constexpr int SIDE_HINT_H = 80;

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

int BlackjackActivity::playerY() const { return bottom() - CARD_H - 150; }

// Black felt lit by a soft oval spotlight: ordered dither whose density falls
// off with distance from the centre of the table.
void BlackjackActivity::paintFelt(int x0, int y0, int x1, int y1) const {
  const int width = renderer.getScreenWidth();
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, width - 1);
  y1 = std::min(y1, bottom());
  if (x0 > x1 || y0 > y1) return;
  renderer.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, true);
  const int cx = width / 2, cy = (dealerY() + playerY() + CARD_H) / 2;
  const int rx = width * 3 / 5, ry = bottom() / 2;
  for (int y = y0; y <= y1; y++) {
    const int ny = (y - cy) * 256 / ry;
    if (ny * ny >= 256 * 256) continue;
    for (int x = x0; x <= x1; x++) {
      const int nx = (x - cx) * 256 / rx;
      const int d2 = (nx * nx + ny * ny) >> 8;  // 0 at centre, 256 on the rim
      const int level = 5 - d2 * 6 / 256;
      if (level > 0 && BAYER[y & 3][x & 3] < level) renderer.drawPixel(x, y, false);
    }
  }
}

void BlackjackActivity::drawTop() const {
  const int width = renderer.getScreenWidth();
  paintFelt(0, 0, width - 1, dealerY() - 7);
  renderer.drawText(UI_12_FONT_ID, 20, 16, tr(STR_BJ_TITLE), false, EpdFontFamily::BOLD);
  char text[32], amount[16];
  money(amount, sizeof(amount), bank);
  snprintf(text, sizeof(text), "%s  %s", tr(STR_BJ_BANK), amount);
  const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, width - w - 20, 16, text, false, EpdFontFamily::BOLD);
  renderer.drawLine(20, 52, width - 21, 52, false);
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
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 8, Color::White);
  renderer.drawRoundedRect(x + 3, y + 3, CARD_W - 6, CARD_H - 6, 1, 6, true);
  renderer.drawText(UI_12_FONT_ID, x + 9, y + 6, RANKS[rank], true, EpdFontFamily::BOLD);
  drawSuit(suit, x + 17, y + 44, 8, true);
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, RANKS[rank], EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + CARD_W - 9 - tw, y + CARD_H - 34, RANKS[rank], true, EpdFontFamily::BOLD);
  if (rank >= 10) {
    // Court cards: a framed panel around the pip.
    renderer.drawRect(x + 26, y + 26, CARD_W - 40, CARD_H - 52, 2, true);
    renderer.drawRect(x + 30, y + 30, CARD_W - 48, CARD_H - 60, 1, true);
  }
  drawSuit(suit, x + CARD_W / 2 + 6, y + CARD_H / 2, rank >= 10 ? 13 : 18, true);
}

void BlackjackActivity::drawCardBack(const int x, const int y) const {
  renderer.fillRoundedRect(x, y, CARD_W, CARD_H, 8, Color::White);
  renderer.fillRect(x + 5, y + 5, CARD_W - 10, CARD_H - 10, true);
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
  paintFelt(0, y - 6, width - 1, y + CARD_H + 33);
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
  renderer.drawText(UI_10_FONT_ID, (width - w) / 2, y + CARD_H + 6, text, false, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawMid() const {
  const int width = renderer.getScreenWidth();
  const int top = dealerY() + CARD_H + 34, low = playerY() - 7;
  paintFelt(0, top, width - 1, low);
  // Table print: a dashed arc with the house rules.
  const int arcY = top + 14;
  for (int x = 50; x < width - 50; x += 12) {
    const int dx = x - width / 2;
    const int y = arcY + (width / 2 * width / 2 - dx * dx) / (width * 3);
    renderer.fillRect(x, y, 6, 2, false);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, arcY + 56, tr(STR_BJ_TABLE_PAYS), false);
  renderer.drawCenteredText(SMALL_FONT_ID, arcY + 76, tr(STR_BJ_TABLE_STANDS), false);

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
  renderer.drawRect((width - w) / 2, boxY, w, 44, 2, false);
  renderer.drawRect((width - w) / 2 + 4, boxY + 4, w - 8, 36, 1, false);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 9, message, false, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawBet() const {
  const int width = renderer.getScreenWidth();
  const int top = playerY() + CARD_H + 34;
  paintFelt(0, top, width - 1, bottom());
  const int amount = state == State::BETTING || state == State::RESULT ? bet() : wager;
  // A side-on chip stack, one chip per 25.
  const int chips = std::clamp(amount / 25 + 1, 1, 10);
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
  renderer.drawText(UI_12_FONT_ID, cx + 46, baseY - 26, text, false, EpdFontFamily::BOLD);
}

void BlackjackActivity::drawHints() const {
  const int width = renderer.getScreenWidth();
  const bool playing = state == State::PLAYER;
  const bool betting = state == State::BETTING || state == State::RESULT;
  renderer.fillRect(0, bottom() + 1, width, renderer.getScreenHeight() - bottom() - 1, false);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), betting ? tr(STR_BJ_DEAL) : "", playing ? tr(STR_BJ_HIT) : "",
                                            playing ? tr(STR_BJ_STAND) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (gpio.hasTouch()) return;
  // Side-button hints in the table's style (the theme's are black-on-white).
  const bool canDouble = playing && playerCount == 2 && bank >= wager;
  const char* up = betting ? tr(STR_BJ_BET_UP) : (canDouble ? tr(STR_BJ_DOUBLE) : "");
  const char* down = betting ? tr(STR_BJ_BET_DOWN) : (canDouble ? tr(STR_BJ_DOUBLE) : "");
  const bool edge = gpio.hasEdgeSideButtons();
  const int xs[2] = {edge ? 4 : width - 4 - SIDE_HINT_W, width - 4 - SIDE_HINT_W};
  const int ys[2] = {edge ? SIDE_HINT_Y : 345, edge ? SIDE_HINT_Y : 345 + SIDE_HINT_H + 4};
  const char* texts[2] = {up, down};
  for (int i = 0; i < 2; i++) {
    paintFelt(xs[i], ys[i], xs[i] + SIDE_HINT_W - 1, ys[i] + SIDE_HINT_H - 1);
    if (!texts[i][0]) continue;
    renderer.drawRect(xs[i], ys[i], SIDE_HINT_W, SIDE_HINT_H, 1, false);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, texts[i]);
    const int th = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(xs[i] + 1, ys[i] + 1, SIDE_HINT_W - 2, SIDE_HINT_H - 2, true);
    renderer.drawTextRotated90CW(SMALL_FONT_ID, xs[i] + (SIDE_HINT_W - th) / 2, ys[i] + (SIDE_HINT_H + tw) / 2,
                                 texts[i], false);
  }
}

void BlackjackActivity::drawHelp() const {
  static constexpr StrId LINES[] = {StrId::STR_BJ_HELP1, StrId::STR_BJ_HELP2, StrId::STR_BJ_HELP3,
                                    StrId::STR_BJ_HELP4, StrId::STR_BJ_HELP5};
  const int width = renderer.getScreenWidth();
  const int boxX = 24, boxW = width - 48, boxY = 120;
  constexpr int LINE_H = 58;
  const int boxH = 70 + LINE_H * static_cast<int>(sizeof(LINES) / sizeof(LINES[0]));
  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, 12, Color::White);
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
