// Noir Jokers full-page screens: title, blind select, cash out, shop, run
// summary, menu, how to play and poker hand levels.
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "JokersActivity.h"
#include "JokersLayout.h"
#include "JokersText.h"
#include "MappedInputManager.h"
#include "fontIds.h"

using namespace jokers;
using namespace jokers::layout;
namespace art = jokers::art;

namespace {
constexpr int STAT_X = 48;
// King of hearts, king of spades, four of clubs: a pair for the help page.
constexpr uint8_t HELP_CARDS[3] = {24, 11, 28};
}  // namespace

// "Label ........ value" with a dotted leader.
void JokersActivity::statLine(const int y, const char* label, const char* value) const {
  const int width = renderer.getScreenWidth();
  renderer.drawText(UI_12_FONT_ID, STAT_X, y, label);
  textRight(UI_12_FONT_ID, width - STAT_X, y, value, true);
  for (int x = STAT_X; x < width - STAT_X; x += 8) renderer.fillRect(x, y + 34, 3, 2, true);
}

void JokersActivity::drawTitle() const {
  const int width = renderer.getScreenWidth();
  if (!pack.draw(renderer, art::TITLE, (width - art::TITLE_W) / 2, 24)) {
    renderer.fillRect(0, 24, width, art::TITLE_H, true);
    textCenter(NOTOSANS_18_FONT_ID, width / 2, 24 + art::TITLE_H / 2 - 20, tr(STR_PLAY_JOKERS), true, false);
  }
  static constexpr StrId ITEMS[3] = {StrId::STR_JK_CONTINUE_RUN, StrId::STR_JK_NEW_RUN, StrId::STR_JK_HOW_TO};
  const int first = hasRun ? 0 : 1;
  constexpr int BW = 360, BH = 56;
  int y = 24 + art::TITLE_H + 40;
  for (int i = first; i < 3; i++, y += BH + 16) {
    const char* label = i == 1 && armed == 1 ? tr(STR_JK_ABANDON_CONFIRM) : I18N.get(ITEMS[i]);
    drawButton((width - BW) / 2, y, BW, BH, label, menuCursor == i - first, true, -1);
  }
  // Lifetime records as three tiles.
  static constexpr StrId LABELS[3] = {StrId::STR_JK_BEST_ANTE, StrId::STR_JK_RUNS_WON, StrId::STR_JK_BEST_HAND};
  char values[3][24];
  snprintf(values[0], sizeof(values[0]), "%ld", static_cast<long>(stats.bestAnte));
  snprintf(values[1], sizeof(values[1]), "%ld / %ld", static_cast<long>(stats.runsWon),
           static_cast<long>(stats.runsPlayed));
  text::number(stats.bestHand, values[2], sizeof(values[2]));
  constexpr int TILE_W = 150, TILE_H = 84, TILE_GAP = 12;
  y += 20;
  for (int i = 0; i < 3; i++) {
    const int tx = (width - 3 * TILE_W - 2 * TILE_GAP) / 2 + i * (TILE_W + TILE_GAP);
    renderer.drawRoundedRect(tx, y, TILE_W, TILE_H, 2, 10, true);
    textCenter(SMALL_FONT_ID, tx + TILE_W / 2, y + 10, I18N.get(LABELS[i]), false);
    textCenter(UI_12_FONT_ID, tx + TILE_W / 2, y + 40, values[i], true);
  }
  drawHints();
}

