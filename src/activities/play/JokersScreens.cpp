// Noir Jokers drawing shared by every screen, plus the round and the score
// reveal. Laid out for a 528x792 portrait panel in 1-bit ink only.
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "JokersActivity.h"
#include "JokersLayout.h"
#include "JokersText.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

using namespace jokers;
using namespace jokers::layout;
namespace art = jokers::art;

// ---- helpers ----------------------------------------------------------------

void JokersActivity::clearBand(const int y0, const int y1) const {
  renderer.fillRect(0, y0, renderer.getScreenWidth(), y1 - y0, false);
}

int JokersActivity::jokerX(const int index) const {
  return (renderer.getScreenWidth() - (MAX_JOKERS - 1) * JOKER_STEP - art::JOKER_W) / 2 + index * JOKER_STEP;
}

int JokersActivity::handCardX(const int index) const {
  constexpr int PER_ROW = MAX_HAND / 2;
  return (renderer.getScreenWidth() - (PER_ROW - 1) * HAND_STEP - art::CARD_W) / 2 + (index % PER_ROW) * HAND_STEP;
}

int JokersActivity::handCardY(const int index) const {
  return HAND_Y + (index / (MAX_HAND / 2)) * (art::CARD_H + HAND_ROW_GAP);
}

void JokersActivity::textCenter(const int font, const int cx, const int y, const char* s, const bool bold,
                                const bool black) const {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(font, cx - renderer.getTextWidth(font, s, style) / 2, y, s, black, style);
}

void JokersActivity::textRight(const int font, const int right, const int y, const char* s, const bool bold,
                               const bool black) const {
  const auto style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  renderer.drawText(font, right - renderer.getTextWidth(font, s, style), y, s, black, style);
}

// A rounded label centred on cx: black with white text, or outlined.
void JokersActivity::pill(const int cx, const int y, const char* s, const bool dark, const int minWidth) const {
  const int w = std::max(minWidth, renderer.getTextWidth(UI_10_FONT_ID, s, EpdFontFamily::BOLD) + 16);
  renderer.fillRoundedRect(cx - w / 2, y, w, PILL_H, 8, dark ? Color::Black : Color::White);
  renderer.drawRoundedRect(cx - w / 2, y, w, PILL_H, 1, 8, true);
  textCenter(UI_10_FONT_ID, cx, y + 2, s, true, !dark);
}

void JokersActivity::ring(const int x, const int y, const int w, const int h, const int thickness) const {
  const int pad = 1 + thickness;
  renderer.drawRoundedRect(x - pad, y - pad, w + 2 * pad, h + 2 * pad, thickness, 12, true);
}

void JokersActivity::drawButton(const int x, const int y, const int w, const int h, const char* label,
                                const bool focused, const bool enabled, const int badge) const {
  renderer.fillRoundedRect(x, y, w, h, 10, focused ? Color::Black : Color::White);
  renderer.drawRoundedRect(x, y, w, h, enabled || focused ? 3 : 1, 10, true);
  const int badgeW = badge >= 0 ? h - 6 : 0;
  const bool fits = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD) <= w - badgeW - 12;
  const int font = fits ? UI_12_FONT_ID : UI_10_FONT_ID;
  textCenter(font, x + (w - badgeW) / 2, y + (h - renderer.getLineHeight(font)) / 2, label, enabled, !focused);
  if (badge < 0) return;
  char text[8];
  snprintf(text, sizeof(text), "%d", badge);
  const int d = h - 14, bx = x + w - d - 8;
  renderer.fillRoundedRect(bx, y + 7, d, d, d / 2, focused ? Color::White : Color::Black);
  textCenter(UI_12_FONT_ID, bx + d / 2, y + 7 + (d - renderer.getLineHeight(UI_12_FONT_ID)) / 2, text, true, focused);
}

