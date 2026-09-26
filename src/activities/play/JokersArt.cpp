#include "JokersArt.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "JokersRules.h"
#include "fontIds.h"

namespace jokers::art {
namespace {
constexpr const char* PACK_PATH = "/.crosspoint/games/jokers/art.bin";
constexpr int HEADER = 8;     // "JKA1" + u16 count + u16 reserved
constexpr int ENTRY = 12;     // u32 offset, u32 length, u16 w, u16 h
constexpr int GLYPH_GAP = 0;  // pack glyphs carry their own side bearings
// The pack has no '.', so decimals get a square dot on the digit baseline.
constexpr int DOT_TOP = 41, DOT_SIZE = 9, DOT_ADVANCE = 13;
constexpr int FALLBACK_BIG_FONT = NOTOSANS_18_FONT_ID;
constexpr int FALLBACK_BIG_HEIGHT = 44;  // a line of the fallback font, with room for descenders
constexpr const char* RANKS[13] = {"2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"};

uint32_t readU32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
int readU16(const uint8_t* p) { return p[0] | p[1] << 8; }

int glyphFor(const char c) {
  if (c >= '0' && c <= '9') return GLYPH_BASE + (c - '0');
  switch (c) {
    case ',':
      return GLYPH_BASE + 10;
    case 'x':
      return GLYPH_BASE + 11;
    case '=':
      return GLYPH_BASE + 12;
    case '+':
      return GLYPH_BASE + 13;
    case '$':
      return GLYPH_BASE + 14;
    default:
      return -1;
  }
}

void framedRect(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int radius) {
  r.fillRoundedRect(x, y, w, h, radius, Color::White);
  r.drawRoundedRect(x, y, w, h, 2, radius, true);
}
}  // namespace

bool Pack::open() {
  close();
  if (!Storage.exists(PACK_PATH) || !Storage.openFileForRead("JOKERS", PACK_PATH, file)) return false;
  uint8_t header[HEADER];
  if (file.read(header, HEADER) != HEADER || memcmp(header, "JKA1", 4) != 0 || readU16(header + 4) != ASSET_COUNT) {
    LOG_ERR("JOKERS", "Art pack invalid: %s", PACK_PATH);
    file.close();
    return false;
  }
  // 1.4 KB: the whole index stays resident so draws need one seek each.
  index = makeUniqueNoThrow<uint8_t[]>(ASSET_COUNT * ENTRY);
  if (!index) {
    LOG_ERR("JOKERS", "OOM: art index");
    file.close();
    return false;
  }
  if (file.read(index.get(), ASSET_COUNT * ENTRY) != ASSET_COUNT * ENTRY) {
    index.reset();
    file.close();
    return false;
  }
  return true;
}

void Pack::close() {
  index.reset();
  if (file.isOpen()) file.close();
}

int Pack::width(const int id) const {
  return ready() && id >= 0 && id < ASSET_COUNT ? readU16(index.get() + id * ENTRY + 8) : 0;
}

int Pack::height(const int id) const {
  return ready() && id >= 0 && id < ASSET_COUNT ? readU16(index.get() + id * ENTRY + 10) : 0;
}

bool Pack::draw(const GfxRenderer& renderer, const int id, const int x, const int y, const bool invert) const {
  if (!ready() || id < 0 || id >= ASSET_COUNT) return false;
  const uint8_t* entry = index.get() + id * ENTRY;
  const uint32_t offset = readU32(entry), length = readU32(entry + 4);
  const int w = readU16(entry + 8);
  const uint32_t total = static_cast<uint32_t>(w) * readU16(entry + 10);
  if (length == 0 || total == 0 || !file.seek(offset)) return false;
  uint8_t chunk[192];
  uint32_t pos = 0;
  for (uint32_t done = 0; done < length;) {
    const int n = file.read(chunk, std::min<uint32_t>(sizeof(chunk), length - done));
    if (n <= 0) return false;
    done += n;
    for (int i = 0; i < n; i++) {
      const int state = chunk[i] >> 6;
      int run = (chunk[i] & 63) + 1;
      if (pos + run > total) return false;  // corrupt data: never paint outside the asset
      while (run > 0) {
        const int col = static_cast<int>(pos % w), row = static_cast<int>(pos / w);
        const int span = std::min(run, w - col);
        if (state != 0) renderer.fillRect(x + col, y + row, span, 1, (state == 2) != invert);
        pos += span;
        run -= span;
      }
    }
  }
  return true;
}

void drawSuit(const GfxRenderer& r, const int suit, const int cx, const int cy, const int radius, const bool black) {
  const auto shape = [&](const int rad, const bool ink) {
    const Color color = ink ? Color::Black : Color::White;
    const auto disc = [&](const int x, const int y, const int d) {
      r.fillRoundedRect(x - d, y - d, d * 2, d * 2, d, color);
    };
    switch (suit) {
      case 0: {  // spade
        disc(cx - rad / 2, cy + rad / 5, rad / 2 + 1);
        disc(cx + rad / 2, cy + rad / 5, rad / 2 + 1);
        const int xs[3] = {cx - rad, cx + rad, cx}, ys[3] = {cy + rad / 6, cy + rad / 6, cy - rad};
        r.fillPolygon(xs, ys, 3, ink);
        const int sx[3] = {cx, cx - rad / 3, cx + rad / 3}, sy[3] = {cy + rad / 4, cy + rad, cy + rad};
        r.fillPolygon(sx, sy, 3, ink);
        break;
      }
      case 1: {  // heart
        disc(cx - rad / 2, cy - rad / 4, rad / 2 + 1);
        disc(cx + rad / 2, cy - rad / 4, rad / 2 + 1);
        const int xs[3] = {cx - rad, cx + rad, cx}, ys[3] = {cy - rad / 8, cy - rad / 8, cy + rad};
        r.fillPolygon(xs, ys, 3, ink);
        break;
      }
      case 2: {  // club
        const int lobe = rad * 9 / 20;
        disc(cx, cy - rad / 2, lobe);
        disc(cx - rad / 2, cy + rad / 8, lobe);
        disc(cx + rad / 2, cy + rad / 8, lobe);
        const int sx[3] = {cx, cx - rad / 3, cx + rad / 3}, sy[3] = {cy - rad / 8, cy + rad, cy + rad};
        r.fillPolygon(sx, sy, 3, ink);
        break;
      }
      default: {  // diamond
        const int xs[4] = {cx, cx + rad * 3 / 4, cx, cx - rad * 3 / 4}, ys[4] = {cy - rad, cy, cy + rad, cy};
        r.fillPolygon(xs, ys, 4, ink);
        break;
      }
    }
  };
  shape(radius, black);
  // Red suits are hollow so flushes read at a glance in 1-bit.
  if ((suit == 1 || suit == 3) && radius >= 6) shape(radius - 3, !black);
}

void drawCard(const GfxRenderer& r, const Pack& pack, const uint8_t card, const int x, const int y) {
  if (pack.draw(r, FACE_BASE + card, x, y)) return;
  const int rank = rankOf(card), suit = suitOf(card);
  framedRect(r, x, y, CARD_W, CARD_H, 8);
  r.drawText(UI_12_FONT_ID, x + 8, y + 6, RANKS[rank], true, EpdFontFamily::BOLD);
  drawSuit(r, suit, x + 16, y + 44, 9, true);
  if (rank >= 9 && rank <= 11) {
    r.drawRect(x + 30, y + 30, CARD_W - 60, CARD_H - 60, 2, true);
    const int w = r.getTextWidth(FALLBACK_BIG_FONT, RANKS[rank], EpdFontFamily::BOLD);
    r.drawText(FALLBACK_BIG_FONT, x + (CARD_W - w) / 2, y + CARD_H / 2 - 40, RANKS[rank], true, EpdFontFamily::BOLD);
    drawSuit(r, suit, x + CARD_W / 2, y + CARD_H / 2 + 22, 14, true);
  } else {
    drawSuit(r, suit, x + CARD_W / 2 + 6, y + CARD_H / 2 + 6, rank == 12 ? 34 : 24, true);
  }
}

void drawCardBack(const GfxRenderer& r, const Pack& pack, const int x, const int y) {
  if (pack.draw(r, CARD_BACK, x, y)) return;
  r.fillRoundedRect(x, y, CARD_W, CARD_H, 8, Color::Black);
  r.drawRect(x + 8, y + 8, CARD_W - 16, CARD_H - 16, 2, false);
  drawSuit(r, 0, x + CARD_W / 2, y + CARD_H / 2, 16, false);
}

void drawJoker(const GfxRenderer& r, const Pack& pack, const uint8_t joker, const int x, const int y) {
  if (pack.draw(r, JOKER_BASE + joker, x, y)) return;
  r.fillRoundedRect(x, y, JOKER_W, JOKER_H, 8, Color::Black);
  r.drawRoundedRect(x + 4, y + 4, JOKER_W - 8, JOKER_H - 8, 1, 6, false);
  // A jester cap: a band with three bells.
  const int cx = x + JOKER_W / 2, cy = y + 50;
  const int bx[4] = {cx - 30, cx + 30, cx + 24, cx - 24}, by[4] = {cy - 22, cy - 22, cy + 16, cy + 16};
  r.fillPolygon(bx, by, 4, false);
  const int tx[3] = {cx - 30, cx, cx + 30}, ty[3] = {cy - 22, cy - 30, cy - 22};
  for (int i = 0; i < 3; i++) r.fillRoundedRect(tx[i] - 5, ty[i] - 5, 10, 10, 5, Color::White);
  char number[4];
  snprintf(number, sizeof(number), "%d", joker + 1);
  const int w = r.getTextWidth(UI_12_FONT_ID, number, EpdFontFamily::BOLD);
  r.drawText(UI_12_FONT_ID, cx - w / 2, y + JOKER_H - 36, number, false, EpdFontFamily::BOLD);
}

void drawBlindChip(const GfxRenderer& r, const Pack& pack, const int blind, const uint8_t boss, const int x,
                   const int y) {
  if (pack.draw(r, blind < 2 ? SMALL_CHIP + blind : BOSS_BASE + boss, x, y)) return;
  const bool dark = blind == 2;
  r.fillRoundedRect(x, y, CHIP, CHIP, CHIP / 2, dark ? Color::Black : Color::White);
  r.drawRoundedRect(x, y, CHIP, CHIP, 3, CHIP / 2, true);
  r.drawRoundedRect(x + 12, y + 12, CHIP - 24, CHIP - 24, 2, CHIP / 2 - 12, !dark);
  const char* mark = blind == 0 ? "S" : (blind == 1 ? "B" : "!");
  const int w = r.getTextWidth(FALLBACK_BIG_FONT, mark, EpdFontFamily::BOLD);
  r.drawText(FALLBACK_BIG_FONT, x + (CHIP - w) / 2, y + CHIP / 2 - 18, mark, !dark, EpdFontFamily::BOLD);
}

void drawPlanet(const GfxRenderer& r, const Pack& pack, const uint8_t hand, const int x, const int y) {
  if (pack.draw(r, PLANET_BASE + hand, x, y)) return;
  r.fillRoundedRect(x + 8, y + 8, PLANET - 16, PLANET - 16, PLANET / 2 - 8, Color::Black);
  r.drawLine(x, y + PLANET / 2 + 6, x + PLANET, y + PLANET / 2 - 6, 3, true);
  r.fillRoundedRect(x + 20, y + 18, 10, 10, 5, Color::White);
}

int bigTextHeight(const Pack& pack) {
  const int h = pack.height(GLYPH_BASE);
  return h > 0 ? h : FALLBACK_BIG_HEIGHT;
}

int bigTextWidth(const GfxRenderer& r, const Pack& pack, const char* text) {
  if (pack.width(GLYPH_BASE) == 0) return r.getTextWidth(FALLBACK_BIG_FONT, text, EpdFontFamily::BOLD);
  int w = 0;
  for (const char* p = text; *p; p++) {
    const int id = glyphFor(*p);
    w += *p == '.' ? DOT_ADVANCE : (id < 0 ? bigTextHeight(pack) / 4 : pack.width(id) + GLYPH_GAP);
  }
  return std::max(0, w - GLYPH_GAP);
}

void drawBigText(const GfxRenderer& r, const Pack& pack, const char* text, int x, const int y, const bool white) {
  if (pack.width(GLYPH_BASE) == 0) {
    r.drawText(FALLBACK_BIG_FONT, x, y, text, !white, EpdFontFamily::BOLD);
    return;
  }
  for (const char* p = text; *p; p++) {
    const int id = glyphFor(*p);
    if (*p == '.') {
      r.fillRect(x + 2, y + DOT_TOP, DOT_SIZE, DOT_SIZE, !white);
      x += DOT_ADVANCE;
      continue;
    }
    if (id < 0) {
      x += bigTextHeight(pack) / 4;
      continue;
    }
    pack.draw(r, id, x, y, white);
    x += pack.width(id) + GLYPH_GAP;
  }
}

void drawMenuIcon(const GfxRenderer& r, const int x, const int y) {
  // A playing card fanned behind a joker card.
  framedRect(r, x + 6, y + 14, 36, 50, 5);
  r.drawText(SMALL_FONT_ID, x + 11, y + 17, "A", true, EpdFontFamily::BOLD);
  drawSuit(r, 0, x + 24, y + 44, 8, true);
  r.fillRoundedRect(x + 28, y + 6, 38, 54, 5, Color::Black);
  r.drawRoundedRect(x + 31, y + 9, 32, 48, 1, 4, false);
  // A grinning face under a pointed cap.
  const int cx = x + 47, cy = y + 36;
  r.fillRoundedRect(cx - 10, cy - 8, 20, 20, 10, Color::White);
  const int hx[3] = {cx - 13, cx + 13, cx + 6}, hy[3] = {cy - 6, cy - 6, cy - 22};
  r.fillPolygon(hx, hy, 3, false);
  r.fillRect(cx - 5, cy - 2, 3, 3, true);
  r.fillRect(cx + 3, cy - 2, 3, 3, true);
  r.drawLine(cx - 5, cy + 5, cx + 5, cy + 5, 2, true);
}

}  // namespace jokers::art
