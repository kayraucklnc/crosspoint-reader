#include "GeneratedCover.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fontIds.h"

namespace generated_cover {
namespace {
constexpr int STRIP_ROWS = 32;
constexpr int MOTIF_COUNT = 6;

// xorshift32: every design decision is a draw from the title's own stream.
struct Rng {
  uint32_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  int range(const int lo, const int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

void fillCircle(const GfxRenderer& r, const int cx, const int cy, const int radius, const bool black) {
  for (int dy = -radius; dy <= radius; dy++) {
    const int half = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)));
    r.fillRect(cx - half, cy + dy, half * 2 + 1, 1, black);
  }
}

void ringCircle(const GfxRenderer& r, const int cx, const int cy, const int radius, const int thickness,
                const bool black) {
  const int inner = std::max(0, radius - thickness);
  for (int dy = -radius; dy <= radius; dy++) {
    const int outerHalf = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)));
    const int innerHalf =
        std::abs(dy) < inner ? static_cast<int>(std::sqrt(static_cast<float>(inner * inner - dy * dy))) : -1;
    if (innerHalf < 0) {
      r.fillRect(cx - outerHalf, cy + dy, outerHalf * 2 + 1, 1, black);
    } else {
      r.fillRect(cx - outerHalf, cy + dy, outerHalf - innerHalf, 1, black);
      r.fillRect(cx + innerHalf + 1, cy + dy, outerHalf - innerHalf, 1, black);
    }
  }
}

void star(const GfxRenderer& r, const int x, const int y, const int size) {
  r.fillRect(x - size, y, size * 2 + 1, 1, false);
  r.fillRect(x, y - size, 1, size * 2 + 1, false);
}

// Motifs are drawn white on the black board inside (x, y, w, h).
void drawSunburst(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  const int cx = x + w / 2;
  const int cy = y + h;
  const int rays = rng.range(9, 17) | 1;
  const int radius = std::max(w, h) * 2;
  const int thick = std::max(1, w / 60);
  for (int i = 0; i < rays; i++) {
    const float a = 3.14159f * (i + 0.5f) / rays;
    if (i % 2 == 0) {
      r.drawLine(cx, cy, cx + static_cast<int>(std::cos(a) * radius), cy - static_cast<int>(std::sin(a) * radius),
                 thick, false);
    }
  }
  const int sun = std::max(4, std::min(w, h) / 4);
  fillCircle(r, cx, cy, sun + 3, true);
  fillCircle(r, cx, cy, sun, false);
}

void drawMoon(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  const int radius = std::max(4, std::min(w, h) * rng.range(30, 40) / 100);
  const int cx = x + w / 2 + rng.range(-w / 8, w / 8);
  const int cy = y + h / 2;
  fillCircle(r, cx, cy, radius, false);
  const int shift = radius * rng.range(35, 60) / 100;
  fillCircle(r, cx + (rng.next() & 1 ? shift : -shift), cy - radius / 5, radius, true);
  const int stars = rng.range(3, 6);
  for (int i = 0; i < stars; i++) star(r, x + rng.range(2, w - 3), y + rng.range(2, h - 3), std::max(1, w / 50));
}

void drawStripes(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  const int gap = std::max(4, w / rng.range(10, 16));
  const bool rising = rng.next() & 1;
  const auto clip = r.getClipRect();
  r.setClipRect(std::max(x, clip[0]), std::max(y, clip[1]), std::min(w, clip[2]), std::min(h, clip[3]));
  for (int d = -h; d < w; d += gap) {
    if (rising) {
      r.drawLine(x + d, y + h, x + d + h, y, 2, false);
    } else {
      r.drawLine(x + d, y, x + d + h, y + h, 2, false);
    }
  }
  r.setClipRect(clip[0], clip[1], clip[2], clip[3]);
  const int radius = std::min(w, h) / 3;
  fillCircle(r, x + w / 2, y + h / 2, radius + 4, true);
  ringCircle(r, x + w / 2, y + h / 2, radius, std::max(2, radius / 6), false);
}

