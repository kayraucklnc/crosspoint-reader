// Drawing for ConnectionsActivity: home, board, result card, archive calendar
// and statistics, all 1-bit.
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
constexpr int TILE_TEXT = connections::kMaxWordLen + 2;  // a line plus the break hyphen

EpdFontFamily::Style tileStyle(const int font) {
  return font == SMALL_FONT_ID ? EpdFontFamily::REGULAR : EpdFontFamily::BOLD;
}

// How a word sits on its tile: a font and one line, or two.
struct TileText {
  int fontIndex = 2;
  char first[TILE_TEXT] = {};
  char second[TILE_TEXT] = {};
};

// Byte offsets of the codepoints of `text` (UTF-8) plus the end; returns the count.
int codepoints(const char* text, uint32_t* cps, int* starts, const int max) {
  int n = 0, at = 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  while (bytes[at] && n < max) {
    const unsigned char lead = bytes[at];
    const int len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    uint32_t cp = len == 1 ? lead : len == 2 ? lead & 0x1F : len == 3 ? lead & 0x0F : lead & 0x07;
    for (int k = 1; k < len && bytes[at + k]; k++) cp = (cp << 6) | (bytes[at + k] & 0x3F);
    starts[n] = at;
    cps[n++] = cp;
    for (int k = 0; k < len && bytes[at]; k++) at++;
  }
  starts[n] = at;
  return n;
}

bool isVowel(const uint32_t cp) {
  switch (cp) {
    case 'A':
    case 'E':
    case 'I':
    case 'O':
    case 'U':
    case 0x130:
    case 0xC2:
    case 0xCE:
    case 0xD6:
    case 0xDB:
    case 0xDC:  // İ Â Î Ö Û Ü
      return true;
    default:
      return false;
  }
}

bool isLetter(const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp >= 0xC0; }

// Codepoint index of the break nearest the middle: at a space or hyphen, or
// (syllable) before a consonant followed by a vowel, which is where Turkish
// splits syllables. -1 when there is none.
int findBreak(const uint32_t* cps, const int n, const bool syllable) {
  int best = -1;
  for (int i = 1; i < n - 1; i++) {
    const bool ok = syllable ? i >= 2 && i <= n - 2 && isLetter(cps[i]) && !isVowel(cps[i]) && isVowel(cps[i + 1]) &&
                                   isLetter(cps[i - 1])
                             : cps[i] == ' ' || cps[i] == '-';
    if (ok && (best < 0 || std::abs(i - n / 2) < std::abs(best - n / 2))) best = i;
  }
  return best;
}

// The largest tile font the word fits in: one line, else two at a space or
// hyphen, else two at a syllable break with a hyphen, else cut short in the
// smallest font. scripts/gen_connections_tr.py mirrors this to check the pack.
TileText fitTile(const GfxRenderer& r, const char* text, const int room) {
  static constexpr struct {
    int fontIndex;
    bool hyphenate;
  } ATTEMPTS[] = {{0, false}, {1, false}, {0, true}, {1, true}, {2, false}, {2, true}};
  uint32_t cps[TILE_TEXT];
  int starts[TILE_TEXT + 1];
  const int n = codepoints(text, cps, starts, TILE_TEXT - 1);
  const int spaceBreak = findBreak(cps, n, false);
  const int syllableBreak = spaceBreak < 0 ? findBreak(cps, n, true) : -1;
  TileText out;
  for (const auto& attempt : ATTEMPTS) {
    const int font = TILE_FONTS[attempt.fontIndex];
    const auto style = tileStyle(font);
    out.fontIndex = attempt.fontIndex;
    snprintf(out.first, sizeof(out.first), "%s", text);
    out.second[0] = '\0';
    if (r.getTextWidth(font, out.first, style) <= room) return out;
    const int at = spaceBreak >= 0 ? spaceBreak : attempt.hyphenate ? syllableBreak : -1;
    if (at < 0) continue;
    const bool keepHyphen = spaceBreak >= 0 && cps[at] == '-';
    const int headBytes = spaceBreak >= 0 ? starts[at] + (keepHyphen ? 1 : 0) : starts[at];
    snprintf(out.first, sizeof(out.first), "%.*s%s", headBytes, text, spaceBreak >= 0 ? "" : "-");
    snprintf(out.second, sizeof(out.second), "%s", text + starts[at] + (spaceBreak >= 0 ? 1 : 0));
    if (std::max(r.getTextWidth(font, out.first, style), r.getTextWidth(font, out.second, style)) <= room) return out;
  }
  out.fontIndex = 2;
  const std::string cut = r.truncatedText(SMALL_FONT_ID, text, room, tileStyle(SMALL_FONT_ID));
  snprintf(out.first, sizeof(out.first), "%s", cut.c_str());
  out.second[0] = '\0';
  return out;
}