void JokersActivity::drawBlindSelect() const {
  const int width = renderer.getScreenWidth();
  char line[64];
  char ante[24];
  snprintf(ante, sizeof(ante), tr(STR_JK_ANTE), run.ante, FINAL_ANTE);
  snprintf(line, sizeof(line), tr(STR_JK_BOSS_AHEAD), text::bossName(run.boss));
  drawTitleBand(ante, line);
  constexpr int COL_W = 160, COL_Y = HEADER_H + 20;
  for (int b = 0; b < 3; b++) {
    const int cx = width / 2 + (b - 1) * (COL_W + 8);
    if (b == run.blind) renderer.drawRoundedRect(cx - COL_W / 2, COL_Y, COL_W, 212, 3, 12, true);
    const std::string name =
        renderer.truncatedText(UI_10_FONT_ID, text::blindName(b, run.boss), COL_W - 12, EpdFontFamily::BOLD);
    textCenter(UI_10_FONT_ID, cx, COL_Y + 10, name.c_str(), true);
    art::drawBlindChip(renderer, pack, b, run.boss, cx - art::CHIP / 2, COL_Y + 38);
    text::number(blindTarget(run.ante, b, b == 2 && run.boss == SYNDICATE), line, sizeof(line));
    textCenter(UI_12_FONT_ID, cx, COL_Y + 142, line, true);
    snprintf(line, sizeof(line), tr(STR_JK_REWARD), blindReward(b));
    textCenter(UI_10_FONT_ID, cx, COL_Y + 174, line, false);
    if (b < run.blind) pill(cx, COL_Y + 74, tr(STR_JK_DEFEATED), true, 0);
  }

  int y = COL_Y + 240;
  textCenter(UI_10_FONT_ID, width / 2, y, tr(STR_JK_UP_NEXT), false);
  textCenter(UI_12_FONT_ID, width / 2, y + 26, text::blindName(run.blind, run.boss), true);
  y += 66;
  textCenter(UI_10_FONT_ID, width / 2, y, tr(STR_JK_TARGET), false);
  text::number(currentTarget(run), line, sizeof(line));
  art::drawBigText(renderer, pack, line, (width - art::bigTextWidth(renderer, pack, line)) / 2, y + 24);
  y += 24 + art::bigTextHeight(pack) + 14;
  if (run.blind == 2) {
    const char* effect = text::bossDesc(run.boss);
    const int ew = renderer.getTextWidth(UI_12_FONT_ID, effect, EpdFontFamily::BOLD) + 40;
    renderer.fillRoundedRect((width - ew) / 2, y, ew, 40, 10, Color::Black);
    textCenter(UI_12_FONT_ID, width / 2, y + 7, effect, true, false);
  }
  y += 56;
  const int hands = START_HANDS - (bossActive(run, MISER) ? 1 : 0);
  const int discards = bossActive(run, MUTE) ? 0 : START_DISCARDS;
  snprintf(line, sizeof(line), tr(STR_JK_ROUND_RULES), hands, discards, handSize(run));
  textCenter(UI_10_FONT_ID, width / 2, y, line, false);
  drawJokerRow(y + 36, false, nullptr);
  drawHints();
}

void JokersActivity::drawCashOut() const {
  const int width = renderer.getScreenWidth();
  drawHeader(false, 0);
  art::drawBlindChip(renderer, pack, run.blind, run.boss, (width - art::CHIP) / 2, HEADER_H + 24);
  textCenter(UI_12_FONT_ID, width / 2, HEADER_H + 132, tr(STR_JK_BLIND_DEFEATED), true);
  int y = HEADER_H + 190;
  char label[48], value[16];
  const auto line = [&](const char* text, const int32_t amount) {
    snprintf(value, sizeof(value), "$%ld", static_cast<long>(amount));
    statLine(y, text, value);
    y += 52;
  };
  line(tr(STR_JK_PAY_BLIND), payout.blind);
  snprintf(label, sizeof(label), tr(STR_JK_PAY_HANDS), payout.hands);
  line(label, payout.hands);
  line(tr(STR_JK_PAY_INTEREST), payout.interest);
  if (payout.jokers > 0) line(tr(STR_JK_PAY_JOKERS), payout.jokers);
  y += 12;
  renderer.drawText(UI_12_FONT_ID, STAT_X, y + 16, tr(STR_JK_TOTAL), true, EpdFontFamily::BOLD);
  snprintf(value, sizeof(value), "$%ld", static_cast<long>(payout.total()));
  art::drawBigText(renderer, pack, value, width - STAT_X - art::bigTextWidth(renderer, pack, value), y);
  drawHints();
}