void drawChevrons(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  const int count = rng.range(3, 5);
  const int step = h / (count + 1);
  const int thick = std::max(2, w / 40);
  const bool up = rng.next() & 1;
  for (int i = 0; i < count; i++) {
    const int cy = y + step * (i + 1);
    const int half = w * (40 - i * 4) / 100;
    const int dy = up ? -step / 2 : step / 2;
    r.drawLine(x + w / 2 - half, cy, x + w / 2, cy + dy, thick, false);
    r.drawLine(x + w / 2, cy + dy, x + w / 2 + half, cy, thick, false);
  }
}

void drawHalftone(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  const int cell = std::max(6, w / rng.range(7, 10));
  const bool fromLeft = rng.next() & 1;
  for (int gy = y + cell / 2; gy < y + h; gy += cell) {
    for (int gx = x + cell / 2; gx < x + w; gx += cell) {
      const int t = fromLeft ? (gx - x) + (gy - y) : (x + w - gx) + (gy - y);
      const int radius = (cell / 2) * t / (w + h);
      if (radius > 0) fillCircle(r, gx, gy, radius, false);
    }
  }
}

void drawSkyline(const GfxRenderer& r, Rng& rng, const int x, const int y, const int w, const int h) {
  fillCircle(r, x + w * rng.range(20, 80) / 100, y + h / 4, std::max(3, w / 12), false);
  int bx = x;
  while (bx < x + w) {
    const int bw = std::max(6, w * rng.range(12, 22) / 100);
    const int bh = h * rng.range(35, 85) / 100;
    const int top = y + h - bh;
    r.fillRect(bx, top, std::min(bw, x + w - bx), bh, true);
    r.drawRect(bx, top, std::min(bw, x + w - bx), bh, 1, false);
    const int win = std::max(2, bw / 6);
    for (int wy = top + win * 2; wy < y + h - win; wy += win * 3) {
      for (int wx = bx + win; wx + win < bx + bw && wx + win < x + w; wx += win * 2) {
        if (rng.next() % 3) r.fillRect(wx, wy, win, win, false);
      }
    }
    bx += bw - 1;
  }
}

void drawMotifShape(const GfxRenderer& r, Rng& rng, int motif, int x, int y, int w, int h);

void drawMotif(const GfxRenderer& r, Rng& rng, const int motif, const int x, const int y, const int w, const int h) {
  if (w < 8 || h < 8) return;
  // Motifs may overshoot (rays, discs); the title below must stay untouched.
  const auto clip = r.getClipRect();
  const int cx0 = std::max(x, clip[0]);
  const int cy0 = std::max(y, clip[1]);
  r.setClipRect(cx0, cy0, std::min(x + w, clip[0] + clip[2]) - cx0, std::min(y + h, clip[1] + clip[3]) - cy0);
  drawMotifShape(r, rng, motif, x, y, w, h);
  r.setClipRect(clip[0], clip[1], clip[2], clip[3]);
}

void drawMotifShape(const GfxRenderer& r, Rng& rng, const int motif, const int x, const int y, const int w,
                    const int h) {
  switch (motif) {
    case 0:
      drawSunburst(r, rng, x, y, w, h);
      break;
    case 1:
      drawMoon(r, rng, x, y, w, h);
      break;
    case 2:
      drawStripes(r, rng, x, y, w, h);
      break;
    case 3:
      drawChevrons(r, rng, x, y, w, h);
      break;
    case 4:
      drawHalftone(r, rng, x, y, w, h);
      break;
    default:
      drawSkyline(r, rng, x, y, w, h);
      break;
  }
}