// Draws a fitted word centred in its tile.
void drawFitted(const GfxRenderer& r, const TileText& text, const int x, const int y, const int w, const int h,
                const bool black) {
  const int font = TILE_FONTS[text.fontIndex];
  const auto style = tileStyle(font);
  const int lh = r.getLineHeight(font);
  const int lines = text.second[0] ? 2 : 1;
  const int top = y + (h - lines * lh) / 2;
  r.drawText(font, x + (w - r.getTextWidth(font, text.first, style)) / 2, top, text.first, black, style);
  if (lines == 2)
    r.drawText(font, x + (w - r.getTextWidth(font, text.second, style)) / 2, top + lh, text.second, black, style);
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

const char* ConnectionsActivity::title() const {
  return lang == LANG_TR ? tr(STR_CONN_TITLE_TR) : tr(STR_CONN_TITLE_EN);
}

// "Daily #N  ·  <date>" for headers.
void ConnectionsActivity::formatPuzzleLabel(const uint32_t date, const char* dateFormat, char* out,
                                            const int size) const {
  char number[24], when[32];
  snprintf(number, sizeof(number), tr(STR_CONN_DAILY_NUM), dailyNumber(date));
  formatDate(date, dateFormat, when, sizeof(when));
  snprintf(out, size, "%s  ·  %s", number, when);
}

// Two segments, English | Türkçe, the current one filled.
void ConnectionsActivity::drawLanguageSwitch(const int x, const int y, const bool focused) const {
  constexpr int H = 30;
  const char* names[LANG_COUNT] = {tr(STR_CONN_LANG_EN), tr(STR_CONN_LANG_TR)};
  int segW = 0;
  for (const char* name : names) segW = std::max(segW, renderer.getTextWidth(UI_10_FONT_ID, name, EpdFontFamily::BOLD));
  segW += 28;
  renderer.drawRoundedRect(x, y, 2 * segW, H, 2, H / 2, true);
  for (int i = 0; i < LANG_COUNT; i++) {
    const bool on = i == lang;
    const int sx = x + i * segW;
    if (on) renderer.fillRoundedRect(sx, y, segW, H, H / 2, Color::Black);
    const auto style = on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, names[i], style);
    renderer.drawText(UI_10_FONT_ID, sx + (segW - tw) / 2, y + (H - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      names[i], !on, style);
  }
  if (focused) drawFocus(renderer, x, y, 2 * segW, H, false);
}

void ConnectionsActivity::renderHome() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();

  drawMenuIcon(renderer, SIDE, 16, 56);
  renderer.drawText(NOTOSANS_18_FONT_ID, SIDE + 72, 2, title(), true, EpdFontFamily::BOLD);
  drawLanguageSwitch(SIDE + 72, 54, homeSelected == HOME_LANG);

  drawStatTiles(100);
  renderer.drawText(SMALL_FONT_ID, SIDE, 166, tr(STR_CONN_RECENT));
  drawRecentStrip(SIDE, 188, width - 2 * SIDE);

  const int top = 230;
  constexpr int ITEM_GAP = 10;
  constexpr int ROWS = HOME_ITEMS - HOME_TODAY;
  const int itemH = std::min(96, (hintsTop(renderer) - 12 - top - ITEM_GAP * (ROWS - 1)) / ROWS);
  const connections::Record todayRecord = resultFor(today);
  char dailyLabel[24];
  snprintf(dailyLabel, sizeof(dailyLabel), tr(STR_CONN_DAILY_NUM), dailyNumber(today));

  for (int i = HOME_TODAY; i < HOME_ITEMS; i++) {
    const int y = top + (i - HOME_TODAY) * (itemH + ITEM_GAP);
    const int x = SIDE, w = width - 2 * SIDE;
    const bool focused = i == homeSelected;
    renderer.drawRoundedRect(x, y, w, itemH, focused ? 3 : 1, 10, true);
    constexpr int ICON = 44;
    const int ix = x + 16, iy = y + (itemH - ICON) / 2;
    char title[64] = {}, line[80] = {};
    switch (i) {
      case HOME_TODAY: {
        renderer.drawRoundedRect(ix, iy + 4, ICON, ICON - 4, 2, 6, true);
        renderer.fillRoundedRect(ix, iy + 4, ICON, 12, 6, true, true, false, false, Color::Black);
        renderer.fillRect(ix + 11, iy, 4, 10, true);
        renderer.fillRect(ix + ICON - 15, iy, 4, 10, true);
        char day[4];
        snprintf(day, sizeof(day), "%u", static_cast<unsigned>(today % 100));
        const int dw = renderer.getTextWidth(UI_10_FONT_ID, day, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, ix + (ICON - dw) / 2, iy + 19, day, true, EpdFontFamily::BOLD);
        char when[32];
        formatDate(today, "%a, %b %d", when, sizeof(when));
        snprintf(title, sizeof(title), clockKnown ? tr(STR_CONN_TODAY_ON) : tr(STR_CONN_LATEST_ON), when);
        if (!packOpen) {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_NO_PACK_SD));
          break;
        }
        const bool inProgress = gameLoaded && game.puzzle().date == today && !todayRecord.played &&
                                game.result() == connections::Result::Playing &&
                                (game.solvedCount() > 0 || game.mistakes() > 0);
        if (todayRecord.played && todayRecord.lost) {
          snprintf(line, sizeof(line), "%s  ·  %s", dailyLabel, tr(STR_CONN_MISSED));
        } else if (todayRecord.played) {
          snprintf(line, sizeof(line), tr(STR_CONN_SOLVED_MISTAKES), dailyLabel, todayRecord.mistakes);
        } else if (inProgress) {
          snprintf(line, sizeof(line), tr(STR_CONN_IN_PROGRESS), dailyLabel, game.solvedCount());
        } else {
          snprintf(line, sizeof(line), "%s  ·  %s", dailyLabel, tr(STR_CONN_NEW));
        }
        if (todayRecord.played && !todayRecord.lost) {
          const int cx = x + w - 34, cy = y + itemH / 2;
          renderer.drawLine(cx - 10, cy, cx - 3, cy + 8, 4, true);
          renderer.drawLine(cx - 3, cy + 8, cx + 12, cy - 10, 4, true);
        }
        break;
      }
      case HOME_RANDOM: {
        drawDie(renderer, ix, iy, ICON);
        snprintf(title, sizeof(title), "%s", tr(STR_CONN_RANDOM));
        const int left = unplayedCount();
        if (!packOpen) {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_NO_PACK_SD));
        } else if (left > 0) {
          snprintf(line, sizeof(line), tr(STR_CONN_PAST_UNPLAYED), left);
        } else {
          snprintf(line, sizeof(line), "%s", dayIndex(today) > 0 ? tr(STR_CONN_ALL_CAUGHT_UP) : tr(STR_CONN_NO_PAST));
        }
        break;
      }
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
          formatDate(launchDate(), "%b %d, %Y", since, sizeof(since));
          snprintf(line, sizeof(line), tr(STR_CONN_DAILIES_SINCE), dailyNumber(today), since);
        } else {
          snprintf(line, sizeof(line), "%s", tr(STR_CONN_NO_PACK_SD));
        }
        break;
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
    const auto titleStyle = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_12_FONT_ID, textX, y + itemH / 2 - 28,
                      renderer.truncatedText(UI_12_FONT_ID, title, textW, titleStyle).c_str(), true, titleStyle);
    renderer.drawText(UI_10_FONT_ID, textX, y + itemH / 2 + 4,
                      renderer.truncatedText(UI_10_FONT_ID, line, textW).c_str());
  }

  const char* confirm = homeSelected == HOME_LANG ? tr(STR_CONN_SWITCH) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- board --------------------------------------------------------------------