// Black band: title and subtitle on the left, money in display numerals on the right.
void JokersActivity::drawTitleBand(const char* title, const char* subtitle) const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_H, true);
  char money[16];
  snprintf(money, sizeof(money), "$%ld", static_cast<long>(run.money));
  const int moneyW = art::bigTextWidth(renderer, pack, money);
  art::drawBigText(renderer, pack, money, width - SIDE - moneyW, (HEADER_H - art::bigTextHeight(pack)) / 2, true);
  const int textW = width - 3 * SIDE - moneyW;
  renderer.drawText(UI_12_FONT_ID, SIDE, 7, renderer.truncatedText(UI_12_FONT_ID, title, textW).c_str(), false,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, SIDE, 37, renderer.truncatedText(UI_10_FONT_ID, subtitle, textW).c_str(), false);
}

// The blind's band, then the round score against the target with a progress bar.
void JokersActivity::drawHeader(const bool showScore, const int32_t shownScore) const {
  char line[64], ante[24], reward[24];
  snprintf(ante, sizeof(ante), tr(STR_JK_ANTE), run.ante, FINAL_ANTE);
  snprintf(reward, sizeof(reward), tr(STR_JK_REWARD), blindReward(run.blind));
  if (run.blind == 2) {
    snprintf(line, sizeof(line), "%s", text::bossDesc(run.boss));
  } else {
    snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", ante, reward);
  }
  drawTitleBand(text::blindName(run.blind, run.boss), line);
  if (!showScore) return;

  const int width = renderer.getScreenWidth();
  clearBand(HEADER_H, SCORE_BAND_END);
  const int32_t goal = currentTarget(run);
  char number[24];
  text::number(shownScore, number, sizeof(number));
  renderer.drawText(SMALL_FONT_ID, SIDE, HEADER_H + 10, tr(STR_JK_SCORE), true, EpdFontFamily::BOLD);
  int x = SIDE + renderer.getTextWidth(SMALL_FONT_ID, tr(STR_JK_SCORE), EpdFontFamily::BOLD) + 10;
  renderer.drawText(UI_12_FONT_ID, x, HEADER_H + 4, number, true, EpdFontFamily::BOLD);
  x += renderer.getTextWidth(UI_12_FONT_ID, number, EpdFontFamily::BOLD) + 8;
  text::number(goal, number, sizeof(number));
  snprintf(line, sizeof(line), "/ %s", number);
  renderer.drawText(UI_12_FONT_ID, x, HEADER_H + 4, line);
  textRight(UI_10_FONT_ID, width - SIDE, HEADER_H + 7, run.blind < 2 ? ante : reward, true);
  const int barY = SCORE_BAND_END - 14, barW = width - 2 * SIDE;
  renderer.drawRect(SIDE, barY, barW, 10, 1, true);
  const int filled = shownScore >= goal ? barW : static_cast<int>(static_cast<int64_t>(shownScore) * barW / goal);
  renderer.fillRect(SIDE, barY, filled, 10, true);
}

// Jokers in firing order; `fx` adds the reveal's trigger labels.
void JokersActivity::drawJokerRow(const int y, const bool focused, const Breakdown* fx) const {
  for (int i = 0; i < MAX_JOKERS; i++) {
    const int x = jokerX(i);
    if (i >= run.jokerCount) {
      renderer.drawRoundedRect(x + 4, y + 4, art::JOKER_W - 8, art::JOKER_H - 8, 1, 8, true);
      continue;
    }
    art::drawJoker(renderer, pack, run.jokers[i], x, y);
    if (focused && cursor[ROW_TOP] == i) ring(x, y, art::JOKER_W, art::JOKER_H, grabbed ? 6 : RING);
    if (!fx || fx->fx[i].kind == Fx::NONE) continue;
    char label[24], value[16];
    const JokerFx& f = fx->fx[i];
    if (f.kind == Fx::XMULT) {
      formatMult(f.value, value, sizeof(value));
      snprintf(label, sizeof(label), "x%s %s", value, tr(STR_JK_MULT_SHORT));
    } else {
      snprintf(label, sizeof(label), "+%ld %s", static_cast<long>(f.value),
               f.kind == Fx::CHIPS ? tr(STR_JK_CHIPS_SHORT) : tr(STR_JK_MULT_SHORT));
    }
    pill(x + art::JOKER_W / 2, y + art::JOKER_H + 4, label, true, art::JOKER_W);
  }
}

