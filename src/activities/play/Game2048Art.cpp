#include "Game2048Art.h"

#include <algorithm>
#include <cstdio>

#include "Game2048Board.h"

namespace g2048 {
namespace {

// ---- textures ------------------------------------------------------------------

enum TilePattern : uint8_t { DOTS = PAT_COUNT, PINSTRIPE, LATTICE, CHECKER, BANDS, DARK, DARK_PINSTRIPE, STARS };

constexpr uint8_t PATTERNS[][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // PAT_WHITE
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},  // PAT_BLACK
    {0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00},  // PAT_BOARD: 12% stipple
    {0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x22, 0x00},  // DOTS: 6% grid
    {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},  // PINSTRIPE: fine "/" every 8 px
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},  // LATTICE: diamond mesh
    {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55},  // CHECKER: 50% grey
    {0xF0, 0xE1, 0xC3, 0x87, 0x0F, 0x1E, 0x3C, 0x78},  // BANDS: bold 45 degree hatching
    {0x55, 0xFF, 0x55, 0xFF, 0x55, 0xFF, 0x55, 0xFF},  // DARK: 75% grey
    {0xEE, 0xDD, 0xBB, 0x77, 0xEE, 0xDD, 0xBB, 0x77},  // DARK_PINSTRIPE: white "/" on black
    {0xFF, 0xFF, 0xDD, 0xFF, 0xFF, 0xFF, 0xDD, 0xFF},  // STARS: white specks on black
};

enum TileFlag : uint8_t { INK_WHITE = 1, FRAME = 2, CROWN = 4, TEXTURED = 8 };

struct TileStyle {
  uint8_t pattern;
  uint8_t flags;
};

// Index = exponent; the last entry covers everything above 2048.
constexpr TileStyle STYLES[] = {
    {PAT_WHITE, 0},                                 // empty (unused)
    {PAT_WHITE, 0},                                 // 2
    {PAT_WHITE, FRAME},                             // 4
    {DOTS, TEXTURED},                               // 8
    {PINSTRIPE, TEXTURED},                          // 16
    {LATTICE, TEXTURED},                            // 32
    {CHECKER, TEXTURED},                            // 64
    {BANDS, TEXTURED},                              // 128
    {DARK, INK_WHITE | TEXTURED},                   // 256
    {DARK_PINSTRIPE, INK_WHITE | TEXTURED},         // 512
    {PAT_BLACK, INK_WHITE | FRAME},                 // 1024
    {PAT_BLACK, INK_WHITE | FRAME | CROWN},         // 2048
    {STARS, INK_WHITE | FRAME | CROWN | TEXTURED},  // 4096+
};
constexpr int STYLE_COUNT = sizeof(STYLES) / sizeof(STYLES[0]);

