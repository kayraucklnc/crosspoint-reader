// Drawing for ConnectionsActivity: home, board, result card, archive calendar,
// statistics and the update screen, all 1-bit.
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ConnectionsActivity.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
using connections::kGroups;
using connections::kMembers;

constexpr int SIDE = 24;
constexpr int BOARD_SIDE = 40;  // keeps the board clear of the side-button hints
constexpr int HEADER_H = 48;
constexpr int GAP = 8;
constexpr int NOTE_H = 32;
constexpr int ACTION_H = 52;
constexpr int MAX_ROW_H = 124;
constexpr int FOCUS = 5;  // focus frame sits this far outside a tile

int hintsTop(const GfxRenderer& r) {
  return r.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
}

void hatch(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int step) {
  const auto clip = r.getClipRect();
  r.setClipRect(x, y, w, h);
  for (int d = -h; d < w; d += step) r.drawLine(x + d, y + h, x + d + h, y, 2, true);
  r.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}

// The NYT's four colours as 1-bit fills, easiest (light dots) to hardest (solid).
// An unknown level falls back to the group's position so groups stay distinct.
void fillLevel(const GfxRenderer& r, const int x, const int y, const int w, const int h, const uint8_t level,
               const int group) {
  const int tone = level <= 3 ? level : group;
  switch (tone) {
    case 0:
      r.fillRectDither(x, y, w, h, Color::LightGray);
      break;
    case 1:
      r.fillRectDither(x, y, w, h, Color::DarkGray);
      break;
    case 2:
      hatch(r, x, y, w, h, 5);
      break;
    default:
      r.fillRect(x, y, w, h, true);
      break;
  }
}

void formatDate(const uint32_t date, const char* format, char* out, const size_t size) {
  struct tm t{};
  t.tm_year = static_cast<int>(date / 10000) - 1900;
  t.tm_mon = static_cast<int>((date / 100) % 100) - 1;
  t.tm_mday = static_cast<int>(date % 100);
  t.tm_wday = connections::dayOfWeek(date);
  if (date == 0 || strftime(out, size, format, &t) == 0) snprintf(out, size, "-");
}

constexpr int TILE_FONTS[3] = {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};
constexpr int TILE_PAD = 8;

EpdFontFamily::Style tileStyle(const int font) {
  return font == SMALL_FONT_ID ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
}

// Splits at the space or hyphen nearest the middle (a hyphen stays on the first
// line); false when there is none.
bool splitWord(const char* text, char* first, char* second, const int size) {
  const char* split = nullptr;
  const int length = static_cast<int>(strlen(text));
  for (int i = 1; i < length - 1; i++) {
    if (text[i] != ' ' && text[i] != '-') continue;
    if (!split || std::abs(i - length / 2) < std::abs(static_cast<int>(split - text) - length / 2)) split = text + i;
  }
  if (!split) return false;
  const int head = std::min(static_cast<int>(split - text) + (*split == '-' ? 1 : 0), size - 1);
  memcpy(first, text, head);
  first[head] = '\0';
  snprintf(second, size, "%s", split + 1);
  return true;
}

// Index into TILE_FONTS of the largest font that fits `text` on one line or two.
int fitIndex(const GfxRenderer& r, const char* text, const int room) {
  char first[40], second[40];
  const bool split = splitWord(text, first, second, sizeof(first));
  for (int i = 0; i < 3; i++) {
    const int font = TILE_FONTS[i];
    if (r.getTextWidth(font, text, tileStyle(font)) <= room) return i;
    if (split &&
        std::max(r.getTextWidth(font, first, tileStyle(font)), r.getTextWidth(font, second, tileStyle(font))) <= room) {
      return i;
    }
  }
  return 2;
}

// Draws a word centred in its tile in the board's font: one line, else two at a
// space or hyphen, else cut short.
void drawFitted(const GfxRenderer& r, const char* text, const int fontIndex, const int x, const int y, const int w,
                const int h, const bool black) {
  const int font = TILE_FONTS[fontIndex];
  const int room = w - TILE_PAD;
  const int lh = r.getLineHeight(font);
  const int tw = r.getTextWidth(font, text, tileStyle(font));
  if (tw <= room) {
    r.drawText(font, x + (w - tw) / 2, y + (h - lh) / 2, text, black, tileStyle(font));
    return;
  }
  char first[40], second[40];
  if (splitWord(text, first, second, sizeof(first))) {
    const int w1 = r.getTextWidth(font, first, tileStyle(font));
    const int w2 = r.getTextWidth(font, second, tileStyle(font));
    if (std::max(w1, w2) <= room) {
      const int top = y + (h - 2 * lh) / 2;
      r.drawText(font, x + (w - w1) / 2, top, first, black, tileStyle(font));
      r.drawText(font, x + (w - w2) / 2, top + lh, second, black, tileStyle(font));
      return;
    }
  }
  const std::string cut = r.truncatedText(font, text, room, tileStyle(font));
  const int cw = r.getTextWidth(font, cut.c_str(), tileStyle(font));
  r.drawText(font, x + (w - cw) / 2, y + (h - lh) / 2, cut.c_str(), black, tileStyle(font));
}