void JokersActivity::formatMult(const int32_t mult10, char* out, const size_t size) {
  if (mult10 % 10 == 0) {
    snprintf(out, size, "%ld", static_cast<long>(mult10 / 10));
  } else {
    snprintf(out, size, "%ld.%ld", static_cast<long>(mult10 / 10), static_cast<long>(mult10 % 10));
  }
}

// [ chips ] x [ mult ] in display numerals, with "= score" beneath when score >= 0.
void JokersActivity::drawEquation(const int y, const int32_t chips, const int32_t mult10, const int32_t score) const {
  const int width = renderer.getScreenWidth();
  char c[24], m[24];
  text::number(chips, c, sizeof(c));
  formatMult(mult10, m, sizeof(m));
  const int h = art::bigTextHeight(pack) + 12;
  const int cw = art::bigTextWidth(renderer, pack, c) + 28, mw = art::bigTextWidth(renderer, pack, m) + 28;
  const int xw = art::bigTextWidth(renderer, pack, "x") + 16;
  int x = (width - cw - xw - mw) / 2;
  renderer.fillRoundedRect(x, y, cw, h, 12, Color::Black);
  art::drawBigText(renderer, pack, c, x + 14, y + 6, true);
  renderer.drawText(SMALL_FONT_ID, x + 10, y - 20, tr(STR_JK_CHIPS_SHORT), true, EpdFontFamily::BOLD);
  x += cw;
  art::drawBigText(renderer, pack, "x", x + 8, y + 6);
  x += xw;
  renderer.drawRoundedRect(x, y, mw, h, 4, 12, true);
  art::drawBigText(renderer, pack, m, x + 14, y + 6);
  textRight(SMALL_FONT_ID, x + mw - 10, y - 20, tr(STR_JK_MULT_SHORT), true);
  if (score < 0) return;
  char s[32];
  text::number(score, c, sizeof(c));
  snprintf(s, sizeof(s), "= %s", c);
  const int sw = art::bigTextWidth(renderer, pack, s);
  const int sy = y + h + 12;
  renderer.drawRoundedRect((width - sw) / 2 - 20, sy, sw + 40, h, 3, 12, true);
  renderer.drawRoundedRect((width - sw) / 2 - 15, sy + 5, sw + 30, h - 10, 1, 9, true);
  art::drawBigText(renderer, pack, s, (width - sw) / 2, sy + 6);
}

