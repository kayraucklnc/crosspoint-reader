// Drawing for WordleActivity: 1-bit tiles, keyboard, home, stats and result card.
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "PlayCommon.h"
#include "WordleActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
using wordle::Lang;

constexpr int SIDE = 24;
constexpr int HEADER_H = 48;
constexpr int GRID_GAP = 8;
constexpr int GRID_SIDE = 60;  // keeps the grid clear of the side-button hints
constexpr int KEY_H = 54;
constexpr int KEY_VGAP = 8;
constexpr int KEY_GAP = 6;
constexpr int KB_MARGIN = 8;
constexpr int KB_BOTTOM_GAP = 10;
constexpr int NOTE_H = 32;
constexpr int NOTE_GAP = 8;
constexpr int REVEAL_SPLIT = 3;  // tiles shown by the first reveal frame
constexpr int HISTOGRAM_ROW_H = 30;
// Letters: 18 pt bold at 1x on keys; 14 pt bold doubled on tiles so Turkish
// accents (Ç Ğ İ Ö Ş Ü) stay inside a 72 px tile.
constexpr int KEY_FONT = NOTOSANS_18_FONT_ID;
constexpr int TILE_FONT = NOTOSANS_14_FONT_ID;

// Tile looks: style in the high byte, letter index (or 0xFF) in the low byte.
enum TileStyle : uint8_t { T_EMPTY, T_ACTIVE, T_NEXT, T_TYPED, T_ABSENT, T_PRESENT, T_CORRECT };
constexpr uint8_t NO_LETTER = 0xFF;
constexpr uint8_t KEY_FOCUS = 4;
constexpr uint8_t KEY_READY = 8;

constexpr StrId TITLES[wordle::TITLE_COUNT] = {StrId::STR_WORDLE_TITLE_1, StrId::STR_WORDLE_TITLE_2,
                                               StrId::STR_WORDLE_TITLE_3, StrId::STR_WORDLE_TITLE_4,
                                               StrId::STR_WORDLE_TITLE_5, StrId::STR_WORDLE_TITLE_6};

const char* langName(const Lang lang) {
  return lang == wordle::LANG_TR ? tr(STR_WORDLE_LANG_TR) : tr(STR_WORDLE_LANG_EN);
}

const EpdFontFamily* glyphFont(const GfxRenderer& r, const int fontId) {
  const auto& fonts = r.getFontMap();
  const auto it = fonts.find(fontId);
  return it == fonts.end() ? nullptr : &it->second;
}

// Draws one bold glyph scaled by an integer factor. Scaled anti-aliased edge
// pixels are ordered-dithered by their gray level so enlarged letters keep
// smooth outlines; `gray` draws a 50% checkerboard of the ink for used-up letters.
int drawGlyph(const GfxRenderer& r, const int fontId, const uint32_t cp, const int x, const int baseline,
              const int scale, const bool black, const bool gray = false) {
  static constexpr uint8_t BAYER2[2][2] = {{0, 2}, {3, 1}};
  static constexpr uint8_t SUBPIXELS[4] = {0, 1, 3, 4};  // of 4, per 2-bit gray level
  const EpdFontFamily* font = glyphFont(r, fontId);
  if (!font) return 0;
  const EpdGlyph* glyph = font->getGlyph(cp, EpdFontFamily::BOLD);
  if (!glyph) return 0;
  const EpdFontData* data = font->getData(EpdFontFamily::BOLD);
  const uint8_t* bitmap = r.getGlyphBitmap(data, glyph);
  const int x0 = x + glyph->left * scale;
  const int y0 = baseline - glyph->top * scale;
  for (int py = 0; bitmap && py < glyph->height; py++) {
    for (int px = 0; px < glyph->width; px++) {
      const int pos = py * glyph->width + px;
      const int level = data->is2Bit ? (bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 3
                                     : ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 1) * 3;
      if (level == 0 || (scale == 1 && level < 2)) continue;
      const int gx = x0 + px * scale, gy = y0 + py * scale;
      if (!gray && (level == 3 || scale == 1)) {
        r.fillRect(gx, gy, scale, scale, black);
        continue;
      }
      for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
          const bool on =
              gray ? level >= 2 && ((gx + dx + gy + dy) & 1) == 0 : BAYER2[dy & 1][dx & 1] < SUBPIXELS[level];
          if (on) r.drawPixel(gx + dx, gy + dy, black);
        }
      }
    }
  }
  return fp4::toPixel(glyph->advanceX) * scale;
}

int capHeight(const GfxRenderer& r, const int fontId = KEY_FONT) {
  const EpdFontFamily* font = glyphFont(r, fontId);
  const EpdGlyph* glyph = font ? font->getGlyph('H', EpdFontFamily::BOLD) : nullptr;
  return glyph ? glyph->top : 18;
}