void drawPill(const GfxRenderer& r, const char* text, const int y) {
  const int width = r.getScreenWidth();
  const int tw = r.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  const int pw = std::min(width - 2 * SIDE, tw + 36);
  r.fillRoundedRect((width - pw) / 2, y, pw, NOTE_H, NOTE_H / 2, Color::Black);
  r.drawText(UI_10_FONT_ID, (width - tw) / 2, y + (NOTE_H - r.getLineHeight(UI_10_FONT_ID)) / 2, text, false,
             EpdFontFamily::BOLD);
}

void drawFocus(const GfxRenderer& r, const int x, const int y, const int w, const int h, const bool dark) {
  r.drawRoundedRect(x - FOCUS, y - FOCUS, w + 2 * FOCUS, h + 2 * FOCUS, 3, 12, true);
  if (dark) r.drawRoundedRect(x + 3, y + 3, w - 6, h - 6, 2, 6, false);
}

struct BoardLayout {
  int gridY, rowH, noteY, mistakesY, actionsY;
};

BoardLayout boardLayout(const GfxRenderer& r) {
  BoardLayout l{};
  l.actionsY = hintsTop(r) - 14 - ACTION_H;
  l.mistakesY = l.actionsY - 12 - 22;
  l.noteY = l.mistakesY - 10 - NOTE_H;
  l.gridY = HEADER_H + 6;
  l.rowH = std::min(MAX_ROW_H, (l.noteY - 12 - l.gridY - (kGroups - 1) * GAP) / kGroups);
  return l;
}

void drawDie(const GfxRenderer& r, const int x, const int y, const int size) {
  r.drawRoundedRect(x + 2, y + 2, size - 4, size - 4, 2, 8, true);
  static constexpr int8_t PIPS[5][2] = {{-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}};
  for (const auto& p : PIPS) {
    r.fillRoundedRect(x + size / 2 + p[0] * 11 - 4, y + size / 2 + p[1] * 11 - 4, 8, 8, 4, Color::Black);
  }
}
}  // namespace

// ---- shared -------------------------------------------------------------------

void ConnectionsActivity::drawMenuIcon(const GfxRenderer& r, const int x, const int y, const int size) {
  // A solved bar over three rows of word tiles.
  const int gap = std::max(2, size / 18);
  const int cell = (size - 3 * gap) / 4;
  r.fillRoundedRect(x, y, size, cell, 3, Color::Black);
  for (int row = 1; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      const int cx = x + col * (cell + gap), cy = y + row * (cell + gap);
      const bool picked = (row == 2 && col != 1) || (row == 3 && col == 2);
      if (picked) {
        r.fillRoundedRect(cx, cy, cell, cell, 3, Color::DarkGray);
        r.drawRoundedRect(cx, cy, cell, cell, 1, 3, true);
      } else {
        r.drawRoundedRect(cx, cy, cell, cell, 2, 3, true);
      }
    }
  }
}

void ConnectionsActivity::drawHeader(const char* title, const char* subtitle) const {
  renderer.drawText(UI_12_FONT_ID, SIDE, 12, title, true, EpdFontFamily::BOLD);
  if (!subtitle) return;
  const int w = renderer.getTextWidth(UI_10_FONT_ID, subtitle);
  renderer.drawText(UI_10_FONT_ID, renderer.getScreenWidth() - SIDE - w, 15, subtitle);
}

// A found (solid) or revealed (outlined) group: level swatch, name and words.
void ConnectionsActivity::drawGroupBar(const int x, const int y, const int w, const int h,
                                       const connections::Group& group, const int groupIndex, const bool solid) const {
  if (solid) {
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 3, 10, true);
  }
  constexpr int SWATCH = 18;
  const int sx = x + 12, sy = y + (h - SWATCH) / 2;
  renderer.fillRect(sx - 2, sy - 2, SWATCH + 4, SWATCH + 4, false);
  fillLevel(renderer, sx, sy, SWATCH, SWATCH, group.level, groupIndex);
  renderer.drawRect(sx, sy, SWATCH, SWATCH, 1, true);

  const int textX = sx + SWATCH + 12;
  const int textW = x + w - 12 - textX;
  const int nameH = renderer.getLineHeight(UI_10_FONT_ID);
  const int wordsH = renderer.getLineHeight(SMALL_FONT_ID);
  // A long name takes a second line when the bar is tall enough.
  char first[connections::kMaxGroupLen + 1];
  snprintf(first, sizeof(first), "%s", group.name);
  const char* rest = nullptr;
  if (renderer.getTextWidth(UI_10_FONT_ID, first, EpdFontFamily::BOLD) > textW && h >= 2 * nameH + wordsH + 8) {
    for (char* space = strrchr(first, ' '); space; space = strrchr(first, ' ')) {
      *space = '\0';
      if (renderer.getTextWidth(UI_10_FONT_ID, first, EpdFontFamily::BOLD) <= textW) {
        rest = group.name + (space - first) + 1;
        break;
      }
    }
    if (!rest) snprintf(first, sizeof(first), "%s", group.name);
  }
  const int lines = rest ? 2 : 1;
  const int top = y + (h - lines * nameH - wordsH) / 2;
  renderer.drawText(UI_10_FONT_ID, textX, top,
                    renderer.truncatedText(UI_10_FONT_ID, first, textW, EpdFontFamily::BOLD).c_str(), !solid,
                    EpdFontFamily::BOLD);
  if (rest) {
    renderer.drawText(UI_10_FONT_ID, textX, top + nameH,
                      renderer.truncatedText(UI_10_FONT_ID, rest, textW, EpdFontFamily::BOLD).c_str(), !solid,
                      EpdFontFamily::BOLD);
  }
  char words[4 * (connections::kMaxWordLen + 2)];
  snprintf(words, sizeof(words), "%s, %s, %s, %s", group.members[0], group.members[1], group.members[2],
           group.members[3]);
  renderer.drawText(SMALL_FONT_ID, textX, top + lines * nameH,
                    renderer.truncatedText(SMALL_FONT_ID, words, textW).c_str(), !solid);
}