// Labels sit above the physical buttons they describe (mapLabels follows any
// remapping); the side hints name the row each side button moves to.
void JokersActivity::drawHints() const {
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  const int hintsH = UITheme::getInstance().getMetrics().buttonHintsHeight;
  renderer.fillRect(0, height - hintsH, width, hintsH, false);
  if (screen == Screen::ROUND) {
    // The round redraws in bands, so old side hints must be wiped; other screens start blank.
    renderer.fillRect(0, SIDE_HINT_Y, SIDE_HINT_W, SIDE_HINT_H, false);
    renderer.fillRect(width - SIDE_HINT_W, SIDE_HINT_Y, SIDE_HINT_W, SIDE_HINT_H, false);
  }
  static constexpr StrId ACTIONS[3] = {StrId::STR_JK_PLAY, StrId::STR_JK_DISCARD, StrId::STR_JK_SORT};
  const char* back = tr(STR_JK_MENU);
  const char* confirm = "";
  bool arrows = true;
  const char* up = "";
  const char* down = "";
  switch (screen) {
    case Screen::TITLE:
      back = tr(STR_EXIT);
      confirm = tr(STR_JK_SELECT);
      break;
    case Screen::ROUND:
      if (row == ROW_TOP) {
        confirm = grabbed ? tr(STR_JK_PLACE) : tr(STR_JK_MOVE);
      } else if (row == ROW_MID) {
        confirm = (selected >> cursor[ROW_MID]) & 1u ? tr(STR_JK_UNSELECT) : tr(STR_JK_SELECT);
      } else {
        confirm = I18N.get(ACTIONS[cursor[ROW_BOTTOM] % 3]);
      }
      up = row == ROW_MID ? (run.jokerCount ? tr(STR_JK_JOKERS) : "") : (row == ROW_BOTTOM ? tr(STR_JK_HAND) : "");
      down = row == ROW_TOP ? tr(STR_JK_HAND) : (row == ROW_MID ? tr(STR_JK_ACTIONS) : "");
      break;
    case Screen::SHOP:
      if (row == ROW_TOP) {
        confirm = run.shop[cursor[ROW_TOP]] == EMPTY ? "" : tr(STR_JK_BUY);
      } else if (row == ROW_MID) {
        confirm = tr(STR_JK_SELL);
      } else {
        confirm = cursor[ROW_BOTTOM] ? tr(STR_JK_NEXT_BLIND) : tr(STR_JK_REROLL_SHORT);
      }
      up = row == ROW_TOP ? "" : (row == ROW_BOTTOM && run.jokerCount ? tr(STR_JK_JOKERS) : tr(STR_JK_OFFERS));
      down = row == ROW_BOTTOM ? "" : (row == ROW_TOP && run.jokerCount ? tr(STR_JK_JOKERS) : tr(STR_JK_ACTIONS));
      break;
    case Screen::MENU:
      back = tr(STR_BACK);
      confirm = tr(STR_JK_SELECT);
      break;
    case Screen::HELP:
      back = tr(STR_BACK);
      confirm = helpPage + 1 < HELP_PAGES ? "\xE2\x80\xBA" : tr(STR_JK_CONTINUE);
      break;
    case Screen::BLIND:
      arrows = false;
      confirm = tr(STR_JK_PLAY);
      break;
    case Screen::CASH_OUT:
      arrows = false;
      confirm = run.phase == Phase::OVER ? tr(STR_JK_CONTINUE) : tr(STR_JK_SHOP);
      break;
    case Screen::LEVELS:
      arrows = false;
      back = tr(STR_BACK);
      confirm = tr(STR_JK_CONTINUE);
      break;
    case Screen::OVER:
      arrows = false;
      back = tr(STR_JK_TITLE_MENU);
      confirm = tr(STR_JK_NEW_RUN);
      break;
    case Screen::REVEAL:
      arrows = false;
      back = "";
      confirm = revealStep + 1 < REVEAL_STEPS ? tr(STR_JK_SKIP) : tr(STR_JK_CONTINUE);
      break;
  }
  // Single guillemets: the UI font has no arrow glyphs.
  const auto labels = mappedInput.mapLabels(back, confirm, arrows ? "\xE2\x80\xB9" : "", arrows ? "\xE2\x80\xBA" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, up, down);
}

// ---- round ------------------------------------------------------------------------