// One letter centred on (cx, cy) by its ink box horizontally and cap height vertically.
void drawLetter(const GfxRenderer& r, const Lang lang, const uint8_t letter, const int cx, const int cy,
                const int scale, const bool black, const bool gray = false) {
  const uint32_t cp = wordle::letterCodepoint(lang, letter);
  const int fontId = scale > 1 ? TILE_FONT : KEY_FONT;
  const EpdFontFamily* font = glyphFont(r, fontId);
  const EpdGlyph* glyph = font ? font->getGlyph(cp, EpdFontFamily::BOLD) : nullptr;
  if (!glyph) return;
  const int x = cx - glyph->width * scale / 2 - glyph->left * scale;
  drawGlyph(r, fontId, cp, x, cy + capHeight(r, fontId) * scale / 2, scale, black, gray);
}

// ASCII digits/text in the big glyph font, centred on cx; returns the width.
int drawBigText(const GfxRenderer& r, const char* text, const int cx, const int top, const int scale,
                const bool black) {
  int width = 0;
  const EpdFontFamily* font = glyphFont(r, KEY_FONT);
  if (!font) return 0;
  for (const char* c = text; *c; c++) {
    const EpdGlyph* glyph = font->getGlyph(static_cast<uint8_t>(*c), EpdFontFamily::BOLD);
    if (glyph) width += fp4::toPixel(glyph->advanceX) * scale;
  }
  int x = cx - width / 2;
  for (const char* c = text; *c; c++) {
    x += drawGlyph(r, KEY_FONT, static_cast<uint8_t>(*c), x, top + capHeight(r) * scale, scale, black);
  }
  return width;
}

void hatch(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int step) {
  const auto clip = r.getClipRect();
  r.setClipRect(x, y, w, h);
  for (int d = -h; d < w; d += step) r.drawLine(x + d, y + h, x + d + h, y, 2, true);
  r.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}

// The five 1-bit looks shared by tiles, keys and the legend.
void drawCell(const GfxRenderer& r, const int x, const int y, const int w, const int h, const uint8_t style,
              const int radius) {
  switch (style) {
    case T_EMPTY:
    case T_ABSENT:
      r.drawRoundedRect(x, y, w, h, 1, radius, true);
      break;
    case T_ACTIVE:
    case T_NEXT:
      r.drawRoundedRect(x, y, w, h, 2, radius, true);
      break;
    case T_TYPED:
      r.drawRoundedRect(x, y, w, h, 3, radius, true);
      break;
    case T_PRESENT: {
      // A hatched frame around a clear centre: reads as "half filled" and keeps the letter crisp.
      const int band = std::max(5, std::min(w, h) / 8);
      const int inner = std::max(2, radius - 2);
      r.drawRoundedRect(x, y, w, h, 2, radius, true);
      hatch(r, x + 2, y + 2, w - 4, h - 4, std::max(5, w / 10));
      r.fillRoundedRect(x + band, y + band, w - 2 * band, h - 2 * band, inner, Color::White);
      r.drawRoundedRect(x + band, y + band, w - 2 * band, h - 2 * band, 1, inner, true);
      break;
    }
    case T_CORRECT:
      r.fillRoundedRect(x, y, w, h, radius, Color::Black);
      break;
  }
}

struct GameLayout {
  int tile, gridX, gridY, noteY, kbY;
};

GameLayout gameLayout(const GfxRenderer& r) {
  const int width = r.getScreenWidth();
  const int hintsTop = r.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
  GameLayout l{};
  l.kbY = hintsTop - KB_BOTTOM_GAP - (wordle::KEY_ROWS * KEY_H + (wordle::KEY_ROWS - 1) * KEY_VGAP);
  l.noteY = l.kbY - NOTE_GAP - NOTE_H;
  const int gridTop = HEADER_H, gridBottom = l.noteY - NOTE_GAP;
  const int byHeight = (gridBottom - gridTop - (wordle::MAX_GUESSES - 1) * GRID_GAP) / wordle::MAX_GUESSES;
  const int byWidth = (width - 2 * GRID_SIDE - (wordle::WORD_LEN - 1) * GRID_GAP) / wordle::WORD_LEN;
  l.tile = std::min(byHeight, byWidth);
  l.gridX = (width - (wordle::WORD_LEN * l.tile + (wordle::WORD_LEN - 1) * GRID_GAP)) / 2;
  l.gridY =
      gridTop + (gridBottom - gridTop - (wordle::MAX_GUESSES * l.tile + (wordle::MAX_GUESSES - 1) * GRID_GAP)) / 2;
  return l;
}

int keyUnits(const uint8_t key) { return key == wordle::KEY_ENTER ? 2 : 1; }

void clearRect(const GfxRenderer& r, const int x, const int y, const int w, const int h) {
  r.fillRect(x, y, w, h, false);
}
}  // namespace

// ---- keyboard geometry -------------------------------------------------------

int WordleActivity::keyPitch() const {
  int units = 1;
  for (int row = 0; row < wordle::KEY_ROWS; row++) {
    const wordle::KeyRow& keys = wordle::keyboard(lang)[row];
    int rowUnits = 0;
    for (int i = 0; i < keys.count; i++) rowUnits += keyUnits(keys.keys[i]);
    units = std::max(units, rowUnits);
  }
  return (renderer.getScreenWidth() - 2 * KB_MARGIN) / units;
}