// Offers (two jokers and a planet) with prices, details of the highlighted
// item, owned jokers with sell values, then Reroll / Next blind.
void JokersActivity::drawShop() const {
  const int width = renderer.getScreenWidth();
  char line[128];
  char ante[24];
  snprintf(ante, sizeof(ante), tr(STR_JK_ANTE), run.ante, FINAL_ANTE);
  drawTitleBand(tr(STR_JK_SHOP), ante);

  const int offerY = HEADER_H + 22;
  for (int s = 0; s < SHOP_SLOTS; s++) {
    const int cx = width / 2 + (s - 1) * 168, x = cx - art::JOKER_W / 2;
    const uint8_t item = run.shop[s];
    if (item == EMPTY) {
      renderer.drawRoundedRect(x + 4, offerY + 4, art::JOKER_W - 8, art::JOKER_H - 8, 1, 8, true);
      textCenter(UI_10_FONT_ID, cx, offerY + art::JOKER_H / 2 - 10, tr(STR_JK_SOLD_OUT), false);
    } else if (s < SHOP_JOKERS) {
      art::drawJoker(renderer, pack, item, x, offerY);
    } else {
      renderer.drawRoundedRect(x, offerY, art::JOKER_W, art::JOKER_H, 2, 8, true);
      art::drawPlanet(renderer, pack, item, x + (art::JOKER_W - art::PLANET) / 2, offerY + 8);
      textCenter(SMALL_FONT_ID, cx, offerY + art::JOKER_H - 26, text::planetName(item), true);
    }
    if (item != EMPTY) {
      snprintf(line, sizeof(line), "$%d", shopPrice(run, s));
      pill(cx, offerY + art::JOKER_H + 10, line, run.money >= shopPrice(run, s), 56);
    }
    if (row == ROW_TOP && cursor[ROW_TOP] == s) ring(x, offerY, art::JOKER_W, art::JOKER_H, RING + 1);
  }

  // Details of whatever is highlighted.
  const int panelY = offerY + art::JOKER_H + 52, panelH = 96;
  renderer.drawRoundedRect(SIDE, panelY, width - 2 * SIDE, panelH, 2, 10, true);
  const char* title = nullptr;
  line[0] = '\0';
  const int c = cursor[row];
  if (row == ROW_TOP && run.shop[c] != EMPTY) {
    const uint8_t item = run.shop[c];
    if (c < SHOP_JOKERS) {
      title = text::jokerName(item);
      text::jokerDesc(item, 0, line, sizeof(line));
    } else {
      title = text::planetName(item);
      const Level now = handLevelValues(static_cast<HandType>(item), run.levels[item]);
      const Level next = handLevelValues(static_cast<HandType>(item), run.levels[item] + 1);
      char gain[64];
      snprintf(gain, sizeof(gain), tr(STR_JK_PLANET_GAIN), run.levels[item], run.levels[item] + 1,
               static_cast<int>(next.chips - now.chips), static_cast<int>(next.mult - now.mult));
      snprintf(line, sizeof(line), tr(STR_JK_PLANET_DESC), text::handName(item));
      const size_t used = strlen(line);
      snprintf(line + used, sizeof(line) - used, ". %s", gain);
    }
  } else if (row == ROW_MID && c < run.jokerCount) {
    title = text::jokerName(run.jokers[c]);
    text::jokerDesc(run.jokers[c], run.jokerCounter[c], line, sizeof(line));
  } else if (row == ROW_BOTTOM) {
    title = c == 0 ? tr(STR_JK_REROLL_SHORT) : tr(STR_JK_NEXT_BLIND);
    snprintf(line, sizeof(line), "%s", c == 0 ? tr(STR_JK_REROLL_DESC) : tr(STR_JK_NEXT_DESC));
  }
  if (title) renderer.drawText(UI_12_FONT_ID, SIDE + 14, panelY + 8, title, true, EpdFontFamily::BOLD);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, line, width - 2 * SIDE - 28, 2);
  for (size_t i = 0; i < lines.size(); i++) {
    renderer.drawText(UI_10_FONT_ID, SIDE + 14, panelY + 40 + static_cast<int>(i) * 24, lines[i].c_str());
  }

  const int ownY = panelY + panelH + 44;
  snprintf(line, sizeof(line), tr(STR_JK_YOUR_JOKERS), run.jokerCount, MAX_JOKERS);
  renderer.drawText(UI_10_FONT_ID, SIDE, ownY - 30, line, true, EpdFontFamily::BOLD);
  drawJokerRow(ownY, false, nullptr);
  for (int i = 0; i < run.jokerCount; i++) {
    const int x = jokerX(i);
    snprintf(line, sizeof(line), "$%d", jokerSellValue(run.jokers[i]));
    pill(x + art::JOKER_W / 2, ownY + art::JOKER_H + 8, line, armed == i, 0);
    if (row == ROW_MID && cursor[ROW_MID] == i) ring(x, ownY, art::JOKER_W, art::JOKER_H, RING + 1);
  }

  const int actY = ownY + art::JOKER_H + 50, actW = (width - 2 * SIDE - 12) / 2;
  snprintf(line, sizeof(line), tr(STR_JK_REROLL), run.rerollCost);
  drawButton(SIDE, actY, actW, 52, line, row == ROW_BOTTOM && c == 0, run.money >= run.rerollCost, -1);
  drawButton(width - SIDE - actW, actY, actW, 52, tr(STR_JK_NEXT_BLIND), row == ROW_BOTTOM && c == 1, true, -1);

  // A warning or sell prompt, else what the next blind asks for.
  const char* note = toast;
  if (!note && armed >= 0 && armed < run.jokerCount) {
    snprintf(line, sizeof(line), tr(STR_JK_SELL_CONFIRM), jokerSellValue(run.jokers[armed]));
    note = line;
  }
  int y = actY + 66;
  if (note) {
    textCenter(UI_10_FONT_ID, width / 2, y, note, true);
    drawHints();
    return;
  }
  renderer.drawLine(SIDE, y, width - SIDE, y, true);
  advanceBlindPreview(line, sizeof(line));
  for (const auto& text : renderer.wrappedText(UI_10_FONT_ID, line, width - 2 * SIDE, 2)) {
    y += 24;
    textCenter(UI_10_FONT_ID, width / 2, y - 12, text.c_str(), false);
  }
  drawHints();
}