void JokersActivity::drawRoundInfo() const {
  const int width = renderer.getScreenWidth();
  char line[96], number[24];
  if (row == ROW_TOP && run.jokerCount > 0) {
    const int i = cursor[ROW_TOP];
    const uint8_t joker = run.jokers[i];
    snprintf(line, sizeof(line), tr(STR_JK_SELL_FOR), jokerSellValue(joker));
    textRight(UI_10_FONT_ID, width - SIDE, INFO_Y + 6, line, false);
    renderer.drawText(UI_12_FONT_ID, SIDE, INFO_Y + 2, text::jokerName(joker), true, EpdFontFamily::BOLD);
    text::jokerDesc(joker, run.jokerCounter[i], line, sizeof(line));
    renderer.drawText(UI_10_FONT_ID, SIDE, INFO_Y + 38,
                      renderer.truncatedText(UI_10_FONT_ID, line, width - 2 * SIDE).c_str());
    return;
  }
  text::number(std::max<int32_t>(0, currentTarget(run) - run.roundScore), number, sizeof(number));
  snprintf(line, sizeof(line), tr(STR_JK_NEED), number);
  textRight(UI_10_FONT_ID, width - SIDE, INFO_Y + 38, line, false);
  if (selected == 0) {
    renderer.drawText(UI_12_FONT_ID, SIDE, INFO_Y + 2, tr(STR_JK_PICK), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, SIDE, INFO_Y + 38, tr(STR_JK_HOLD_TIP));
    return;
  }
  Breakdown preview;
  scoreHand(run, selected, preview);
  const char* name = text::handName(preview.type);
  renderer.drawText(UI_12_FONT_ID, SIDE, INFO_Y + 2, name, true, EpdFontFamily::BOLD);
  snprintf(line, sizeof(line), tr(STR_JK_LEVEL), run.levels[preview.type]);
  renderer.drawText(UI_10_FONT_ID, SIDE + renderer.getTextWidth(UI_12_FONT_ID, name, EpdFontFamily::BOLD) + 8,
                    INFO_Y + 6, line);
  text::number(preview.score, number, sizeof(number));
  snprintf(line, sizeof(line), tr(STR_JK_EST), number);
  renderer.drawText(UI_10_FONT_ID, SIDE, INFO_Y + 38, line, true, EpdFontFamily::BOLD);
  // The hand level's base Chips x Mult, as in the reveal's first step.
  drawMiniEquation(width - SIDE, INFO_Y, preview.baseChips, preview.baseMult);
}

// Small right-aligned [chips] x [mult] boxes in the UI font.
void JokersActivity::drawMiniEquation(const int right, const int y, const int32_t chips, const int32_t mult) const {
  char a[16], b[16];
  snprintf(a, sizeof(a), "%ld", static_cast<long>(chips));
  snprintf(b, sizeof(b), "%ld", static_cast<long>(mult));
  const int aw = std::max(56, renderer.getTextWidth(UI_12_FONT_ID, a, EpdFontFamily::BOLD) + 20);
  const int bw = std::max(40, renderer.getTextWidth(UI_12_FONT_ID, b, EpdFontFamily::BOLD) + 20);
  constexpr int BOX_H = 32, TIMES_W = 24;
  int x = right - bw - TIMES_W - aw;
  renderer.fillRoundedRect(x, y, aw, BOX_H, 8, Color::Black);
  textCenter(UI_12_FONT_ID, x + aw / 2, y + 3, a, true, false);
  textCenter(UI_12_FONT_ID, x + aw + TIMES_W / 2, y + 3, "x", true);
  x += aw + TIMES_W;
  renderer.drawRoundedRect(x, y, bw, BOX_H, 3, 8, true);
  textCenter(UI_12_FONT_ID, x + bw / 2, y + 3, b, true);
}

void JokersActivity::drawHand() const {
  for (int i = 0; i < run.handCount; i++) {
    const bool lifted = (selected >> i) & 1u;
    const int x = handCardX(i), baseY = handCardY(i);
    const int y = lifted ? baseY - LIFT : baseY;
    art::drawCard(renderer, pack, run.hand[i], x, y);
    // A lifted card leaves a black "shadow" bar where it sat.
    if (lifted) renderer.fillRoundedRect(x + 12, baseY + art::CARD_H - 8, art::CARD_W - 24, 8, 3, Color::Black);
    if (isDebuffed(run, run.hand[i])) {
      const int tx = x + art::CARD_W - 30, ty = y + 6;
      renderer.fillRoundedRect(tx, ty, 24, 24, 4, Color::Black);
      renderer.drawLine(tx + 6, ty + 6, tx + 17, ty + 17, 3, false);
      renderer.drawLine(tx + 17, ty + 6, tx + 6, ty + 17, 3, false);
    }
    if (row == ROW_MID && cursor[ROW_MID] == i) ring(x, y, art::CARD_W, art::CARD_H, RING + 1);
  }
}