// Largest title font (capped by cover height) that sets every word whole within
// `maxLines`; narrow covers step down rather than split "Yellow" across lines.
int titleFontFor(const GfxRenderer& r, const char* title, const int height, const int width, const int maxLines,
                 const int maxTextHeight) {
  constexpr int FONTS[] = {NOTOSANS_18_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_14_FONT_ID,
                           NOTOSANS_12_FONT_ID, UI_10_FONT_ID,        SMALL_FONT_ID};
  constexpr int FONT_COUNT = sizeof(FONTS) / sizeof(FONTS[0]);
  const int first = height >= 280 ? 0 : (height >= 200 ? 1 : 2);
  for (int f = first; f < FONT_COUNT; f++) {
    bool fits = true;
    const char* p = title;
    while (*p && fits) {
      const char* end = p;
      while (*end && *end != ' ') end++;
      char word[48];
      snprintf(word, sizeof(word), "%.*s", static_cast<int>(end - p), p);
      fits = r.getTextWidth(FONTS[f], word, EpdFontFamily::BOLD) <= width;
      p = *end ? end + 1 : end;
    }
    if (!fits) continue;
    const int lines = static_cast<int>(r.wrappedText(FONTS[f], title, width, maxLines + 1, EpdFontFamily::BOLD).size());
    if (lines <= maxLines && lines * r.getLineHeight(FONTS[f]) <= maxTextHeight) return FONTS[f];
  }
  return SMALL_FONT_ID;
}

// First character of the title, for monogram-sized covers.
void initialOf(const char* title, char* out, const size_t outSize) {
  size_t n = 0;
  if (title && title[0]) {
    n = 1;
    while (n < outSize - 1 && (static_cast<uint8_t>(title[n]) & 0xC0) == 0x80) n++;
  }
  memcpy(out, title ? title : "", n);
  out[n] = '\0';
}

void writeLE16(uint8_t* p, const uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
void writeLE32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}
}  // namespace

uint32_t seedFor(const char* title) {
  uint32_t h = 2166136261u;
  for (const char* p = title ? title : ""; *p; p++) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  return h ? h : 1;
}

void draw(const GfxRenderer& r, const int x, const int y, const int w, const int h, const char* title,
          const char* author) {
  Rng rng{seedFor(title)};
  const int motif = static_cast<int>(rng.next() % MOTIF_COUNT);
  const bool titleOnTop = (rng.next() % 3) == 0;
  r.fillRect(x, y, w, h, true);

  const int m = std::max(3, w / 22);
  r.drawRect(x + m, y + m, w - m * 2, h - m * 2, 1, false);
  if (w >= 60) r.drawRect(x + m + 3, y + m + 3, w - m * 2 - 6, h - m * 2 - 6, 1, false);
  const int innerX = x + m + 5;
  const int innerW = w - (m + 5) * 2;

  if (h < 140) {
    // Monogram: the motif as a backdrop, the initial set large on a black plate.
    drawMotif(r, rng, motif, innerX, y + m + 6, innerW, h - (m + 6) * 2);
    char initial[5];
    initialOf(title, initial, sizeof(initial));
    const int font = NOTOSANS_16_FONT_ID;
    const int tw = r.getTextWidth(font, initial, EpdFontFamily::BOLD);
    const int th = r.getLineHeight(font);
    const int plate = std::max(tw, th) + 6;
    r.fillRect(x + (w - plate) / 2, y + (h - plate) / 2, plate, plate, true);
    r.drawRect(x + (w - plate) / 2, y + (h - plate) / 2, plate, plate, 1, false);
    r.drawText(font, x + (w - tw) / 2, y + (h - th) / 2 + 2, initial, false, EpdFontFamily::BOLD);
    return;
  }

  const char* text = title && title[0] ? title : "?";
  const int maxLines = h >= 250 ? 4 : 3;
  // The title may take at most ~40% of the board so the motif keeps its room.
  const int titleFont = titleFontFor(r, text, h, innerW - 2, maxLines, h * 40 / 100);
  const int authorFont = h >= 280 ? UI_12_FONT_ID : (h >= 160 ? UI_10_FONT_ID : SMALL_FONT_ID);
  const auto lines = r.wrappedText(titleFont, text, innerW - 2, maxLines, EpdFontFamily::BOLD);
  const int lineH = r.getLineHeight(titleFont);
  const int titleH = static_cast<int>(lines.size()) * lineH;
  const int authorH = author && author[0] ? r.getLineHeight(authorFont) : 0;
  const int textBlock = titleH + 14 + authorH;

  const int motifTop = titleOnTop ? y + m + 12 + textBlock : y + m + 12;
  const int motifH = h - (m + 12) * 2 - textBlock - 8;
  drawMotif(r, rng, motif, innerX, motifTop, innerW, motifH);

  int ty = titleOnTop ? y + m + 12 : y + h - m - 12 - textBlock;
  for (const auto& line : lines) {
    const int lw = r.getTextWidth(titleFont, line.c_str(), EpdFontFamily::BOLD);
    r.drawText(titleFont, x + (w - lw) / 2, ty, line.c_str(), false, EpdFontFamily::BOLD);
    ty += lineH;
  }
  const int ruleW = std::max(12, w / 5);
  r.fillRect(x + (w - ruleW) / 2, ty + 6, ruleW, 1, false);
  if (authorH) {
    // Full name when it fits, else the surname, else a truncated surname.
    std::string name = author;
    if (r.getTextWidth(authorFont, name.c_str()) > innerW) {
      const size_t space = name.rfind(' ');
      if (space != std::string::npos) name = name.substr(space + 1);
    }
    const std::string fitted = r.truncatedText(authorFont, name.c_str(), innerW);
    const int aw = r.getTextWidth(authorFont, fitted.c_str());
    r.drawText(authorFont, x + (w - aw) / 2, ty + 14, fitted.c_str(), false);
  }
}

