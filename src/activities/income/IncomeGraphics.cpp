#include "IncomeGraphics.h"

#include <GfxRenderer.h>

#include <cstdio>
#include <cstring>

#include "fontIds.h"

namespace income_gfx {
namespace {

constexpr uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

constexpr int GLYPH_W = 5;
constexpr int GLYPH_H = 7;
constexpr int GLYPH_GAP = 1;
constexpr int SHADOW_LEVEL = 7;

struct Glyph {
  char ch;
  uint8_t rows[GLYPH_H];  // low 5 bits, MSB = leftmost column
};

constexpr Glyph GLYPHS[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'$', {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
};

const Glyph* findGlyph(const char ch) {
  for (const Glyph& g : GLYPHS) {
    if (g.ch == ch) return &g;
  }
  return nullptr;
}

// Narrow punctuation keeps the hero number tight.
bool isNarrow(const char ch) { return ch == ',' || ch == '.' || ch == ':'; }
int glyphAdvance(const char ch) { return isNarrow(ch) ? 3 : GLYPH_W; }

void drawGlyphBlocks(const GfxRenderer& renderer, const Glyph& glyph, const int x, const int y, const int size,
                     const bool shadow);

// Copies the UTF-8 character at `text` into `glyph` (null-terminated); returns its byte length.
int nextGlyph(const char* text, char* glyph) {
  const auto lead = static_cast<uint8_t>(text[0]);
  const int length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
  int i = 0;
  for (; i < length && text[i] != '\0'; i++) glyph[i] = text[i];
  glyph[i] = '\0';
  return i;
}

void groupThousands(const long long value, char* buf, const int bufSize) {
  char digits[24];
  snprintf(digits, sizeof(digits), "%lld", value);
  const int len = static_cast<int>(strlen(digits));
  int out = 0;
  for (int i = 0; i < len && out < bufSize - 1; i++) {
    if (i > 0 && (len - i) % 3 == 0 && out < bufSize - 2) buf[out++] = ',';
    buf[out++] = digits[i];
  }
  buf[out] = '\0';
}

}  // namespace

void fillDither(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const int level) {
  if (level <= 0) return;
  if (level >= 16) {
    renderer.fillRect(x, y, w, h, true);
    return;
  }
  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      if (BAYER4[py & 3][px & 3] < level) renderer.drawPixel(px, py, true);
    }
  }
}

void fadeRect(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      if (BAYER4[py & 3][px & 3] < 10) renderer.drawPixel(px, py, false);
    }
  }
}

void drawTrackedLabel(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
                      const int tracking) {
  int cursor = x;
  char glyph[5];
  for (const char* p = text; *p;) {
    p += nextGlyph(p, glyph);
    renderer.drawText(fontId, cursor, y, glyph, true, EpdFontFamily::BOLD);
    cursor += renderer.getTextWidth(fontId, glyph, EpdFontFamily::BOLD) + tracking;
  }
}

namespace {
void drawGlyphBlocks(const GfxRenderer& renderer, const Glyph& glyph, const int x, const int y, const int size,
                     const bool shadow) {
  const int colOffset = isNarrow(glyph.ch) ? 1 : 0;
  for (int row = 0; row < GLYPH_H; row++) {
    for (int col = colOffset; col < GLYPH_W; col++) {
      if (!(glyph.rows[row] & (0x10 >> col))) continue;
      const int px = x + (col - colOffset) * size;
      const int py = y + row * size;
      if (shadow) {
        fillDither(renderer, px, py, size, size, SHADOW_LEVEL);
      } else {
        renderer.fillRect(px, py, size, size, true);
      }
    }
  }
}
}  // namespace

int pixelTextWidth(const char* text, const int pixelSize) {
  int width = 0;
  for (const char* p = text; *p; p++) {
    if (!findGlyph(*p)) continue;
    width += (glyphAdvance(*p) + GLYPH_GAP) * pixelSize;
  }
  return width > 0 ? width - GLYPH_GAP * pixelSize : 0;
}

void drawPixelText(const GfxRenderer& renderer, const int x, const int y, const char* text, const int pixelSize) {
  const int shadowOffset = pixelSize / 2 > 0 ? pixelSize / 2 : 1;
  for (const bool shadow : {true, false}) {
    int cursor = x;
    for (const char* p = text; *p; p++) {
      const Glyph* glyph = findGlyph(*p);
      if (!glyph) continue;
      const int offset = shadow ? shadowOffset : 0;
      drawGlyphBlocks(renderer, *glyph, cursor + offset, y + offset, pixelSize, shadow);
      cursor += (glyphAdvance(*p) + GLYPH_GAP) * pixelSize;
    }
  }
}