void JokersActivity::drawActions() const {
  const int width = renderer.getScreenWidth();
  constexpr int GAP = 10, SORT_W = 132;
  const int bigW = (width - 2 * SIDE - SORT_W - 2 * GAP) / 2;
  const bool focused = row == ROW_BOTTOM;
  const int c = cursor[ROW_BOTTOM];
  drawButton(SIDE, ACTIONS_Y, bigW, ACTIONS_H, tr(STR_JK_PLAY), focused && c == 0, selected != 0, run.handsLeft);
  drawButton(SIDE + bigW + GAP, ACTIONS_Y, bigW, ACTIONS_H, tr(STR_JK_DISCARD), focused && c == 1,
             selected != 0 && run.discardsLeft > 0, run.discardsLeft);
  char sort[32];
  snprintf(sort, sizeof(sort), "%s: %s", tr(STR_JK_SORT), run.sortBySuit ? tr(STR_JK_SORT_SUIT) : tr(STR_JK_SORT_RANK));
  drawButton(width - SIDE - SORT_W, ACTIONS_Y, SORT_W, ACTIONS_H, sort, focused && c == 2, true, -1);
}

void JokersActivity::drawFooter() const {
  if (toast) {
    renderer.drawCenteredText(UI_10_FONT_ID, FOOTER_Y, toast, true, EpdFontFamily::BOLD);
    return;
  }
  int count = 0;
  for (uint8_t v = selected; v; v &= v - 1) count++;
  char line[48];
  snprintf(line, sizeof(line), tr(STR_JK_SELECTED), count);
  renderer.drawText(UI_10_FONT_ID, SIDE, FOOTER_Y, line);
  snprintf(line, sizeof(line), tr(STR_JK_DECK), DECK_SIZE - run.deckPos, DECK_SIZE);
  textRight(UI_10_FONT_ID, renderer.getScreenWidth() - SIDE, FOOTER_Y, line, false);
}

// Only the bands marked dirty are cleared and redrawn.
void JokersActivity::drawRound() {
  if (dirty & D_HEADER) drawHeader(true, run.roundScore);
  if (dirty & D_JOKERS) {
    clearBand(JOKER_Y - 8, INFO_Y - 2);
    drawJokerRow(JOKER_Y, row == ROW_TOP, nullptr);
  }
  if (dirty & D_INFO) {
    clearBand(INFO_Y - 2, HAND_Y - LIFT - 6);
    drawRoundInfo();
  }
  if (dirty & D_HAND) {
    clearBand(HAND_Y - LIFT - 6, ACTIONS_Y - 4);
    drawHand();
  }
  if (dirty & D_ACTIONS) {
    clearBand(ACTIONS_Y - 4, FOOTER_Y - 2);
    drawActions();
  }
  if (dirty & D_FOOTER) {
    clearBand(FOOTER_Y - 2, renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight);
    drawFooter();
  }
  if (dirty & (D_HINTS | D_JOKERS | D_INFO)) drawHints();
}

// ---- reveal -----------------------------------------------------------------------