bool writeThumb(GfxRenderer& renderer, const std::string& path, const int width, const int height, const char* title,
                const char* author) {
  // Landscape CCW maps logical pixels 1:1 onto panel memory, so each strip
  // band holds whole image rows in BMP bit order (0 = black, 1 = white).
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  if (width <= 0 || height <= 0 || width > renderer.getScreenWidth() || height > renderer.getScreenHeight()) {
    renderer.setOrientation(savedOrientation);
    LOG_ERR("GCOV", "Unsupported cover size %dx%d", width, height);
    return false;
  }
  const int stride = renderer.getDisplayWidthBytes();
  auto strip = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(stride) * STRIP_ROWS);
  if (!strip) {
    renderer.setOrientation(savedOrientation);
    LOG_ERR("GCOV", "OOM: %d byte strip", stride * STRIP_ROWS);
    return false;
  }

  const int rowBytes = (width + 31) / 32 * 4;
  uint8_t header[62] = {};
  header[0] = 'B';
  header[1] = 'M';
  writeLE32(header + 2, sizeof(header) + rowBytes * height);
  writeLE32(header + 10, sizeof(header));
  writeLE32(header + 14, 40);
  writeLE32(header + 18, width);
  writeLE32(header + 22, static_cast<uint32_t>(-height));  // top-down
  writeLE16(header + 26, 1);
  writeLE16(header + 28, 1);
  writeLE32(header + 34, rowBytes * height);
  writeLE32(header + 46, 2);
  memset(header + 58, 0xFF, 3);  // palette: 0 black, 1 white

  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForWrite("GCOV", path, file) && file.write(header, sizeof(header)) == sizeof(header)) {
      const auto clip = renderer.getClipRect();
      renderer.setClipRect(0, 0, width, height);
      ok = true;
      for (int band = 0; band < height && ok; band += STRIP_ROWS) {
        const int rows = std::min(STRIP_ROWS, height - band);
        memset(strip.get(), 0xFF, static_cast<size_t>(stride) * STRIP_ROWS);
        renderer.beginStripTarget(strip.get(), band, rows);
        draw(renderer, 0, 0, width, height, title, author);
        renderer.endStripTarget();
        for (int row = 0; row < rows && ok; row++) {
          uint8_t* src = strip.get() + row * stride;
          // Pad bits past the right edge white, then write the 4-byte aligned row.
          if (width % 8) src[width / 8] |= static_cast<uint8_t>(0xFF >> (width % 8));
          ok = file.write(src, rowBytes) == static_cast<size_t>(rowBytes);
        }
      }
      renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);
    }
  }
  renderer.setOrientation(savedOrientation);
  if (!ok) {
    LOG_ERR("GCOV", "Failed to write %s", path.c_str());
    Storage.remove(path.c_str());
  }
  return ok;
}

}  // namespace generated_cover