int isqrt(int value) {
  if (value <= 0) return 0;
  int root = 0;
  int bit = 1 << 30;
  while (bit > value) bit >>= 2;
  while (bit != 0) {
    if (value >= root + bit) {
      value -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return root;
}

// ---- sub-pixel shapes (coordinates in 1/16 px) --------------------------------

constexpr int SUB = 16;
constexpr int MAX_POLY = 52;

// Even-odd scanline fill sampled at pixel centres; spans go out as 1-row rects.
void fillPoly(const GfxRenderer& renderer, const int16_t* xs, const int16_t* ys, const int n, const bool black) {
  if (n < 3) return;
  int minY = ys[0], maxY = ys[0];
  for (int i = 1; i < n; i++) {
    minY = std::min<int>(minY, ys[i]);
    maxY = std::max<int>(maxY, ys[i]);
  }
  int16_t nodes[MAX_POLY];
  for (int py = (minY - SUB / 2 + SUB - 1) >> 4; py * SUB + SUB / 2 < maxY; py++) {
    const int sy = py * SUB + SUB / 2;
    int count = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      const int yi = ys[i], yj = ys[j];
      if ((yi <= sy && yj > sy) || (yj <= sy && yi > sy)) {
        const int x = xs[i] + (sy - yi) * (xs[j] - xs[i]) / (yj - yi);
        int k = count++;
        while (k > 0 && nodes[k - 1] > x) {
          nodes[k] = nodes[k - 1];
          k--;
        }
        nodes[k] = static_cast<int16_t>(x);
      }
    }
    for (int k = 0; k + 1 < count; k += 2) {
      const int x0 = (nodes[k] - SUB / 2 + SUB - 1) >> 4;
      const int x1 = ((nodes[k + 1] - SUB / 2 + SUB - 1) >> 4) - 1;
      if (x1 >= x0) renderer.fillRect(x0, py, x1 - x0 + 1, 1, black);
    }
  }
}

void fillDisc(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool black) {
  for (int py = (cy - radius - SUB / 2 + SUB - 1) >> 4; py * SUB + SUB / 2 < cy + radius; py++) {
    const int dy = py * SUB + SUB / 2 - cy;
    const int hw = isqrt(radius * radius - dy * dy);
    const int x0 = (cx - hw - SUB / 2 + SUB - 1) >> 4;
    const int x1 = ((cx + hw - SUB / 2 + SUB - 1) >> 4) - 1;
    if (x1 >= x0) renderer.fillRect(x0, py, x1 - x0 + 1, 1, black);
  }
}

// ---- numeral face ----------------------------------------------------------------
// Strokes on a 60 x 100 unit grid (y down), drawn with round caps. Arcs run
// clockwise on screen from a0 to a1 in 15 degree steps, 0 = pointing right.

struct Stroke {
  uint8_t arc;
  int8_t a, b, c, d, e, f;  // line: x0 y0 x1 y1 | arc: cx cy rx ry a0 a1
};

constexpr Stroke L(int x0, int y0, int x1, int y1) {
  return {0, static_cast<int8_t>(x0), static_cast<int8_t>(y0), static_cast<int8_t>(x1), static_cast<int8_t>(y1), 0, 0};
}
constexpr Stroke A(int cx, int cy, int rx, int ry, int a0, int a1) {
  return {1,
          static_cast<int8_t>(cx),
          static_cast<int8_t>(cy),
          static_cast<int8_t>(rx),
          static_cast<int8_t>(ry),
          static_cast<int8_t>(a0),
          static_cast<int8_t>(a1)};
}

constexpr Stroke STROKES[] = {
    // 0
    A(30, 30, 30, 30, 12, 24),
    L(60, 30, 60, 70),
    A(30, 70, 30, 30, 0, 12),
    L(0, 70, 0, 30),
    // 1
    L(30, 0, 30, 100),
    L(2, 22, 30, 0),
    // 2
    A(30, 30, 30, 30, 13, 27),
    L(51, 51, 0, 100),
    L(0, 100, 60, 100),
    // 3
    A(30, 26, 28, 24, 13, 30),
    A(30, 74, 30, 26, 18, 35),
    L(16, 49, 30, 49),
    // 4
    L(44, 100, 44, 0),
    L(44, 0, 0, 68),
    L(0, 68, 60, 68),
    // 5
    L(58, 0, 7, 0),
    L(7, 0, 4, 48),
    A(30, 67, 30, 33, 14, 34),
    // 6
    A(30, 68, 30, 32, 0, 24),
    A(60, 68, 60, 68, 12, 17),
    // 7
    L(0, 0, 60, 0),
    L(60, 0, 20, 100),
    // 8
    A(30, 24, 25, 24, 0, 24),
    A(30, 73, 30, 27, 0, 24),
    // 9
    A(30, 32, 30, 32, 0, 24),
    A(0, 32, 60, 68, 0, 5),
};
constexpr uint8_t GLYPH_START[11] = {0, 4, 6, 9, 12, 15, 18, 20, 22, 24, 26};
constexpr int GLYPH_WIDTH[10] = {60, 30, 60, 60, 60, 60, 60, 60, 60, 60};
constexpr int STROKE_UNITS = 19;  // stroke weight in grid units
constexpr int GLYPH_HEIGHT = 100 + STROKE_UNITS;
constexpr int TRACKING = 8;

// sin(k * 15 deg) * 1024 for k = 0..6.
constexpr int16_t SIN15[7] = {0, 265, 512, 724, 887, 989, 1024};

int sin15(int k) {
  k = ((k % 24) + 24) % 24;
  if (k <= 6) return SIN15[k];
  if (k <= 12) return SIN15[12 - k];
  if (k <= 18) return -SIN15[k - 12];
  return -SIN15[24 - k];
}
int cos15(const int k) { return sin15(k + 6); }

struct Pen {
  const GfxRenderer& renderer;
  int left;  // glyph origin in 1/16 px (grid 0,0 sits half a stroke inside)
  int top;
  int num;  // grid units -> 1/16 px = num / den
  int den;
  int half;  // half the stroke width in 1/16 px, halo included
  bool black;

  int x(const int u) const { return left + (u * 2 + STROKE_UNITS) * num / (2 * den); }
  int y(const int u) const { return top + (u * 2 + STROKE_UNITS) * num / (2 * den); }
  int len(const int u) const { return u * num / den; }

  // Both stroke kinds become one polygon (a quad, or a ring sector) plus two
  // round caps at the centre-line ends; a closed ring needs no caps.
  void stroke(const Stroke& s) const {
    int16_t xs[MAX_POLY], ys[MAX_POLY];
    int n, x0, y0, x1, y1;
    if (s.arc) {
      const int cx = x(s.a), cy = y(s.b), rx = len(s.c), ry = len(s.d);
      const int count = s.f - s.e + 1;
      const int inX = std::max(rx - half, 0), inY = std::max(ry - half, 0);
      for (int i = 0; i < count; i++) {
        const int c = cos15(s.e + i), sn = sin15(s.e + i);
        xs[i] = static_cast<int16_t>(cx + (rx + half) * c / 1024);
        ys[i] = static_cast<int16_t>(cy + (ry + half) * sn / 1024);
        xs[2 * count - 1 - i] = static_cast<int16_t>(cx + inX * c / 1024);
        ys[2 * count - 1 - i] = static_cast<int16_t>(cy + inY * sn / 1024);
      }
      n = 2 * count;
      x0 = cx + rx * cos15(s.e) / 1024, y0 = cy + ry * sin15(s.e) / 1024;
      x1 = cx + rx * cos15(s.f) / 1024, y1 = cy + ry * sin15(s.f) / 1024;
    } else {
      x0 = x(s.a), y0 = y(s.b), x1 = x(s.c), y1 = y(s.d);
      const int dx = x1 - x0, dy = y1 - y0;
      const int length = std::max(isqrt(dx * dx + dy * dy), 1);
      const int nx = -dy * half / length, ny = dx * half / length;
      xs[0] = static_cast<int16_t>(x0 + nx), ys[0] = static_cast<int16_t>(y0 + ny);
      xs[1] = static_cast<int16_t>(x1 + nx), ys[1] = static_cast<int16_t>(y1 + ny);
      xs[2] = static_cast<int16_t>(x1 - nx), ys[2] = static_cast<int16_t>(y1 - ny);
      xs[3] = static_cast<int16_t>(x0 - nx), ys[3] = static_cast<int16_t>(y0 - ny);
      n = 4;
    }
    fillPoly(renderer, xs, ys, n, black);
    if (s.arc && s.f - s.e >= 24) return;
    fillDisc(renderer, x0, y0, half, black);
    fillDisc(renderer, x1, y1, half, black);
  }

  void glyph(const int digit) const {
    for (int i = GLYPH_START[digit]; i < GLYPH_START[digit + 1]; i++) stroke(STROKES[i]);
  }
};

}  // namespace