int WordleActivity::keyX(const int row, const int col, int* width) const {
  const wordle::KeyRow& keys = wordle::keyboard(lang)[row];
  const int pitch = keyPitch();
  int rowUnits = 0, before = 0;
  for (int i = 0; i < keys.count; i++) {
    if (i == col) before = rowUnits;
    rowUnits += keyUnits(keys.keys[i]);
  }
  *width = keyUnits(keys.keys[col]) * pitch - KEY_GAP;
  return (renderer.getScreenWidth() - rowUnits * pitch) / 2 + before * pitch + KEY_GAP / 2;
}

// ---- game --------------------------------------------------------------------

uint16_t WordleActivity::tileLook(const int row, const int col) const {
  const wordle::Board& b = board();
  const auto look = [](const uint8_t style, const uint8_t letter) {
    return static_cast<uint16_t>(style << 8 | letter);
  };
  if (row < b.count) {
    uint8_t letters[wordle::WORD_LEN];
    wordle::unpack(b.guesses[row], letters);
    if (revealStage != 0 && row == b.count - 1 && col >= REVEAL_SPLIT) return look(T_TYPED, letters[col]);
    uint8_t marks[wordle::WORD_LEN];
    wordle::score(b.guesses[row], b.answer, marks);
    const uint8_t style = marks[col] == wordle::MARK_CORRECT   ? T_CORRECT
                          : marks[col] == wordle::MARK_PRESENT ? T_PRESENT
                                                               : T_ABSENT;
    return look(style, letters[col]);
  }
  if (row == b.count && b.state == wordle::GAME_PLAYING) {
    if (col < inputLength) return look(T_TYPED, input[col]);
    return look(col == inputLength ? T_NEXT : T_ACTIVE, NO_LETTER);
  }
  return look(T_EMPTY, NO_LETTER);
}

void WordleActivity::keyMarks(uint8_t* marks) const {
  const wordle::Board& b = board();
  const int rows = b.count - (revealStage != 0 ? 1 : 0);
  for (int row = 0; row < rows; row++) {
    uint8_t letters[wordle::WORD_LEN], rowMarks[wordle::WORD_LEN];
    wordle::unpack(b.guesses[row], letters);
    wordle::score(b.guesses[row], b.answer, rowMarks);
    for (int i = 0; i < wordle::WORD_LEN; i++) marks[letters[i]] = std::max(marks[letters[i]], rowMarks[i]);
  }
}

void WordleActivity::drawTile(const int row, const int col, const uint16_t look) const {
  const GameLayout l = gameLayout(renderer);
  const int x = l.gridX + col * (l.tile + GRID_GAP);
  const int y = l.gridY + row * (l.tile + GRID_GAP);
  const uint8_t style = look >> 8, letter = look & 0xFF;
  clearRect(renderer, x - 2, y - 2, l.tile + 4, l.tile + 4);
  drawCell(renderer, x, y, l.tile, l.tile, style, 8);
  if (style == T_NEXT) renderer.fillRect(x + l.tile / 4, y + l.tile - 12, l.tile / 2, 4, true);
  if (letter == NO_LETTER) return;
  drawLetter(renderer, lang, letter, x + l.tile / 2, y + l.tile / 2, l.tile >= 56 ? 2 : 1, style != T_CORRECT,
             style == T_ABSENT);
}

void WordleActivity::drawKey(const int row, const int col, const uint8_t look) const {
  const GameLayout l = gameLayout(renderer);
  int w = 0;
  const int x = keyX(row, col, &w);
  const int y = l.kbY + row * (KEY_H + KEY_VGAP);
  clearRect(renderer, x - KEY_GAP / 2, y - KEY_GAP / 2, w + KEY_GAP, KEY_H + KEY_GAP);
  const uint8_t key = wordle::keyboard(lang)[row].keys[col];
  const uint8_t mark = look & 3;
  if (key == wordle::KEY_ENTER) {
    const bool ready = (look & KEY_READY) != 0;
    drawCell(renderer, x, y, w, KEY_H, ready ? T_CORRECT : T_TYPED, 6);
    const char* label = tr(STR_WORDLE_ENTER_KEY);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (w - tw) / 2, y + (KEY_H - renderer.getLineHeight(UI_12_FONT_ID)) / 2, label,
                      !ready, EpdFontFamily::BOLD);
  } else {
    const uint8_t style = mark == wordle::MARK_CORRECT   ? T_CORRECT
                          : mark == wordle::MARK_PRESENT ? T_PRESENT
                          : mark == wordle::MARK_ABSENT  ? T_ABSENT
                                                         : T_EMPTY;
    if (style != T_ABSENT) drawCell(renderer, x, y, w, KEY_H, style, 6);
    drawLetter(renderer, lang, key, x + w / 2, y + KEY_H / 2, 1, style != T_CORRECT, style == T_ABSENT);
  }
  if (look & KEY_FOCUS) {
    // A heavy frame in the key gap; solid keys also get a white inner ring so focus reads on black.
    renderer.drawRoundedRect(x - KEY_GAP / 2, y - KEY_GAP / 2, w + KEY_GAP, KEY_H + KEY_GAP, KEY_GAP / 2, 8, true);
    const bool dark = mark == wordle::MARK_CORRECT || (key == wordle::KEY_ENTER && (look & KEY_READY));
    if (dark) renderer.drawRoundedRect(x + 3, y + 3, w - 6, KEY_H - 6, 2, 4, false);
    renderer.fillRect(x + w / 2 - 8, y + KEY_H - 8, 16, 3, !dark);
  }
}

