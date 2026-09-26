#include "MinesweeperArt.h"

#include <GfxRenderer.h>

#include <algorithm>

namespace mines::art {
namespace {
struct Glyph {
  uint8_t rows[DIGIT_HEIGHT];  // bit 4 is the leftmost column
  uint8_t cols;
};

constexpr Glyph GLYPHS[] = {
    {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, 5},  // 0
    {{0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, 5},  // 1
    {{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, 5},  // 2
    {{0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, 5},  // 3
    {{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, 5},  // 4
    {{0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, 5},  // 5
    {{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, 5},  // 6
    {{0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, 5},  // 7
    {{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, 5},  // 8
    {{0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, 5},  // 9
    {{0x00, 0x10, 0x10, 0x00, 0x10, 0x10, 0x00}, 1},  // :
    {{0x00, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00}, 4},  // -
};

const Glyph* glyphFor(const char ch) {
  if (ch >= '0' && ch <= '9') return &GLYPHS[ch - '0'];
  if (ch == ':') return &GLYPHS[10];
  if (ch == '-') return &GLYPHS[11];
  return nullptr;
}

int boldExtra(const int scale, const bool bold) { return bold ? std::max(1, scale / 2) : 0; }

// Stroke of `width` px in any direction (GfxRenderer's thick line only grows downwards).
void stroke(const GfxRenderer& renderer, const int x1, const int y1, const int x2, const int y2, const int width,
            const bool black) {
  for (int i = 0; i < width; i++) {
    const int o = i - width / 2;
    renderer.drawLine(x1 + o, y1, x2 + o, y2, black);
    renderer.drawLine(x1, y1 + o, x2, y2 + o, black);
  }
}

int digitScaleFor(const int size) { return std::clamp(size / 9, 2, 5); }
}  // namespace

int digitsWidth(const char* text, const int scale, const bool bold) {
  int width = 0;
  for (const char* p = text; *p; p++) {
    const Glyph* glyph = glyphFor(*p);
    if (!glyph) continue;
    if (width > 0) width += scale;
    width += glyph->cols * scale + boldExtra(scale, bold);
  }
  return width;
}

void digits(const GfxRenderer& renderer, const char* text, int x, const int y, const int scale, const bool black,
            const bool bold) {
  const int extra = boldExtra(scale, bold);
  for (const char* p = text; *p; p++) {
    const Glyph* glyph = glyphFor(*p);
    if (!glyph) continue;
    for (int row = 0; row < DIGIT_HEIGHT; row++) {
      for (int col = 0; col < glyph->cols; col++) {
        if (glyph->rows[row] & (0x10 >> col)) {
          renderer.fillRect(x + col * scale, y + row * scale, scale + extra, scale, black);
        }
      }
    }
    x += glyph->cols * scale + extra + scale;
  }
}

void hiddenTile(const GfxRenderer& renderer, const int x, const int y, const int size, const bool flagged,
                const bool focused) {
  renderer.fillRect(x, y, size, size, false);
  renderer.fillRect(x, y, size, 1, true);
  renderer.fillRect(x, y, 1, size, true);
  if (focused) {
    // Solid black with a checker face: still reads as "unopened", unlike a
    // focused blank, which is plain black.
    renderer.fillRect(x + 1, y + 1, size - 1, size - 1, true);
    if (!flagged) renderer.fillRectDither(x + 5, y + 5, size - 9, size - 9, Color::DarkGray);
  } else {
    // Dotted face with a white top/left highlight and a heavy bottom/right shadow.
    if (!flagged) renderer.fillRectDither(x + 3, y + 3, size - 6, size - 6, Color::LightGray);
    renderer.fillRect(x + 1, y + size - 2, size - 1, 2, true);
    renderer.fillRect(x + size - 2, y + 1, 2, size - 1, true);
  }
  if (flagged) flag(renderer, x + (size - 1) / 2, y + (size - 1) / 2, size, !focused);
}

void openTile(const GfxRenderer& renderer, const int x, const int y, const int size) {
  renderer.fillRect(x, y, size, size, false);
  for (int k = 0; k < size; k += 2) {
    renderer.drawPixel(x + k, y, true);
    renderer.drawPixel(x, y + k, true);
  }
}

void number(const GfxRenderer& renderer, const int x, const int y, const int size, const int value, const bool ink) {
  if (value < 1 || value > 8) return;
  const char text[2] = {static_cast<char>('0' + value), '\0'};
  const int scale = digitScaleFor(size);
  const bool bold = value >= 2;
  const int w = digitsWidth(text, scale, bold);
  const int h = DIGIT_HEIGHT * scale;
  const int gx = x + (size + 1) / 2 - w / 2;
  const int gy = y + (size + 1) / 2 - h / 2;
  const int pad = std::max(2, scale - 1);
  if (value >= 4) {
    renderer.fillRoundedRect(gx - pad, gy - pad, w + 2 * pad, h + 2 * pad, pad + 1, ink ? Color::Black : Color::White);
    digits(renderer, text, gx, gy, scale, !ink, true);
    return;
  }
  if (value == 3) renderer.drawRoundedRect(gx - pad, gy - pad, w + 2 * pad, h + 2 * pad, 2, pad + 1, ink);
  digits(renderer, text, gx, gy, scale, ink, bold);
}

void mine(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool black) {
  const int width = std::max(2, radius / 3);
  const int len = radius + radius / 2;
  const int diag = len * 7 / 10;
  renderer.fillRect(cx - len, cy - width / 2, 2 * len + 1, width, black);
  renderer.fillRect(cx - width / 2, cy - len, width, 2 * len + 1, black);
  stroke(renderer, cx - diag, cy - diag, cx + diag, cy + diag, width, black);
  stroke(renderer, cx - diag, cy + diag, cx + diag, cy - diag, width, black);
  renderer.fillRoundedRect(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1, radius,
                           black ? Color::Black : Color::White);
  const int glint = std::max(2, radius / 3);
  renderer.fillRect(cx - radius / 2, cy - radius / 2, glint, glint, !black);
}

void flag(const GfxRenderer& renderer, const int cx, const int cy, const int size, const bool ink) {
  // Pennant flies right so it can't be mistaken for the "1" glyph.
  const int h = size * 3 / 5;
  const int top = cy - h / 2;
  const int pole = std::max(2, size / 16);
  const int poleX = cx - h / 4;
  const int xs[3] = {poleX, poleX + h * 2 / 3, poleX};
  const int ys[3] = {top, top + h / 4, top + h / 2};
  renderer.fillPolygon(xs, ys, 3, ink);
  renderer.fillRect(poleX, top, pole, h, ink);
  const int bar = std::max(2, size / 16);
  const int base = h * 3 / 4;
  const int mid = poleX + pole / 2;
  renderer.fillRect(mid - base / 2, top + h - bar, base, bar, ink);
  renderer.fillRect(mid - base / 4, top + h - 2 * bar, base / 2, bar, ink);
}

void cross(const GfxRenderer& renderer, const int x, const int y, const int size, const bool ink) {
  const int m = size / 5;
  const int width = std::max(2, size / 12);
  for (const bool front : {false, true}) {
    const int w = front ? width : width + 2;
    const bool color = front ? ink : !ink;
    stroke(renderer, x + m, y + m, x + size - m, y + size - m, w, color);
    stroke(renderer, x + size - m, y + m, x + m, y + size - m, w, color);
  }
}

void cursor(const GfxRenderer& renderer, const int x, const int y, const int size) {
  // The focused tile is drawn inverted; this widens it into a black block with
  // a white halo and a thin white inset marking the tile's own edge.
  renderer.drawRect(x - 4, y - 4, size + 8, size + 8, 1, false);
  renderer.drawRect(x - 3, y - 3, size + 6, size + 6, 4, true);
  renderer.drawRect(x + 2, y + 2, size - 3, size - 3, 1, false);
}

void clock(const GfxRenderer& renderer, const int cx, const int cy, const int radius) {
  renderer.drawRoundedRect(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1, 2, radius, true);
  renderer.fillRect(cx - 1, cy - radius + 4, 2, radius - 3, true);
  renderer.fillRect(cx, cy - 1, radius - 4, 2, true);
}

}  // namespace mines::art