int trackedLabelWidth(const GfxRenderer& renderer, const int fontId, const char* text, const int tracking) {
  int width = 0;
  char glyph[5];
  for (const char* p = text; *p;) {
    p += nextGlyph(p, glyph);
    width += renderer.getTextWidth(fontId, glyph, EpdFontFamily::BOLD) + tracking;
  }
  return width > 0 ? width - tracking : 0;
}

int drawStepStatus(const GfxRenderer& renderer, const int x, const int y, const int width, const bool alignRight,
                   const int currentStep, const char* label) {
  constexpr int STEP = 10;
  constexpr int STEP_GAP = 6;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const int stepsWidth = STEP * 3 + STEP_GAP * 2;
  const int total = stepsWidth + 10 + labelWidth;
  const int startX = alignRight ? x + width - total : x;
  const int stepY = y + (lineHeight - STEP) / 2;
  for (int i = 0; i < 3; i++) {
    const int sx = startX + i * (STEP + STEP_GAP);
    if (i < currentStep) {
      renderer.fillRect(sx, stepY, STEP, STEP, true);
    } else if (i == currentStep) {
      fillDither(renderer, sx, stepY, STEP, STEP, 8);
      renderer.drawRect(sx, stepY, STEP, STEP, 1, true);
    } else {
      renderer.drawRect(sx, stepY, STEP, STEP, 1, true);
    }
  }
  renderer.drawText(SMALL_FONT_ID, startX + stepsWidth + 10, y, label);
  return y + lineHeight;
}

int wrapText(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth, char (*lines)[96],
             const int maxLines, const EpdFontFamily::Style style) {
  int count = 0;
  const char* p = text;
  while (*p && count < maxLines) {
    while (*p == ' ') p++;
    char* line = lines[count];
    line[0] = '\0';
    size_t len = 0;
    const char* rest = p;
    // Add whole words while they fit.
    while (*rest) {
      const char* wordEnd = rest;
      while (*wordEnd && *wordEnd != ' ') wordEnd++;
      char candidate[96];
      snprintf(candidate, sizeof(candidate), "%s%s%.*s", line, len ? " " : "", static_cast<int>(wordEnd - rest), rest);
      if (len && renderer.getTextWidth(fontId, candidate, style) > maxWidth) break;
      snprintf(line, 96, "%s", candidate);
      len = strlen(line);
      rest = wordEnd;
      while (*rest == ' ') rest++;
    }
    p = rest;
    count++;
  }
  if (*p && count > 0) {
    char* last = lines[count - 1];
    size_t len = strlen(last);
    const int ellipsis = renderer.getTextWidth(fontId, "...", style);
    while (len > 0 && renderer.getTextWidth(fontId, last, style) + ellipsis > maxWidth) last[--len] = '\0';
    snprintf(last + len, 96 - len, "...");
  }
  return count;
}

// Whole dollars, rounded toward zero, so a figure never overstates income.
void formatUsd(const int64_t cents, char* buf, const int bufSize) {
  const bool negative = cents < 0;
  const long long dollars = (negative ? -cents : cents) / 100;
  char whole[24];
  groupThousands(dollars, whole, sizeof(whole));
  snprintf(buf, bufSize, "%s$%s", negative && dollars > 0 ? "-" : "", whole);
}

void formatUsdShort(const int64_t cents, char* buf, const int bufSize) {
  const bool negative = cents < 0;
  const long long dollars = (negative ? -cents : cents) / 100;
  const char* sign = negative && dollars > 0 ? "-" : "";
  if (dollars < 1000) {
    snprintf(buf, bufSize, "%s$%lld", sign, dollars);
  } else if (dollars < 10000) {
    // One decimal, rounded toward zero; "$3k" rather than "$3.0k".
    const long long tenths = dollars / 100;
    if (tenths % 10 == 0) {
      snprintf(buf, bufSize, "%s$%lldk", sign, tenths / 10);
    } else {
      snprintf(buf, bufSize, "%s$%lld.%lldk", sign, tenths / 10, tenths % 10);
    }
  } else if (dollars < 1000000) {
    snprintf(buf, bufSize, "%s$%lldk", sign, dollars / 1000);
  } else {
    const long long tenths = dollars / 100000;
    snprintf(buf, bufSize, "%s$%lld.%lldM", sign, tenths / 10, tenths % 10);
  }
}

}  // namespace income_gfx