void WordleActivity::drawNote() const {
  const GameLayout l = gameLayout(renderer);
  const int width = renderer.getScreenWidth();
  clearRect(renderer, 0, l.noteY - 2, width, NOTE_H + 4);
  if (note == Note::LEGEND) {
    static constexpr uint8_t STYLES[3] = {T_CORRECT, T_PRESENT, T_ABSENT};
    static constexpr StrId LABELS[3] = {StrId::STR_WORDLE_LEGEND_CORRECT, StrId::STR_WORDLE_LEGEND_PRESENT,
                                        StrId::STR_WORDLE_LEGEND_ABSENT};
    const int slot = (width - 2 * SIDE) / 3;
    for (int i = 0; i < 3; i++) {
      const int x = SIDE + i * slot;
      drawCell(renderer, x, l.noteY + 2, 28, 28, STYLES[i], 5);
      drawLetter(renderer, wordle::LANG_EN, 0, x + 14, l.noteY + 16, 1, STYLES[i] != T_CORRECT, STYLES[i] == T_ABSENT);
      renderer.drawText(SMALL_FONT_ID, x + 36, l.noteY + 8, I18N.get(LABELS[i]));
    }
    return;
  }
  const char* text = note == Note::TOO_SHORT  ? tr(STR_WORDLE_TOO_SHORT)
                     : note == Note::NOT_WORD ? tr(STR_WORDLE_NOT_WORD)
                     : note == Note::NO_LISTS ? tr(STR_WORDLE_NO_LISTS)
                                              : nullptr;
  if (!text) return;
  const int tw = renderer.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  const int pw = std::min(width - 2 * SIDE, tw + 36);
  renderer.fillRoundedRect((width - pw) / 2, l.noteY, pw, NOTE_H, NOTE_H / 2, Color::Black);
  renderer.drawText(UI_10_FONT_ID, (width - tw) / 2, l.noteY + (NOTE_H - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    text, false, EpdFontFamily::BOLD);
}

void WordleActivity::drawGameHints() const {
  const int width = renderer.getScreenWidth();
  const int hintsTop = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
  clearRect(renderer, 0, hintsTop, width, renderer.getScreenHeight() - hintsTop);
  const bool submits =
      inputLength == wordle::WORD_LEN || wordle::keyboard(lang)[keyRow].keys[keyCol] == wordle::KEY_ENTER;
  const auto labels = mappedInput.mapLabels(inputLength == 0 ? tr(STR_WORDLE_MENU) : tr(STR_WORDLE_HINT_DELETE),
                                            submits ? tr(STR_WORDLE_SUBMIT) : tr(STR_WORDLE_HINT_TYPE),
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WordleActivity::drawHeader(const char* title, const char* subtitle) const {
  const int width = renderer.getScreenWidth();
  renderer.drawText(UI_12_FONT_ID, SIDE, 12, title, true, EpdFontFamily::BOLD);
  if (!subtitle) return;
  const int w = renderer.getTextWidth(UI_10_FONT_ID, subtitle);
  renderer.drawText(UI_10_FONT_ID, width - SIDE - w, 15, subtitle);
}

void WordleActivity::renderGame() {
  const wordle::Board& b = board();
  if (fullRedraw) {
    renderer.clearScreen();
    for (auto& row : shownTiles) std::fill(std::begin(row), std::end(row), STALE);
    for (auto& row : shownKeys) std::fill(std::begin(row), std::end(row), 0xFF);
    shownNote = shownHints = 0xFF;
    char title[48], status[48];
    snprintf(title, sizeof(title), "%s  ·  %s", daily ? tr(STR_WORDLE_DAILY) : tr(STR_WORDLE_RANDOM), langName(lang));
    const auto level = wordle::levelFor(progress.stats[lang].xp);
    const uint16_t streak = wordle::currentStreak(progress.stats[lang], today);
    if (daily && streak > 0) {
      snprintf(status, sizeof(status), "%s %d  ·  %s %d", tr(STR_WORDLE_LEVEL), level.level, tr(STR_WORDLE_STREAK),
               streak);
    } else {
      snprintf(status, sizeof(status), "%s %d", tr(STR_WORDLE_LEVEL), level.level);
    }
    drawHeader(title, status);
    GUI.drawSideButtonHints(renderer, tr(STR_WORDLE_ROW_UP), tr(STR_WORDLE_ROW_DOWN));
  }
  for (int row = 0; row < wordle::MAX_GUESSES; row++) {
    for (int col = 0; col < wordle::WORD_LEN; col++) {
      const uint16_t look = tileLook(row, col);
      if (look == shownTiles[row][col]) continue;
      drawTile(row, col, look);
      shownTiles[row][col] = look;
    }
  }
  uint8_t marks[32] = {};
  keyMarks(marks);
  const bool ready = inputLength == wordle::WORD_LEN && b.state == wordle::GAME_PLAYING;
  for (int row = 0; row < wordle::KEY_ROWS; row++) {
    const wordle::KeyRow& keys = wordle::keyboard(lang)[row];
    for (int col = 0; col < keys.count; col++) {
      const uint8_t key = keys.keys[col];
      uint8_t look = key == wordle::KEY_ENTER ? (ready ? KEY_READY : 0) : marks[key];
      if (row == keyRow && col == keyCol && b.state == wordle::GAME_PLAYING) look |= KEY_FOCUS;
      if (look == shownKeys[row][col]) continue;
      drawKey(row, col, look);
      shownKeys[row][col] = look;
    }
  }
  if (shownNote != static_cast<uint8_t>(note)) {
    drawNote();
    shownNote = static_cast<uint8_t>(note);
  }
  const bool submits =
      inputLength == wordle::WORD_LEN || wordle::keyboard(lang)[keyRow].keys[keyCol] == wordle::KEY_ENTER;
  const uint8_t hints = (inputLength == 0 ? 1 : 0) | (submits ? 2 : 0);
  if (shownHints != hints) {
    drawGameHints();
    shownHints = hints;
  }
}

// ---- shared widgets ----------------------------------------------------------

// Level badge, title and XP progress bar in a band `width` wide and 56 px tall.
void WordleActivity::drawLevelBar(const int x, const int y, const int width, const uint32_t xp,
                                  const bool large) const {
  const auto level = wordle::levelFor(xp);
  constexpr int BADGE = 56;
  renderer.fillRoundedRect(x, y, BADGE, BADGE, BADGE / 2, Color::Black);
  char text[40];
  snprintf(text, sizeof(text), "%d", level.level);
  drawBigText(renderer, text, x + BADGE / 2, y + BADGE / 2 - capHeight(renderer) / 2, 1, false);
  const int tx = x + BADGE + 14;
  const int tw = width - BADGE - 14;
  snprintf(text, sizeof(text), "%s %d  ·  %s", tr(STR_WORDLE_LEVEL), level.level,
           I18N.get(TITLES[wordle::titleIndex(level.level)]));
  renderer.drawText(large ? UI_12_FONT_ID : UI_10_FONT_ID, tx, y + 2, text, true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), "%lu / %lu %s", static_cast<unsigned long>(level.into),
           static_cast<unsigned long>(level.span), tr(STR_WORDLE_XP));
  const int xpw = renderer.getTextWidth(SMALL_FONT_ID, text);
  renderer.drawText(SMALL_FONT_ID, x + width - xpw, y + (large ? 8 : 4), text);
  constexpr int BAR_H = 14;
  const int barY = y + BADGE - BAR_H - 4;
  renderer.drawRoundedRect(tx, barY, tw, BAR_H, 2, BAR_H / 2, true);
  const int fill = static_cast<int>((tw - 6) * static_cast<uint64_t>(level.into) / std::max<uint32_t>(level.span, 1));
  if (fill > 0)
    renderer.fillRoundedRect(tx + 3, barY + 3, std::max(fill, BAR_H - 6), BAR_H - 6, (BAR_H - 6) / 2, Color::Black);
}

void WordleActivity::drawHistogram(const int x, const int y, const int width, const wordle::LangStats& s,
                                   const int highlight) const {
  constexpr int ROW_H = HISTOGRAM_ROW_H;
  constexpr int LABEL_W = 22;
  uint16_t most = 1;
  for (const uint16_t n : s.dist) most = std::max(most, n);
  for (int i = 0; i < wordle::MAX_GUESSES; i++) {
    const int ry = y + i * ROW_H;
    char text[8];
    snprintf(text, sizeof(text), "%d", i + 1);
    renderer.drawText(UI_10_FONT_ID, x, ry + 1, text, true, EpdFontFamily::BOLD);
    snprintf(text, sizeof(text), "%u", s.dist[i]);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, text, EpdFontFamily::BOLD);
    const int span = width - LABEL_W;
    const int barW = std::max(tw + 14, static_cast<int>(span * static_cast<uint32_t>(s.dist[i]) / most));
    const bool strong = i == highlight || (highlight < 0 && s.dist[i] == most && most > 0);
    if (strong) {
      renderer.fillRect(x + LABEL_W, ry + 2, barW, ROW_H - 6, true);
    } else {
      renderer.drawRect(x + LABEL_W, ry + 2, barW, ROW_H - 6, 2, true);
    }
    renderer.drawText(SMALL_FONT_ID, x + LABEL_W + barW - tw - 7, ry + 4, text, !strong, EpdFontFamily::BOLD);
  }
}

void WordleActivity::drawMiniBoard(const int x, const int y, const int cell) const {
  const wordle::Board& b = board();
  for (int row = 0; row < wordle::MAX_GUESSES; row++) {
    uint8_t marks[wordle::WORD_LEN] = {};
    if (row < b.count) wordle::score(b.guesses[row], b.answer, marks);
    for (int col = 0; col < wordle::WORD_LEN; col++) {
      const uint8_t style = row >= b.count                       ? T_EMPTY
                            : marks[col] == wordle::MARK_CORRECT ? T_CORRECT
                            : marks[col] == wordle::MARK_PRESENT ? T_PRESENT
                                                                 : T_ABSENT;
      const int cx = x + col * (cell + 4), cy = y + row * (cell + 4);
      if (style == T_PRESENT) {
        renderer.drawRect(cx, cy, cell, cell, 2, true);
        hatch(renderer, cx + 2, cy + 2, cell - 4, cell - 4, 5);
      } else {
        drawCell(renderer, cx, cy, cell, cell, style, 3);
      }
    }
  }
}

// ---- home --------------------------------------------------------------------

void WordleActivity::renderHome() const {
  const int width = renderer.getScreenWidth();
  const int hintsTop = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
  renderer.clearScreen();

  // Logo: W O R D L E in the game's own tile looks.
  static constexpr uint8_t LOGO_STYLES[6] = {T_CORRECT, T_PRESENT, T_TYPED, T_CORRECT, T_ABSENT, T_PRESENT};
  static constexpr uint8_t LOGO_LETTERS[6] = {22, 14, 17, 3, 11, 4};
  constexpr int LOGO = 50, LOGO_GAP = 6;
  const int logoX = (width - (6 * LOGO + 5 * LOGO_GAP)) / 2;
  for (int i = 0; i < 6; i++) {
    const int x = logoX + i * (LOGO + LOGO_GAP);
    drawCell(renderer, x, 20, LOGO, LOGO, LOGO_STYLES[i], 7);
    drawLetter(renderer, wordle::LANG_EN, LOGO_LETTERS[i], x + LOGO / 2, 20 + LOGO / 2, 1, LOGO_STYLES[i] != T_CORRECT,
               LOGO_STYLES[i] == T_ABSENT);
  }

  // Overall level across both languages.
  const uint32_t totalXp = progress.stats[0].xp + progress.stats[1].xp;
  drawLevelBar(SIDE + 4, 94, width - 2 * SIDE - 8, totalXp, true);

  const int top = 168;
  constexpr int GAP = 10;
  const int itemH = std::min(96, (hintsTop - 12 - top - GAP * (HOME_ITEMS - 1)) / HOME_ITEMS);
  for (int i = 0; i < HOME_ITEMS; i++) {
    const int y = top + i * (itemH + GAP);
    const bool focused = i == homeSelected;
    const int x = SIDE, w = width - 2 * SIDE;
    renderer.drawRoundedRect(x, y, w, itemH, focused ? 3 : 1, 10, true);
    const bool isDaily = i < 2, isStats = i == 4;
    const Lang itemLang = (i == 1 || i == 3) ? wordle::LANG_TR : wordle::LANG_EN;
    const wordle::LangStats& s = progress.stats[itemLang];

    // Icon: calendar page, die, or bar chart.
    constexpr int ICON = 48;
    const int ix = x + 16, iy = y + (itemH - ICON) / 2;
    if (isStats) {
      static constexpr int BARS[3] = {20, 34, 46};
      for (int k = 0; k < 3; k++) renderer.fillRect(ix + 4 + k * 15, iy + ICON - BARS[k], 11, BARS[k], true);
    } else if (isDaily) {
      renderer.drawRoundedRect(ix, iy + 4, ICON, ICON - 4, 2, 6, true);
      renderer.fillRoundedRect(ix, iy + 4, ICON, 12, 6, true, true, false, false, Color::Black);
      renderer.fillRect(ix + 12, iy, 4, 10, true);
      renderer.fillRect(ix + ICON - 16, iy, 4, 10, true);
      const char* code = itemLang == wordle::LANG_TR ? "TR" : "EN";
      const int cw = renderer.getTextWidth(UI_10_FONT_ID, code, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, ix + (ICON - cw) / 2, iy + 20, code, true, EpdFontFamily::BOLD);
    } else {
      renderer.drawRoundedRect(ix + 2, iy + 2, ICON - 4, ICON - 4, 2, 8, true);
      static constexpr int8_t PIPS[5][2] = {{-1, -1}, {1, -1}, {0, 0}, {-1, 1}, {1, 1}};
      for (const auto& p : PIPS) {
        renderer.fillRoundedRect(ix + ICON / 2 + p[0] * 11 - 4, iy + ICON / 2 + p[1] * 11 - 4, 8, 8, 4, Color::Black);
      }
    }

    char title[48], line[64];
    if (isStats) {
      snprintf(title, sizeof(title), "%s", tr(STR_WORDLE_STATS));
      snprintf(line, sizeof(line), "%s: %u", tr(STR_WORDLE_DAILIES_WON),
               static_cast<unsigned>(progress.stats[0].dailyWon + progress.stats[1].dailyWon));
    } else {
      snprintf(title, sizeof(title), "%s  ·  %s", isDaily ? tr(STR_WORDLE_DAILY) : tr(STR_WORDLE_RANDOM),
               langName(itemLang));
      const wordle::Board& b = isDaily ? progress.daily[itemLang] : progress.random[itemLang];
      const uint16_t streak = wordle::currentStreak(s, today);
      if (isDaily && today < 0) {
        snprintf(line, sizeof(line), "%s", tr(STR_WORDLE_NO_CLOCK));
      } else if (isDaily && b.started && b.day == today && b.state == wordle::GAME_WON) {
        char solved[32];
        snprintf(solved, sizeof(solved), tr(STR_WORDLE_SOLVED_IN), b.count);
        snprintf(line, sizeof(line), "%s  ·  %s %u", solved, tr(STR_WORDLE_STREAK), streak);
      } else if (isDaily && b.started && b.day == today && b.state == wordle::GAME_LOST) {
        snprintf(line, sizeof(line), "%s", tr(STR_WORDLE_MISSED));
      } else if (b.started && b.state == wordle::GAME_PLAYING && b.count > 0 && (!isDaily || b.day == today)) {
        snprintf(line, sizeof(line), "%s  ·  %d/%d", tr(STR_WORDLE_IN_PROGRESS), b.count, wordle::MAX_GUESSES);
      } else if (isDaily && streak > 0) {
        snprintf(line, sizeof(line), "%s  ·  %s %u", tr(STR_WORDLE_NEW_DAILY), tr(STR_WORDLE_STREAK), streak);
      } else {
        snprintf(line, sizeof(line), "%s", isDaily ? tr(STR_WORDLE_NEW_DAILY) : tr(STR_WORDLE_PRACTICE));
      }
      // Solved today: a check mark on the right.
      if (isDaily && b.started && b.day == today && b.state == wordle::GAME_WON) {
        const int cx = x + w - 34, cy = y + itemH / 2;
        renderer.drawLine(cx - 10, cy, cx - 3, cy + 8, 4, true);
        renderer.drawLine(cx - 3, cy + 8, cx + 12, cy - 10, 4, true);
      } else if (!isStats) {
        char lv[16];
        snprintf(lv, sizeof(lv), "%s %d", tr(STR_WORDLE_LEVEL), wordle::levelFor(s.xp).level);
        const int lw = renderer.getTextWidth(SMALL_FONT_ID, lv, EpdFontFamily::BOLD);
        renderer.drawText(SMALL_FONT_ID, x + w - 16 - lw, y + 12, lv, true, EpdFontFamily::BOLD);
      }
    }
    const int textX = ix + ICON + 26;
    const int textW = x + w - 60 - textX;
    renderer.drawText(UI_12_FONT_ID, textX, y + itemH / 2 - 28, title, true,
                      focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, textX, y + itemH / 2 + 4,
                      renderer.truncatedText(UI_10_FONT_ID, line, textW).c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- stats -------------------------------------------------------------------

void WordleActivity::renderStats() const {
  const int width = renderer.getScreenWidth();
  const wordle::LangStats& s = progress.stats[statsLang];
  renderer.clearScreen();
  drawHeader(tr(STR_WORDLE_STATS), nullptr);

  // Language tabs.
  const int tabW = (width - 2 * SIDE) / 2;
  for (int i = 0; i < wordle::LANG_COUNT; i++) {
    const int x = SIDE + i * tabW;
    const bool on = i == statsLang;
    if (on) {
      renderer.fillRoundedRect(x, 50, tabW, 40, 8, Color::Black);
    } else {
      renderer.drawRoundedRect(x, 50, tabW, 40, 1, 8, true);
    }
    const char* name = langName(static_cast<Lang>(i));
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, name, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (tabW - tw) / 2, 57, name, !on, EpdFontFamily::BOLD);
  }

  drawLevelBar(SIDE + 4, 112, width - 2 * SIDE - 8, s.xp, true);

  // Hero: dailies won.
  char text[48];
  const int heroY = 196;
  renderer.drawRoundedRect(SIDE, heroY, width - 2 * SIDE, 128, 2, 12, true);
  snprintf(text, sizeof(text), "%u", s.dailyWon);
  drawBigText(renderer, text, width / 2, heroY + 20, 2, true);
  renderer.drawCenteredText(UI_12_FONT_ID, heroY + 88, tr(STR_WORDLE_DAILIES_WON), true, EpdFontFamily::BOLD);

  // Four stat tiles.
  const int pct = s.dailyPlayed ? (s.dailyWon * 100 + s.dailyPlayed / 2) / s.dailyPlayed : 0;
  const unsigned values[4] = {s.dailyPlayed, static_cast<unsigned>(pct), wordle::currentStreak(s, today), s.bestStreak};
  static constexpr StrId LABELS[4] = {StrId::STR_WORDLE_PLAYED, StrId::STR_WORDLE_WIN_PCT, StrId::STR_WORDLE_STREAK,
                                      StrId::STR_WORDLE_BEST_STREAK};
  const int cellW = (width - 2 * SIDE) / 4;
  const int statY = heroY + 150;
  for (int i = 0; i < 4; i++) {
    const int cx = SIDE + i * cellW + cellW / 2;
    snprintf(text, sizeof(text), "%u", values[i]);
    drawBigText(renderer, text, cx, statY, 1, true);
    const char* label = I18N.get(LABELS[i]);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, cx - std::min(lw, cellW - 4) / 2, statY + 34,
                      renderer.truncatedText(SMALL_FONT_ID, label, cellW - 4).c_str());
  }

  const int distY = statY + 78;
  renderer.drawText(UI_10_FONT_ID, SIDE, distY, tr(STR_WORDLE_DISTRIBUTION), true, EpdFontFamily::BOLD);
  drawHistogram(SIDE, distY + 30, width - 2 * SIDE, s, -1);

  snprintf(text, sizeof(text), "%s: %u / %u", tr(STR_WORDLE_RANDOM_WON), s.randomWon, s.randomPlayed);
  renderer.drawText(UI_10_FONT_ID, SIDE, distY + 30 + wordle::MAX_GUESSES * HISTOGRAM_ROW_H + 12, text);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", langName(wordle::LANG_EN), langName(wordle::LANG_TR));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---- result card -------------------------------------------------------------

void WordleActivity::renderResult() const {
  const int width = renderer.getScreenWidth();
  const wordle::Board& b = board();
  const wordle::LangStats& s = progress.stats[lang];
  const bool won = b.state == wordle::GAME_WON;
  renderer.clearScreen();
  char text[64];
  snprintf(text, sizeof(text), "%s  ·  %s", daily ? tr(STR_WORDLE_DAILY) : tr(STR_WORDLE_RANDOM), langName(lang));
  drawHeader(text, nullptr);
  const int top = daily ? 0 : 80;  // random cards have no histogram: centre them

  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, top + 60, won ? tr(STR_WORDLE_WON) : tr(STR_WORDLE_LOST), true,
                            EpdFontFamily::BOLD);

  // The answer in solid tiles.
  constexpr int TILE = 64, GAP = 8;
  uint8_t letters[wordle::WORD_LEN];
  wordle::unpack(b.answer, letters);
  const int ax = (width - (wordle::WORD_LEN * TILE + (wordle::WORD_LEN - 1) * GAP)) / 2;
  for (int i = 0; i < wordle::WORD_LEN; i++) {
    const int x = ax + i * (TILE + GAP);
    drawCell(renderer, x, top + 112, TILE, TILE, T_CORRECT, 8);
    drawLetter(renderer, lang, letters[i], x + TILE / 2, top + 112 + TILE / 2, 2, false);
  }

  // Mini board on the left, XP and streak on the right.
  constexpr int CELL = 18;
  const int boardY = top + 214;
  drawMiniBoard(SIDE + 8, boardY, CELL);
  const int rx = SIDE + 8 + 5 * (CELL + 4) + 24;
  const int rw = width - SIDE - rx;
  int ry = boardY - 4;
  if (xpGained > 0) {
    snprintf(text, sizeof(text), "+%lu %s", static_cast<unsigned long>(xpGained), tr(STR_WORDLE_XP));
    renderer.drawText(NOTOSANS_18_FONT_ID, rx, ry, text, true, EpdFontFamily::BOLD);
    ry += 46;
  }
  if (leveledUp) {
    const char* up = tr(STR_WORDLE_LEVEL_UP);
    const int uw = renderer.getTextWidth(UI_10_FONT_ID, up, EpdFontFamily::BOLD) + 24;
    renderer.fillRoundedRect(rx, ry, uw, 28, 14, Color::Black);
    renderer.drawText(UI_10_FONT_ID, rx + 12, ry + (28 - renderer.getLineHeight(UI_10_FONT_ID)) / 2, up, false,
                      EpdFontFamily::BOLD);
    ry += 40;
  }
  if (daily) {
    snprintf(text, sizeof(text), "%s %u  ·  %s %u", tr(STR_WORDLE_STREAK), wordle::currentStreak(s, today),
             tr(STR_WORDLE_BEST_STREAK), s.bestStreak);
    renderer.drawText(UI_10_FONT_ID, rx, ry, renderer.truncatedText(UI_10_FONT_ID, text, rw).c_str());
    renderer.drawText(UI_10_FONT_ID, rx, ry + 26, tr(STR_WORDLE_TOMORROW), true, EpdFontFamily::BOLD);
  } else {
    snprintf(text, sizeof(text), "%u / %u", s.randomWon, s.randomPlayed);
    renderer.drawText(NOTOSANS_18_FONT_ID, rx, ry, text, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, rx, ry + 42,
                      renderer.truncatedText(UI_10_FONT_ID, tr(STR_WORDLE_RANDOM_WON), rw).c_str());
  }

  const int levelY = boardY + 6 * (CELL + 4) + 30;
  drawLevelBar(SIDE + 4, levelY, width - 2 * SIDE - 8, s.xp, false);

  // The guess distribution belongs to dailies; random games only track wins.
  if (daily) {
    const int distY = levelY + 84;
    renderer.drawText(UI_10_FONT_ID, SIDE, distY, tr(STR_WORDLE_DISTRIBUTION), true, EpdFontFamily::BOLD);
    drawHistogram(SIDE, distY + 30, width - 2 * SIDE, s, won ? b.count - 1 : -1);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), daily ? tr(STR_WORDLE_STATS) : tr(STR_WORDLE_NEW_WORD), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WordleActivity::render(RenderLock&& lock) {
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
    case Screen::STATS:
      renderStats();
      break;
  }
  fullRedraw = false;
  const HalDisplay::RefreshMode mode = cleanNext ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  cleanNext = false;
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