// Four stat numbers across the width: played, win %, streak, best streak.
void ConnectionsActivity::drawStatTiles(const int y) const {
  const int width = renderer.getScreenWidth();
  const int pct = summary.played ? (summary.won * 100 + summary.played / 2) / summary.played : 0;
  const int values[4] = {summary.played, pct, summary.streak, summary.bestStreak};
  static constexpr StrId LABELS[4] = {StrId::STR_WORDLE_PLAYED, StrId::STR_WORDLE_WIN_PCT, StrId::STR_WORDLE_STREAK,
                                      StrId::STR_WORDLE_BEST_STREAK};
  const int cellW = (width - 2 * SIDE) / 4;
  for (int i = 0; i < 4; i++) {
    const int cx = SIDE + i * cellW + cellW / 2;
    char text[12];
    snprintf(text, sizeof(text), "%d", values[i]);
    const int tw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_18_FONT_ID, cx - tw / 2, y, text, true, EpdFontFamily::BOLD);
    const std::string label = renderer.truncatedText(SMALL_FONT_ID, I18N.get(LABELS[i]), cellW - 4);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label.c_str());
    renderer.drawText(SMALL_FONT_ID, cx - lw / 2, y + 42, label.c_str());
  }
}

// The last 16 puzzle days, oldest first: solid = perfect, framed dot = solved,
// cross = missed, faint frame = not played.
void ConnectionsActivity::drawRecentStrip(const int x, const int y, const int width) const {
  constexpr int COUNT = 16;
  const int pitch = width / COUNT;
  const int cell = std::min(24, pitch - 4);
  for (int i = 0; i < COUNT; i++) {
    const int cx = x + i * pitch + (pitch - cell) / 2;
    switch (summary.recent[i]) {
      case connections::kDayPerfect:
        renderer.fillRoundedRect(cx, y, cell, cell, 4, Color::Black);
        break;
      case connections::kDayWon:
        renderer.drawRoundedRect(cx, y, cell, cell, 2, 4, true);
        renderer.fillRoundedRect(cx + cell / 2 - 4, y + cell / 2 - 4, 8, 8, 4, Color::Black);
        break;
      case connections::kDayLost:
        renderer.drawRoundedRect(cx, y, cell, cell, 2, 4, true);
        renderer.drawLine(cx + 6, y + 6, cx + cell - 7, y + cell - 7, 2, true);
        renderer.drawLine(cx + cell - 7, y + 6, cx + 6, y + cell - 7, 2, true);
        break;
      default:
        renderer.drawRoundedRect(cx, y, cell, cell, 1, 4, true);
        break;
    }
  }
}

// ---- home ---------------------------------------------------------------------