void ConnectionsActivity::drawTile(const int tile, const int x, const int y, const int w, const int h) const {
  const bool selected = game.isSelected(tile);
  const bool focused = finaleStage == 0 && tile == cursorTile();
  if (selected) {
    renderer.fillRoundedRect(x, y, w, h, 10, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
  }
  drawFitted(renderer, fitTile(renderer, game.tileWord(tile), w - TILE_PAD), x, y, w, h, !selected);
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

  char subtitle[64];
  formatPuzzleLabel(game.puzzle().date, "%b %d, %Y", subtitle, sizeof(subtitle));
  drawHeader(title(), subtitle);
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
      // Long words step down a size or break on their own, as on the paper's board.
      drawTile(t, BOARD_SIDE + col * (tileW + GAP), l.gridY + row * (l.rowH + GAP), tileW, l.rowH);
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
  char text[64];
  formatPuzzleLabel(game.puzzle().date, "%a, %b %d %Y", text, sizeof(text));
  drawHeader(title(), text);

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
    const bool available = packOpen && dayIndex(date) >= 0 && date <= today;
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
  char when[32], line[80], state[40], number[24];
  formatDate(archiveDate, "%A, %b %d", when, sizeof(when));
  const bool available = packOpen && dayIndex(archiveDate) >= 0;
  const connections::Record record = resultFor(archiveDate);
  renderer.drawText(UI_12_FONT_ID, SIDE, infoY, when, true, EpdFontFamily::BOLD);
  if (!available) {
    snprintf(state, sizeof(state), "%s", tr(STR_CONN_NOT_IN_PACK));
  } else if (!record.played) {
    snprintf(state, sizeof(state), "%s", tr(STR_CONN_NEW));
  } else if (record.lost) {
    snprintf(state, sizeof(state), "%s", tr(STR_CONN_MISSED));
  } else {
    snprintf(state, sizeof(state), tr(STR_CONN_MISTAKES_MADE), record.mistakes);
  }
  snprintf(number, sizeof(number), tr(STR_CONN_DAILY_NUM), dailyNumber(archiveDate));
  if (available) {
    snprintf(line, sizeof(line), "%s  ·  %s", number, state);
  } else {
    snprintf(line, sizeof(line), "%s", state);
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
      mappedInput.mapLabels(tr(STR_BACK), available ? tr(STR_OPEN) : "", tr(STR_CONN_DAY_BACK), tr(STR_CONN_DAY_ON));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- statistics -----------------------------------------------------------------

void ConnectionsActivity::renderStats() const {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  drawHeader(tr(STR_WORDLE_STATS), title());

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
  }
  const bool clean = cleanNext || ++framesSinceClean >= play::FRAMES_PER_CLEAN;
  if (clean) framesSinceClean = 0;
  cleanNext = false;
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