// Step 0: the played cards and their chips. Step 1: jokers fire, final Chips x
// Mult. Step 2: the total lands in the round score.
void JokersActivity::drawReveal() const {
  const int width = renderer.getScreenWidth();
  const Breakdown& b = reveal;
  drawHeader(true, revealStep >= 2 ? run.roundScore : scoreBefore);
  drawJokerRow(JOKER_Y, false, revealStep >= 1 ? &b : nullptr);
  char line[48];
  snprintf(line, sizeof(line), tr(STR_JK_LEVEL), run.levels[b.type]);
  const char* name = text::handName(b.type);
  const int nw = renderer.getTextWidth(UI_12_FONT_ID, name, EpdFontFamily::BOLD);
  const int nx = (width - nw - 10 - renderer.getTextWidth(UI_10_FONT_ID, line)) / 2;
  renderer.drawText(UI_12_FONT_ID, nx, REVEAL_NAME_Y, name, true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, nx + nw + 10, REVEAL_NAME_Y + 4, line);

  const int n = b.count;
  const int step = n > 1 ? std::min(HAND_STEP, (width - 2 * SIDE - art::CARD_W) / (n - 1)) : 0;
  const int x0 = (width - art::CARD_W - step * (n - 1)) / 2;
  for (int i = 0; i < n; i++) {
    const bool scores = (b.scoringMask >> i) & 1u;
    art::drawCard(renderer, pack, b.played[i], x0 + i * step, scores ? REVEAL_CARDS_Y - LIFT : REVEAL_CARDS_Y);
  }
  for (int i = 0; i < n; i++) {
    if (!((b.scoringMask >> i) & 1u)) continue;
    const bool debuffed = (b.debuffedMask >> i) & 1u;
    char chips[16];
    snprintf(chips, sizeof(chips), "+%d", b.cardChips[i]);
    const int visible = i + 1 < n ? std::min(step, art::CARD_W) : art::CARD_W;
    pill(x0 + i * step + visible / 2, REVEAL_CARDS_Y + art::CARD_H + 4, debuffed ? tr(STR_JK_DEBUFFED) : chips,
         !debuffed, 0);
  }
  const int eqY = REVEAL_CARDS_Y + art::CARD_H + 50;
  if (revealStep == 0) {
    drawEquation(eqY, b.cardsChips, b.cardsMult10, -1);
  } else {
    drawEquation(eqY, b.chips, b.mult10, revealStep >= 2 ? b.score : -1);
  }
  if (revealStep >= 2) {
    // Where the round stands now.
    const int y = eqY + 2 * (art::bigTextHeight(pack) + 12) + 18;
    if (roundWon(run)) {
      const char* won = tr(STR_JK_BLIND_DEFEATED);
      const int w = renderer.getTextWidth(UI_12_FONT_ID, won, EpdFontFamily::BOLD) + 40;
      renderer.fillRoundedRect((width - w) / 2, y, w, 40, 10, Color::Black);
      textCenter(UI_12_FONT_ID, width / 2, y + 7, won, true, false);
    } else {
      char need[24];
      text::number(std::max<int32_t>(0, currentTarget(run) - run.roundScore), need, sizeof(need));
      snprintf(line, sizeof(line), tr(STR_JK_NEED), need);
      textCenter(UI_12_FONT_ID, width / 2, y + 7, line, true);
    }
  }
  drawHints();
}

// ---- frame --------------------------------------------------------------------

void JokersActivity::drawScreen(const Screen which) {
  switch (which) {
    case Screen::TITLE:
      drawTitle();
      break;
    case Screen::BLIND:
      drawBlindSelect();
      break;
    case Screen::ROUND:
      drawRound();
      break;
    case Screen::REVEAL:
      drawReveal();
      break;
    case Screen::CASH_OUT:
      drawCashOut();
      break;
    case Screen::SHOP:
      drawShop();
      break;
    case Screen::OVER:
      drawOver();
      break;
    case Screen::HELP:
      drawHelp();
      break;
    case Screen::LEVELS:
      drawLevels();
      break;
    case Screen::MENU:
      break;
  }
}

void JokersActivity::render(RenderLock&& lock) {
  const bool clean = cleanRefresh;
  // The round and the menu overlay redraw only what changed; other screens are cheap to redraw whole.
  const bool full = redrawAll || (screen != Screen::ROUND && screen != Screen::MENU);
  if (full) {
    renderer.clearScreen();
    dirty = D_ALL;
  }
  if (screen == Screen::MENU) {
    if (redrawAll) drawScreen(underlay);
    drawMenu();
  } else {
    drawScreen(screen);
  }
  dirty = 0;
  redrawAll = false;
  cleanRefresh = false;
  const HalDisplay::RefreshMode mode = pacer.next(clean);
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