void ConnectionsActivity::renderHome() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();

  drawMenuIcon(renderer, SIDE, 16, 56);
  renderer.drawText(NOTOSANS_18_FONT_ID, SIDE + 72, 8, tr(STR_PLAY_CONNECTIONS), true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, SIDE + 72, 50, tr(STR_CONN_TAGLINE));

  drawStatTiles(88);
  renderer.drawText(SMALL_FONT_ID, SIDE, 158, tr(STR_CONN_RECENT));
  drawRecentStrip(SIDE, 180, width - 2 * SIDE);

  const int top = 220;
  constexpr int ITEM_GAP = 10;
  const int itemH = std::min(92, (hintsTop(renderer) - 12 - top - ITEM_GAP * (HOME_ITEMS - 1)) / HOME_ITEMS);
  const int todayIdx = todayIndex();
  uint32_t todayDate = 0;
  if (todayIdx >= 0) pack.dateAt(todayIdx, todayDate);

  for (int i = 0; i < HOME_ITEMS; i++) {
    const int y = top + i * (itemH + ITEM_GAP);
    const int x = SIDE, w = width - 2 * SIDE;
    const bool focused = i == homeSelected;
    renderer.drawRoundedRect(x, y, w, itemH, focused ? 3 : 1, 10, true);
    constexpr int ICON = 44;
    const int ix = x + 16, iy = y + (itemH - ICON) / 2;
    char title[48] = {}, line[80] = {};
    switch (i) {
      case HOME_TODAY: {
        renderer.drawRoundedRect(ix, iy + 4, ICON, ICON - 4, 2, 6, true);
        renderer.fillRoundedRect(ix, iy + 4, ICON, 12, 6, true, true, false, false, Color::Black);
        renderer.fillRect(ix + 11, iy, 4, 10, true);
        renderer.fillRect(ix + ICON - 15, iy, 4, 10, true);
        char day[4];
        snprintf(day, sizeof(day), "%u", static_cast<unsigned>(todayDate % 100));
        const int dw = renderer.getTextWidth(UI_10_FONT_ID, day, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, ix + (ICON - dw) / 2, iy + 19, todayDate ? day : "-", true,
                          EpdFontFamily::BOLD);
        const bool current = today != 0 && todayDate == today;
        snprintf(title, sizeof(title), "%s", current || today == 0 ? tr(STR_CONN_TODAY) : tr(STR_CONN_LATEST));
        if (!packOpen) {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_NO_PACK));
          break;
        }
        const connections::Record record = resultFor(todayDate);
        char when[24];
        formatDate(todayDate, "%b %d", when, sizeof(when));
        const bool inProgress = gameLoaded && game.puzzle().date == todayDate && !record.played &&
                                game.result() == connections::Result::Playing &&
                                (game.solvedCount() > 0 || game.mistakes() > 0);
        if (record.played && record.lost) {
          snprintf(line, sizeof(line), "%s  ·  %s", when, tr(STR_CONN_MISSED));
        } else if (record.played) {
          snprintf(line, sizeof(line), tr(STR_CONN_SOLVED_MISTAKES), when, record.mistakes);
        } else if (inProgress) {
          snprintf(line, sizeof(line), tr(STR_CONN_IN_PROGRESS), when, game.solvedCount());
        } else {
          snprintf(line, sizeof(line), "%s  ·  %s", when, tr(STR_CONN_NEW));
        }
        if (record.played && !record.lost) {
          const int cx = x + w - 34, cy = y + itemH / 2;
          renderer.drawLine(cx - 10, cy, cx - 3, cy + 8, 4, true);
          renderer.drawLine(cx - 3, cy + 8, cx + 12, cy - 10, 4, true);
        }
        break;
      }
      case HOME_RANDOM:
        drawDie(renderer, ix, iy, ICON);
        snprintf(title, sizeof(title), "%s", tr(STR_CONN_RANDOM));
        snprintf(line, sizeof(line), tr(STR_CONN_UNPLAYED), unplayedCount());
        break;
      case HOME_ARCHIVE:
        for (int k = 0; k < 9; k++) {
          const int c = ICON / 3;
          const int cx = ix + (k % 3) * c, cy = iy + (k / 3) * c;
          if (k == 4) {
            renderer.fillRect(cx + 2, cy + 2, c - 4, c - 4, true);
          } else {
            renderer.drawRect(cx + 2, cy + 2, c - 4, c - 4, 1, true);
          }
        }
        snprintf(title, sizeof(title), "%s", tr(STR_CONN_ARCHIVE));
        if (packOpen) {
          char since[24];
          formatDate(firstDate, "%b %Y", since, sizeof(since));
          snprintf(line, sizeof(line), tr(STR_CONN_PUZZLES_SINCE), pack.count(), since);
        } else {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_NO_PACK));
        }
        break;
      case HOME_UPDATE: {
        const int ax = ix + ICON / 2;
        renderer.fillRect(ax - 4, iy + 2, 8, 22, true);
        const int xs[3] = {ax - 14, ax + 14, ax};
        const int ys[3] = {iy + 22, iy + 22, iy + 36};
        renderer.fillPolygon(xs, ys, 3, true);
        renderer.fillRect(ix + 4, iy + ICON - 5, ICON - 8, 4, true);
        snprintf(title, sizeof(title), "%s", tr(STR_CONN_UPDATE));
        if (packOpen) {
          char newest[24];
          formatDate(lastDate, "%b %d, %Y", newest, sizeof(newest));
          snprintf(line, sizeof(line), tr(STR_CONN_NEWEST), newest);
        } else {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_GET_PUZZLES));
        }
        break;
      }
      default: {
        static constexpr int BARS[3] = {18, 30, 42};
        for (int k = 0; k < 3; k++) renderer.fillRect(ix + 4 + k * 14, iy + ICON - BARS[k], 10, BARS[k], true);
        snprintf(title, sizeof(title), "%s", tr(STR_WORDLE_STATS));
        snprintf(line, sizeof(line), tr(STR_CONN_STATS_LINE), summary.won, summary.perfect);
        break;
      }
    }
    const int textX = ix + ICON + 24;
    const int textW = x + w - 56 - textX;
    renderer.drawText(UI_12_FONT_ID, textX, y + itemH / 2 - 28, title, true,
                      focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, textX, y + itemH / 2 + 4,
                      renderer.truncatedText(UI_10_FONT_ID, line, textW).c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- board --------------------------------------------------------------------

void ConnectionsActivity::drawTile(const int tile, const int fontIndex, const int x, const int y, const int w,
                                   const int h) const {
  const bool selected = game.isSelected(tile);
  const bool focused = finaleStage == 0 && tile == cursorTile();
  if (selected) {
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  }
  drawFitted(renderer, game.tileWord(tile), fontIndex, x, y, w, h, !selected);
  if (focused) drawFocus(renderer, x, y, w, h, selected);
}

void ConnectionsActivity::drawActions(const int y, const int h) const {
  const int width = renderer.getScreenWidth();
  const int w = (width - 2 * BOARD_SIDE - 2 * 14) / ACTION_COUNT;
  static constexpr StrId LABELS[ACTION_COUNT] = {StrId::STR_CONN_SHUFFLE, StrId::STR_CONN_DESELECT,
                                                 StrId::STR_CONN_SUBMIT};
  for (int i = 0; i < ACTION_COUNT; i++) {
    const int x = BOARD_SIDE + i * (w + 14);
    const bool ready = i == ACT_SUBMIT && game.canSubmit() && !game.selectionAlreadyGuessed();
    const bool idle = (i == ACT_DESELECT && game.selectedCount() == 0) || (i == ACT_SUBMIT && !ready);
    if (ready) {
      renderer.fillRoundedRect(x, y, w, h, h / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, w, h, idle ? 1 : 2, h / 2, true);
    }
    const char* label = I18N.get(LABELS[i]);
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, x + (w - tw) / 2, y + (h - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label,
                      !ready, idle ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD);
    if (finaleStage == 0 && cursorRow >= boardRows() && cursorCol == i) {
      renderer.drawRoundedRect(x - FOCUS, y - FOCUS, w + 2 * FOCUS, h + 2 * FOCUS, 3, h / 2 + FOCUS, true);
      if (ready) renderer.drawRoundedRect(x + 3, y + 3, w - 6, h - 6, 2, h / 2 - 3, false);
    }
  }
}

void ConnectionsActivity::renderGame() const {
  const int width = renderer.getScreenWidth();
  const BoardLayout l = boardLayout(renderer);
  renderer.clearScreen();

  char subtitle[48], when[24];
  formatDate(game.puzzle().date, "%b %d, %Y", when, sizeof(when));
  snprintf(subtitle, sizeof(subtitle), "#%u  ·  %s", game.puzzle().id, when);
  drawHeader(tr(STR_PLAY_CONNECTIONS), subtitle);
  GUI.drawSideButtonHints(renderer, tr(STR_WORDLE_ROW_UP), tr(STR_WORDLE_ROW_DOWN));

  const int gridW = width - 2 * BOARD_SIDE;
  const bool lost = game.result() == connections::Result::Lost;
  const int bars = game.revealedCount();
  for (int i = 0; i < bars; i++) {
    const connections::Group& group = game.solvedGroup(i);
    const int groupIndex = static_cast<int>(&group - game.puzzle().groups);
    drawGroupBar(BOARD_SIDE, l.gridY + i * (l.rowH + GAP), gridW, l.rowH, group, groupIndex,
                 !lost || i < game.solvedCount());
  }
  if (!lost) {
    const int tileW = (gridW - (kMembers - 1) * GAP) / kMembers;
    for (int t = 0; t < game.tileCount(); t++) {
      const int row = bars + t / kMembers, col = t % kMembers;
      // Long words step down a size on their own, as on the paper's board.
      const int fontIndex = fitIndex(renderer, game.tileWord(t), tileW - TILE_PAD);
      drawTile(t, fontIndex, BOARD_SIDE + col * (tileW + GAP), l.gridY + row * (l.rowH + GAP), tileW, l.rowH);
    }
  }

  const char* note = nullptr;
  switch (toast) {
    case Toast::ONE_AWAY:
      note = tr(STR_CONN_ONE_AWAY);
      break;
    case Toast::WRONG:
      note = tr(STR_CONN_WRONG);
      break;
    case Toast::REPEAT:
      note = tr(STR_CONN_REPEAT);
      break;
    case Toast::NEED_FOUR:
      note = tr(STR_CONN_NEED_FOUR);
      break;
    case Toast::FULL:
      note = tr(STR_CONN_FULL);
      break;
    case Toast::NONE:
      break;
  }
  if (finaleStage != 0) {
    note = game.result() == connections::Result::Won ? tr(STR_CONN_SOLVED) : tr(STR_CONN_OUT_OF_GUESSES);
  }
  if (note) drawPill(renderer, note, l.noteY);

  // Mistakes remaining as dots.
  const char* label = tr(STR_CONN_MISTAKES_LEFT);
  constexpr int DOT = 14, DOT_GAP = 10;
  const int lw = renderer.getTextWidth(UI_10_FONT_ID, label);
  const int total = lw + 16 + connections::kMaxMistakes * (DOT + DOT_GAP) - DOT_GAP;
  const int mx = (width - total) / 2;
  renderer.drawText(UI_10_FONT_ID, mx, l.mistakesY, label);
  for (int i = 0; i < connections::kMaxMistakes; i++) {
    const int dx = mx + lw + 16 + i * (DOT + DOT_GAP);
    const int dy = l.mistakesY + (renderer.getLineHeight(UI_10_FONT_ID) - DOT) / 2;
    if (i < game.mistakesLeft()) {
      renderer.fillRoundedRect(dx, dy, DOT, DOT, DOT / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(dx, dy, DOT, DOT, 1, DOT / 2, true);
    }
  }

  drawActions(l.actionsY, ACTION_H);

  const char* confirm = tr(STR_CONN_SELECT);
  if (finaleStage != 0) {
    confirm = tr(STR_TRIVIA_RESULTS);
  } else if (cursorRow >= boardRows()) {
    confirm = cursorCol == ACT_SHUFFLE    ? tr(STR_CONN_SHUFFLE)
              : cursorCol == ACT_DESELECT ? tr(STR_CONN_DESELECT)
                                          : tr(STR_CONN_SUBMIT);
  } else if (game.isSelected(cursorTile())) {
    confirm = tr(STR_CONN_UNSELECT);
  }
  const bool playing = finaleStage == 0;
  const auto labels = mappedInput.mapLabels(playing ? tr(STR_WORDLE_MENU) : "", confirm,
                                            playing ? tr(STR_DIR_LEFT) : "", playing ? tr(STR_DIR_RIGHT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- result -------------------------------------------------------------------

// One row per submitted guess, each word shown in its group's fill.
void ConnectionsActivity::drawGuessGrid(const int x, const int y, const int cell) const {
  const connections::Game::Save state = game.save();
  for (int g = 0; g < state.guessCount; g++) {
    int col = 0;
    for (int bit = 0; bit < connections::kTiles && col < kMembers; bit++) {
      if (!(state.guessed[g] & (1u << bit))) continue;
      const int group = bit / kMembers;
      const int cx = x + col * (cell + 4), cy = y + g * (cell + 4);
      fillLevel(renderer, cx, cy, cell, cell, game.puzzle().groups[group].level, group);
      renderer.drawRect(cx, cy, cell, cell, 1, true);
      col++;
    }
  }
}

void ConnectionsActivity::renderResult() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  char text[64], when[24];
  formatDate(game.puzzle().date, "%a, %b %d %Y", when, sizeof(when));
  snprintf(text, sizeof(text), "#%u  ·  %s", game.puzzle().id, when);
  drawHeader(tr(STR_PLAY_CONNECTIONS), text);

  const bool won = game.result() == connections::Result::Won;
  const char* headline = !won                   ? tr(STR_CONN_NEXT_TIME)
                         : game.mistakes() == 0 ? tr(STR_CONN_PERFECT)
                                                : tr(STR_CONN_SOLVED);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, 58, headline, true, EpdFontFamily::BOLD);
  if (won) {
    snprintf(text, sizeof(text), tr(STR_CONN_MISTAKES_MADE), game.mistakes());
  } else {
    snprintf(text, sizeof(text), "%s", tr(STR_CONN_OUT_OF_GUESSES));
  }
  if (replay) snprintf(text + strlen(text), sizeof(text) - strlen(text), "  ·  %s", tr(STR_CONN_REPLAY));
  renderer.drawCenteredText(UI_10_FONT_ID, 102, text);

  constexpr int CELL = 24;
  const connections::Game::Save state = game.save();
  const int gridW = kMembers * (CELL + 4) - 4;
  drawGuessGrid((width - gridW) / 2, 136, CELL);
  int y = 136 + state.guessCount * (CELL + 4) + 16;

  const int barH = 60;
  for (int i = 0; i < game.revealedCount(); i++) {
    const connections::Group& group = game.solvedGroup(i);
    drawGroupBar(SIDE, y, width - 2 * SIDE, barH, group, static_cast<int>(&group - game.puzzle().groups),
                 i < game.solvedCount());
    y += barH + 8;
  }
  drawStatTiles(y + 14);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WORDLE_STATS), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- archive ------------------------------------------------------------------

void ConnectionsActivity::renderArchive() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  char month[32];
  formatDate(archiveDate - archiveDate % 100 + 1, "%B %Y", month, sizeof(month));
  drawHeader(tr(STR_CONN_ARCHIVE), month);

  const int year = static_cast<int>(archiveDate / 10000), mon = static_cast<int>((archiveDate / 100) % 100);
  const uint32_t first = archiveDate - archiveDate % 100 + 1;
  const int lead = (connections::dayOfWeek(first) + 6) % 7;  // Monday first
  const int days = connections::daysInMonth(year, mon);
  const int cell = std::min(64, (width - 2 * SIDE) / 7);
  const int gx = (width - 7 * cell) / 2;
  const int gy = 84;

  static constexpr StrId WEEKDAYS[7] = {StrId::STR_WD_MON, StrId::STR_WD_TUE, StrId::STR_WD_WED, StrId::STR_WD_THU,
                                        StrId::STR_WD_FRI, StrId::STR_WD_SAT, StrId::STR_WD_SUN};
  for (int d = 0; d < 7; d++) {
    const char* name = I18N.get(WEEKDAYS[d]);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, name);
    renderer.drawText(SMALL_FONT_ID, gx + d * cell + (cell - tw) / 2, gy - 26, name);
  }
  for (int day = 1; day <= days; day++) {
    const int slot = lead + day - 1;
    const int cx = gx + (slot % 7) * cell + 3, cy = gy + (slot / 7) * cell + 3, cs = cell - 6;
    const uint32_t date = first + static_cast<uint32_t>(day - 1);
    const bool available = date >= firstDate && date <= lastDate;
    const connections::Record record = resultFor(date);
    char label[4];
    snprintf(label, sizeof(label), "%d", day);
    bool whiteText = false;
    if (!available) {
      // Outside the archive: number only.
    } else if (record.played && !record.lost && record.mistakes == 0) {
      renderer.fillRoundedRect(cx, cy, cs, cs, 8, Color::Black);
      whiteText = true;
    } else if (record.played && !record.lost) {
      renderer.drawRoundedRect(cx, cy, cs, cs, 3, 8, true);
      renderer.fillRoundedRect(cx + cs / 2 - 4, cy + cs - 13, 8, 8, 4, Color::Black);
    } else if (record.played) {
      renderer.drawRoundedRect(cx, cy, cs, cs, 2, 8, true);
      renderer.drawLine(cx + 8, cy + cs - 9, cx + cs - 9, cy + 8, 2, true);
    } else {
      renderer.drawRoundedRect(cx, cy, cs, cs, 1, 8, true);
    }
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, cx + (cs - tw) / 2, cy + (cs - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label,
                      !whiteText, available ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (date == today) renderer.fillRect(cx + cs / 2 - 8, cy + 6, 16, 3, !whiteText);  // today: a bar on top
    if (date == archiveDate) drawFocus(renderer, cx, cy, cs, cs, whiteText);
  }

  // Details of the focused day.
  const int infoY = gy + 6 * cell + 12;
  char when[32], line[80];
  formatDate(archiveDate, "%A, %b %d", when, sizeof(when));
  const int index = pack.indexOfDate(archiveDate);
  const connections::Record record = resultFor(archiveDate);
  renderer.drawText(UI_12_FONT_ID, SIDE, infoY, when, true, EpdFontFamily::BOLD);
  if (index < 0) {
    snprintf(line, sizeof(line), "%s", tr(STR_CONN_NOT_IN_PACK));
  } else if (!record.played) {
    snprintf(line, sizeof(line), "%s", tr(STR_CONN_NEW));
  } else if (record.lost) {
    snprintf(line, sizeof(line), "%s", tr(STR_CONN_MISSED));
  } else {
    snprintf(line, sizeof(line), tr(STR_CONN_MISTAKES_MADE), record.mistakes);
  }
  renderer.drawText(UI_10_FONT_ID, SIDE, infoY + 32, line);

  // Legend.
  const int legendY = infoY + 72;
  const int slot = (width - 2 * SIDE) / 3;
  static constexpr StrId LEGEND[3] = {StrId::STR_CONN_PERFECT, StrId::STR_CONN_SOLVED, StrId::STR_CONN_MISSED};
  for (int i = 0; i < 3; i++) {
    const int lx = SIDE + i * slot;
    if (i == 0) {
      renderer.fillRoundedRect(lx, legendY, 24, 24, 5, Color::Black);
    } else if (i == 1) {
      renderer.drawRoundedRect(lx, legendY, 24, 24, 3, 5, true);
      renderer.fillRoundedRect(lx + 9, legendY + 9, 6, 6, 3, Color::Black);
    } else {
      renderer.drawRoundedRect(lx, legendY, 24, 24, 2, 5, true);
      renderer.drawLine(lx + 5, legendY + 18, lx + 18, legendY + 5, 2, true);
    }
    renderer.drawText(SMALL_FONT_ID, lx + 32, legendY + 3, I18N.get(LEGEND[i]));
  }

  GUI.drawSideButtonHints(renderer, tr(STR_CONN_WEEK_BACK), tr(STR_CONN_WEEK_ON));
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), index >= 0 ? tr(STR_OPEN) : "", tr(STR_CONN_DAY_BACK), tr(STR_CONN_DAY_ON));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- statistics -----------------------------------------------------------------

void ConnectionsActivity::renderStats() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(tr(STR_WORDLE_STATS), tr(STR_PLAY_CONNECTIONS));

  // Hero: puzzles solved.
  constexpr int HERO_Y = 60;
  renderer.drawRoundedRect(SIDE, HERO_Y, width - 2 * SIDE, 118, 2, 12, true);
  char text[48];
  snprintf(text, sizeof(text), "%d", summary.won);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, HERO_Y + 22, text, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, HERO_Y + 70, tr(STR_CONN_PUZZLES_SOLVED), true, EpdFontFamily::BOLD);

  drawStatTiles(HERO_Y + 142);

  // Wins by mistakes made, plus misses.
  int y = HERO_Y + 226;
  renderer.drawText(UI_10_FONT_ID, SIDE, y, tr(STR_CONN_MISTAKE_SPREAD), true, EpdFontFamily::BOLD);
  y += 32;
  const int rows[5] = {summary.winsByMistakes[0], summary.winsByMistakes[1], summary.winsByMistakes[2],
                       summary.winsByMistakes[3], summary.played - summary.won};
  int most = 1;
  for (const int n : rows) most = std::max(most, n);
  constexpr int ROW_H = 34, LABEL_W = 34;
  for (int i = 0; i < 5; i++) {
    const int ry = y + i * ROW_H;
    char label[8];
    snprintf(label, sizeof(label), i < 4 ? "%d" : "X", i);
    renderer.drawText(UI_10_FONT_ID, SIDE, ry + 3, label, true, EpdFontFamily::BOLD);
    snprintf(label, sizeof(label), "%d", rows[i]);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::BOLD);
    const int span = width - 2 * SIDE - LABEL_W;
    const int barW = std::max(tw + 14, span * rows[i] / most);
    const bool strong = rows[i] == most && rows[i] > 0;
    if (strong) {
      renderer.fillRect(SIDE + LABEL_W, ry + 2, barW, ROW_H - 8, true);
    } else {
      renderer.drawRect(SIDE + LABEL_W, ry + 2, barW, ROW_H - 8, 2, true);
    }
    renderer.drawText(SMALL_FONT_ID, SIDE + LABEL_W + barW - tw - 7, ry + 6, label, !strong, EpdFontFamily::BOLD);
  }
  y += 5 * ROW_H + 18;
  snprintf(text, sizeof(text), "%s: %d", tr(STR_CONN_PERFECT), summary.perfect);
  renderer.drawText(UI_10_FONT_ID, SIDE, y, text);
  renderer.drawText(SMALL_FONT_ID, SIDE, y + 36, tr(STR_CONN_RECENT));
  drawRecentStrip(SIDE, y + 58, width - 2 * SIDE);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- update -------------------------------------------------------------------