void JokersActivity::advanceBlindPreview(char* out, const size_t size) const {
  const int nextBlind = (run.blind + 1) % 3;
  const int nextAnte = run.blind == 2 ? run.ante + 1 : run.ante;
  char target[24];
  text::number(blindTarget(nextAnte, nextBlind, nextBlind == 2 && run.boss == SYNDICATE), target, sizeof(target));
  if (nextBlind == 2) {
    snprintf(out, size, "%s: %s (%s). %s %s", tr(STR_JK_UP_NEXT), text::bossName(run.boss), text::bossDesc(run.boss),
             tr(STR_JK_TARGET), target);
  } else {
    snprintf(out, size, "%s: %s. %s %s", tr(STR_JK_UP_NEXT), text::blindName(nextBlind, run.boss), tr(STR_JK_TARGET),
             target);
  }
}

void JokersActivity::drawOver() const {
  const int width = renderer.getScreenWidth();
  char line[64], number[24];
  if (run.won) {
    snprintf(line, sizeof(line), "%s", tr(STR_JK_ALL_ANTES));
  } else {
    snprintf(line, sizeof(line), tr(STR_JK_BEATEN_BY), text::blindName(run.blind, run.boss));
  }
  drawTitleBand(run.won ? tr(STR_JK_VICTORY) : tr(STR_JK_RUN_OVER), line);
  art::drawBlindChip(renderer, pack, run.blind, run.boss, (width - art::CHIP) / 2, HEADER_H + 20);

  int y = HEADER_H + 130;
  textCenter(UI_10_FONT_ID, width / 2, y, tr(STR_JK_BEST_HAND), false);
  text::number(run.bestHand, number, sizeof(number));
  art::drawBigText(renderer, pack, number, (width - art::bigTextWidth(renderer, pack, number)) / 2, y + 24);
  y += 24 + art::bigTextHeight(pack) + 6;
  if (newRecord) pill(width / 2, y, tr(STR_JK_NEW_RECORD), true, 0);
  y += 44;
  snprintf(number, sizeof(number), "%d", run.ante);
  statLine(y, tr(STR_JK_REACHED), number);
  if (!run.won) {
    // How close the last blind came.
    char goal[24];
    text::number(run.roundScore, line, sizeof(line));
    text::number(currentTarget(run), goal, sizeof(goal));
    const size_t used = strlen(line);
    snprintf(line + used, sizeof(line) - used, " / %s", goal);
    statLine(y + 48, tr(STR_JK_LAST_BLIND), line);
    y += 48;
  }
  snprintf(number, sizeof(number), "%d", run.handsPlayed);
  statLine(y + 48, tr(STR_JK_HANDS_PLAYED), number);
  snprintf(number, sizeof(number), "%ld / %ld", static_cast<long>(stats.runsWon), static_cast<long>(stats.runsPlayed));
  statLine(y + 96, tr(STR_JK_RUNS_WON), number);
  y += 152;
  drawJokerRow(y, false, nullptr);
  text::number(stats.bestHand, number, sizeof(number));
  snprintf(line, sizeof(line), tr(STR_JK_STATS), static_cast<int>(stats.bestAnte), static_cast<int>(stats.runsWon),
           number);
  textCenter(UI_10_FONT_ID, width / 2, y + art::JOKER_H + 20, line, false);
  drawHints();
}