void fillRounded(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, int radius,
                 const uint8_t pattern, const int anchorX, const int anchorY) {
  if (w <= 0 || h <= 0) return;
  radius = std::clamp(radius, 0, std::min(w, h) / 2);
  const uint8_t* rows = PATTERNS[pattern];
  for (int row = 0; row < h; row++) {
    int inset = 0;
    const int edge = std::min(row, h - 1 - row);
    if (edge < radius) {
      const int dy2 = 2 * (radius - edge) - 1;  // distance to the corner centre, in half pixels
      inset = radius - (isqrt(4 * radius * radius - dy2 * dy2) + 1) / 2;
    }
    const int py = y + row;
    const int x0 = x + inset, width = w - 2 * inset;
    if (pattern == PAT_WHITE || pattern == PAT_BLACK) {
      renderer.fillRect(x0, py, width, 1, pattern == PAT_BLACK);
      continue;
    }
    const uint8_t bits = rows[(py - anchorY) & 7];
    for (int px = x0; px < x0 + width; px++) renderer.drawPixel(px, py, (bits >> (7 - ((px - anchorX) & 7))) & 1);
  }
}

void drawNumber(const GfxRenderer& renderer, const int cx, const int cy, const uint32_t value, const int maxW,
                const int maxH, const bool black, const int halo) {
  char digits[12];
  const int count = snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
  if (count <= 0) return;
  int units = TRACKING * (count - 1);
  for (int i = 0; i < count; i++) units += GLYPH_WIDTH[digits[i] - '0'] + STROKE_UNITS;
  const int height = std::min(maxH, maxW * GLYPH_HEIGHT / units);
  if (height <= 0) return;
  const int num = height * SUB, den = GLYPH_HEIGHT;
  const int left = cx * SUB - units * num / den / 2;
  const int top = cy * SUB - height * SUB / 2;
  const int stroke = STROKE_UNITS * num / den;
  for (int pass = halo > 0 ? 0 : 1; pass < 2; pass++) {
    const int half = stroke / 2 + (pass == 0 ? halo * SUB : 0);
    int offset = 0;
    for (int i = 0; i < count; i++) {
      const int digit = digits[i] - '0';
      const Pen pen{renderer, left + offset * num / den, top, num, den, half, pass == 0 ? !black : black};
      pen.glyph(digit);
      offset += GLYPH_WIDTH[digit] + STROKE_UNITS + TRACKING;
    }
  }
}