void ConnectionsActivity::renderUpdate() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(tr(STR_CONN_UPDATE), nullptr);
  const int mid = renderer.getScreenHeight() / 2 - 80;
  drawMenuIcon(renderer, (width - 72) / 2, mid - 100, 72);

  char line[96] = {};
  const char* title = "";
  switch (updateStep) {
    case UpdateStep::CONNECT:
    case UpdateStep::PICKING:
      title = tr(STR_INCOME_CONNECTING);
      break;
    case UpdateStep::DOWNLOAD:
      title = tr(STR_CONN_DOWNLOADING);
      snprintf(line, sizeof(line), "%s", tr(STR_CONN_DOWNLOAD_HINT));
      break;
    case UpdateStep::DONE: {
      title = tr(STR_CONN_UPDATED);
      char newest[24];
      formatDate(lastDate, "%b %d, %Y", newest, sizeof(newest));
      snprintf(line, sizeof(line), tr(STR_CONN_UPDATED_LINE), updateAccepted, newest);
      break;
    }
    case UpdateStep::FAILED:
      title = tr(STR_CONN_UPDATE_FAILED);
      snprintf(line, sizeof(line), "%s", updateError);
      break;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, mid, title, true, EpdFontFamily::BOLD);
  if (line[0]) renderer.drawCenteredText(UI_10_FONT_ID, mid + 36, line);
  renderer.drawCenteredText(SMALL_FONT_ID, mid + 80, tr(STR_CONN_SOURCE));

  const bool busy = updateStep == UpdateStep::CONNECT || updateStep == UpdateStep::DOWNLOAD;
  const auto labels = mappedInput.mapLabels(busy ? "" : tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ConnectionsActivity::render(RenderLock&& lock) {
  switch (screen) {
    case Screen::HOME:
      renderHome();
      break;
    case Screen::GAME:
      renderGame();
      break;
    case Screen::RESULT:
      renderResult();
      break;
    case Screen::ARCHIVE:
      renderArchive();
      break;
    case Screen::STATS:
      renderStats();
      break;
    case Screen::UPDATE:
      renderUpdate();
      break;
  }
  const bool clean = cleanNext || ++framesSinceClean >= play::FRAMES_PER_CLEAN;
  if (clean) framesSinceClean = 0;
  cleanNext = false;
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