void JokersActivity::drawMenu() const {
  static constexpr StrId ITEMS[] = {StrId::STR_JK_RESUME, StrId::STR_JK_POKER_HANDS, StrId::STR_JK_HOW_TO,
                                    StrId::STR_JK_SAVE_QUIT, StrId::STR_JK_ABANDON};
  constexpr int COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);
  constexpr int W = 400, ITEM_H = 58, H = 30 + COUNT * ITEM_H, Y = 190;
  const int x = (renderer.getScreenWidth() - W) / 2;
  renderer.fillRoundedRect(x - 8, Y - 8, W + 16, H + 16, 16, Color::White);
  renderer.drawRoundedRect(x, Y, W, H, 3, 12, true);
  renderer.drawRoundedRect(x + 6, Y + 6, W - 12, H - 12, 1, 8, true);
  for (int i = 0; i < COUNT; i++) {
    const char* label = i == COUNT - 1 && armed == COUNT - 1 ? tr(STR_JK_ABANDON_CONFIRM) : I18N.get(ITEMS[i]);
    drawButton(x + 20, Y + 16 + i * ITEM_H, W - 40, ITEM_H - 12, label, menuCursor == i, true, -1);
  }
  drawHints();
}

void JokersActivity::drawHelp() const {
  static constexpr StrId TITLES[HELP_PAGES] = {StrId::STR_JK_HELP_T1, StrId::STR_JK_HELP_T2, StrId::STR_JK_HELP_T3,
                                               StrId::STR_JK_HELP_T4};
  static constexpr StrId LINES[HELP_PAGES][3] = {{StrId::STR_JK_HELP_1A, StrId::STR_JK_HELP_1B, StrId::STR_JK_HELP_1C},
                                                 {StrId::STR_JK_HELP_2A, StrId::STR_JK_HELP_2B, StrId::STR_JK_HELP_2C},
                                                 {StrId::STR_JK_HELP_3A, StrId::STR_JK_HELP_3B, StrId::STR_JK_HELP_3C},
                                                 {StrId::STR_JK_HELP_4A, StrId::STR_JK_HELP_4B, StrId::STR_JK_HELP_4C}};
  const int width = renderer.getScreenWidth();
  const int page = helpPage % HELP_PAGES;
  char title[48];
  snprintf(title, sizeof(title), "%s  %d/%d", tr(STR_JK_HOW_TO), page + 1, HELP_PAGES);
  drawTitleBand(title, I18N.get(TITLES[page]));

  // A small picture per page, then the text.
  int y = HEADER_H + 24;
  switch (page) {
    case 0:
      for (int b = 0; b < 3; b++) art::drawBlindChip(renderer, pack, b, WIDOW, width / 2 + (b - 1) * 130 - 48, y);
      y += art::CHIP + 30;
      break;
    case 1:
      for (int i = 0; i < 3; i++) art::drawCard(renderer, pack, HELP_CARDS[i], width / 2 - 150 + i * 92, y);
      drawEquation(y + art::CARD_H + 44, 30, 20, -1);
      y += art::CARD_H + 44 + art::bigTextHeight(pack) + 36;
      break;
    case 2:
      art::drawJoker(renderer, pack, TWIN_DETECTIVES, width / 2 - 120, y);
      art::drawJoker(renderer, pack, CLEAN_HANDS, width / 2 - 24, y);
      art::drawPlanet(renderer, pack, FLUSH, width / 2 + 80, y + 20);
      y += art::JOKER_H + 30;
      break;
    default: {
      // The four front buttons as mapped right now, under a sketch of the screen.
      constexpr int KEY_W = 56, KEY_STEP = 62, BODY_W = 4 * KEY_STEP + 26, BODY_H = 170;
      const int bx = (width - BODY_W) / 2;
      renderer.drawRoundedRect(bx, y, BODY_W, BODY_H, 2, 14, true);
      renderer.drawRect(bx + 18, y + 14, BODY_W - 36, BODY_H - 70, 1, true);
      const auto keys = mappedInput.mapLabels(tr(STR_JK_MENU), tr(STR_JK_SELECT), "\xE2\x80\xB9", "\xE2\x80\xBA");
      const char* names[4] = {keys.btn1, keys.btn2, keys.btn3, keys.btn4};
      for (int i = 0; i < 4; i++) {
        const int kx = bx + 16 + i * KEY_STEP;
        renderer.drawRoundedRect(kx, y + BODY_H - 44, KEY_W, 30, 1, 6, true);
        textCenter(SMALL_FONT_ID, kx + KEY_W / 2, y + BODY_H - 40, names[i], true);
      }
      y += BODY_H + 30;
      break;
    }
  }
  for (const StrId id : LINES[page]) {
    renderer.fillRoundedRect(SIDE, y + 8, 10, 10, 5, Color::Black);
    for (const auto& text : renderer.wrappedText(UI_12_FONT_ID, I18N.get(id), width - 2 * SIDE - 24, 3)) {
      renderer.drawText(UI_12_FONT_ID, SIDE + 24, y, text.c_str());
      y += 30;
    }
    y += 18;
  }
  drawHints();
}

void JokersActivity::drawLevels() const {
  const int width = renderer.getScreenWidth();
  char line[24];
  snprintf(line, sizeof(line), tr(STR_JK_ANTE), run.ante, FINAL_ANTE);
  drawTitleBand(tr(STR_JK_POKER_HANDS), line);
  int y = HEADER_H + 14;
  for (int t = HAND_TYPES - 1; t >= 0; t--, y += 70) {
    const Level level = handLevelValues(static_cast<HandType>(t), run.levels[t]);
    renderer.drawText(UI_12_FONT_ID, SIDE, y + 8, text::handName(t), true, EpdFontFamily::BOLD);
    snprintf(line, sizeof(line), tr(STR_JK_LEVEL), run.levels[t]);
    renderer.drawText(UI_10_FONT_ID, SIDE, y + 36, line);
    drawMiniEquation(width - SIDE, y + 12, level.chips, level.mult);
    renderer.drawLine(SIDE, y + 64, width - SIDE, y + 64, true);
  }
  drawHints();
}