void drawCrown(const GfxRenderer& renderer, const int cx, const int baseY, const int w, const bool black) {
  // Outline on a 20 x 11 grid; jewels sit on the three points.
  static constexpr int8_t CROWN_X[7] = {0, 0, 5, 10, 15, 20, 20};
  static constexpr int8_t CROWN_Y[7] = {11, 2, 7, 0, 7, 2, 11};
  const int left = (cx - w / 2) * SUB, top = (baseY - w * 11 / 20) * SUB;
  int16_t xs[7], ys[7];
  for (int i = 0; i < 7; i++) {
    xs[i] = static_cast<int16_t>(left + CROWN_X[i] * w * SUB / 20);
    ys[i] = static_cast<int16_t>(top + CROWN_Y[i] * w * SUB / 20);
  }
  fillPoly(renderer, xs, ys, 7, black);
  for (int i = 1; i < 7; i += 2) fillDisc(renderer, xs[i], ys[i], std::max(w * SUB / 14, SUB), black);
}

void drawTile(const GfxRenderer& renderer, const int x, const int y, const int size, const uint8_t exp,
              const bool numerals) {
  const TileStyle& style = STYLES[std::min<int>(exp, STYLE_COUNT - 1)];
  const bool inkWhite = style.flags & INK_WHITE;
  const int radius = std::max(3, size / 9);
  const int border = size >= 64 ? 2 : 1;
  fillRounded(renderer, x, y, size, size, radius, PAT_BLACK, x, y);
  fillRounded(renderer, x + border, y + border, size - 2 * border, size - 2 * border, radius - border, style.pattern, x,
              y);
  if ((style.flags & FRAME) && size >= 24) {
    const int inset = std::max(3, size / 16);
    renderer.drawRoundedRect(x + inset, y + inset, size - 2 * inset, size - 2 * inset, 1, std::max(2, radius - inset),
                             !inkWhite);
  }
  const bool crown = (style.flags & CROWN) && size >= 20;
  if (crown) drawCrown(renderer, x + size / 2, y + (numerals ? size * 31 / 100 : size * 3 / 4), size * 2 / 5, false);
  if (!numerals) return;
  const int pad = size / 8;
  const int cy = y + size / 2 + (crown ? size / 9 : 0);
  const int maxH = crown ? size * 34 / 100 : size * 44 / 100;
  const int halo = (style.flags & TEXTURED) ? std::max(2, size / 28) : 0;
  drawNumber(renderer, x + size / 2, cy, tileValue(exp), size - 2 * pad, maxH, !inkWhite, halo);
}

void drawNewMark(const GfxRenderer& renderer, const int x, const int y, const int size) {
  // Corner brackets in the gap around the tile, like a viewfinder.
  constexpr int OFF = 4, THICK = 2;
  const int arm = std::max(8, size / 6);
  const int span = size + 2 * OFF;
  for (int corner = 0; corner < 4; corner++) {
    const int left = x - OFF + (corner & 1) * (span - arm);
    const int top = y - OFF + (corner >> 1) * (span - arm);
    renderer.fillRect(left, y - OFF + (corner >> 1) * (span - THICK), arm, THICK, true);
    renderer.fillRect(x - OFF + (corner & 1) * (span - THICK), top, THICK, arm, true);
  }
}

void drawMergeMark(const GfxRenderer& renderer, const int x, const int y, const int size) {
  constexpr int OFF = 4;
  renderer.drawRoundedRect(x - OFF, y - OFF, size + 2 * OFF, size + 2 * OFF, 2, std::max(3, size / 9) + OFF, true);
}

}  // namespace g2048
