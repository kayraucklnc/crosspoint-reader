#include "LibraryArt.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#include "fontIds.h"

namespace library_art {
namespace {
constexpr uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
constexpr int SPINE_RAISE = 10;
constexpr int SEGMENTS = 6;
constexpr uint32_t CLOCK_VALID_AFTER = 1600000000;  // 2020: anything earlier is an unset RTC

uint32_t mix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x7feb352d;
  h ^= h >> 15;
  h *= 0x846ca68b;
  h ^= h >> 16;
  return h;
}

// Tone 0 black .. 3 white. `cloth` swaps the dot screens for woven stripes.
void fillTone(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int tone,
              const bool cloth) {
  if (w <= 0 || h <= 0) return;
  switch (tone) {
    case 0:
      r.fillRect(x, y, w, h, true);
      return;
    case 3:
      r.fillRect(x, y, w, h, false);
      return;
    default:
      break;
  }
  r.fillRect(x, y, w, h, false);
  if (!cloth) {
    r.fillRectDither(x, y, w, h, tone == 1 ? Color::DarkGray : Color::LightGray);
    return;
  }
  // Woven binding: fine vertical threads, denser for the darker tone.
  const int pitch = tone == 1 ? 2 : 3;
  for (int px = x; px < x + w; px++) {
    if ((px % pitch) == 0) r.fillRect(px, y, 1, h, true);
  }
  if (tone == 1) {
    for (int py = y; py < y + h; py += 4) r.fillRect(x, py, w, 1, true);
  }
}

bool darkTone(const int tone) { return tone <= 1; }

// Sets `text` along the spine, reading bottom to top. Prefers one line in
// `font`, then two lines on a wide spine, then the small face, then truncates.
void drawRotated(const GfxRenderer& r, int font, const int x, const int w, const int yTop, const int yBottom,
                 const char* text, const bool black, const EpdFontFamily::Style style, const bool plate,
                 const bool plateBlack) {
  const int length = yBottom - yTop - (plate ? 8 : 0);
  if (!text || !text[0] || length < 12) return;
  std::vector<std::string> lines;
  if (r.getTextWidth(font, text, style) > length) {
    const int twoLineHeight = r.getTextHeight(SMALL_FONT_ID) * 2 + 2;
    if (w - 6 >= twoLineHeight) {
      font = SMALL_FONT_ID;
      lines = r.wrappedText(font, text, length, 2, style);
    } else {
      font = SMALL_FONT_ID;
    }
  }
  if (lines.empty()) lines.push_back(r.truncatedText(font, text, length, style));
  int tw = 0;
  for (const auto& line : lines) tw = std::max(tw, r.getTextWidth(font, line.c_str(), style));
  const int lh = r.getTextHeight(font) + 1;
  const int block = lh * static_cast<int>(lines.size());
  const int ty = yBottom - (yBottom - yTop - tw) / 2;
  if (plate) {
    r.fillRect(x + 3, ty - tw - 4, w - 6, tw + 8, plateBlack);
    r.drawRect(x + 3, ty - tw - 4, w - 6, tw + 8, 1, true);
  }
  int tx = x + (w - block) / 2;
  for (const auto& line : lines) {
    r.drawTextRotated90CW(font, tx, ty, line.c_str(), black, style);
    tx += lh;
  }
}

// Last word of a display author ("Herman Melville" -> "Melville").
std::string surnameOf(const char* author) {
  std::string s = author ? author : "";
  while (!s.empty() && s.back() == ' ') s.pop_back();
  const size_t space = s.rfind(' ');
  return space == std::string::npos ? s : s.substr(space + 1);
}
}  // namespace

void drawPlank(const GfxRenderer& r, const int x, const int y, const int width, const uint32_t seed) {
  r.fillRect(x, y, width, 4, false);
  r.fillRectDither(x, y, width, 4, Color::LightGray);
  r.fillRect(x, y + 3, width, PLANK_HEIGHT - 3, true);
  // Two broken, gently waving grain lines across the front face.
  for (int line = 0; line < 2; line++) {
    const uint32_t s = mix(seed + line * 977);
    const float phase = static_cast<float>(s % 97);
    const int baseY = y + 7 + line * 4;
    for (int px = x + 2; px < x + width - 2; px++) {
      if (mix(s + px / 9) % 5 == 0) continue;
      const int wave = static_cast<int>(std::sin((px + phase) / (13.0f + line * 5)) * 1.4f);
      r.drawPixel(px, baseY + wave, false);
    }
  }
  // Shadow cast onto the compartment below.
  for (int py = y + PLANK_HEIGHT; py < y + PLANK_HEIGHT + 4; py++) {
    const int strength = (y + PLANK_HEIGHT + 4 - py) * 3;
    for (int px = x; px < x + width; px++) {
      if (BAYER4[py & 3][px & 3] < strength) r.drawPixel(px, py, true);
    }
  }
}

void drawUpright(const GfxRenderer& r, const int x, const int y, const int width, const int height,
                 const uint32_t seed) {
  r.fillRect(x, y, width, height, true);
  for (int col = 0; col < 2; col++) {
    const int gx = x + 2 + col * (width - 5);
    const uint32_t s = mix(seed + col * 31);
    for (int py = y + 2; py < y + height - 2; py++) {
      if (mix(s + py / 11) % 4 == 0) continue;
      const int wave = static_cast<int>(std::sin((py + static_cast<int>(s % 61)) / 17.0f) * 1.2f);
      r.drawPixel(gx + wave, py, false);
    }
  }
}

void drawBackPanel(const GfxRenderer& r, const int x, const int y, const int width, const int height) {
  for (int py = y + 3; py < y + height; py += 6) {
    const int offset = ((py - y) / 6) % 2 ? 3 : 0;
    for (int px = x + 2 + offset; px < x + width; px += 6) r.drawPixel(px, py, true);
  }
}

int spineWidthFor(const uint32_t fileSize) {
  // Log scale (in half-octaves, integer only) so a 20 MB illustrated book is
  // thick without being a slab: ~19 px at 100 KB, ~30 px at 1 MB.
  const uint32_t kb = std::max<uint32_t>(40, fileSize / 1024);
  int halfOctaves = 0;
  for (uint32_t v = kb / 40; v > 1; v >>= 1) halfOctaves += 2;
  if ((kb / 40) * 2 >= (1u << (halfOctaves / 2)) * 3) halfOctaves++;  // top half of the octave
  return std::clamp(12 + halfOctaves * 11 / 4, 16, 42);
}

int spineHeightFor(const uint32_t seed, const int maxHeight) {
  const int lo = maxHeight * 76 / 100;
  return lo + static_cast<int>(mix(seed) % static_cast<uint32_t>(maxHeight - lo + 1));
}

uint16_t spineSigFromSeed(const uint32_t seed) {
  const uint32_t h = mix(seed);
  constexpr uint8_t BASES[] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 3};
  const int base = BASES[h % 10];
  int tones[SEGMENTS];
  for (int& t : tones) t = base;
  if ((h >> 8) % 3) tones[0] = static_cast<int>((h >> 12) & 3);
  if ((h >> 14) % 2) tones[SEGMENTS - 1] = static_cast<int>((h >> 18) & 3);
  uint16_t sig = SIG_VALID;
  for (int i = 0; i < SEGMENTS; i++) sig |= static_cast<uint16_t>(tones[i] << (i * 2));
  return sig;
}

uint16_t spineSigFromBmp(const std::string& bmpPath) {
  HalFile file;
  if (!Storage.exists(bmpPath.c_str()) || !Storage.openFileForRead("LART", bmpPath, file)) return 0;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return 0;
  const int w = bitmap.getWidth();
  const int h = bitmap.getHeight();
  if (w < 4 || h < SEGMENTS || w > 1024) return 0;
  const int stride = (w + 3) / 4;
  auto packed = makeUniqueNoThrow<uint8_t[]>(stride);
  auto row = makeUniqueNoThrow<uint8_t[]>(bitmap.getRowBytes());
  if (!packed || !row) return 0;
  uint32_t sums[SEGMENTS] = {};
  uint32_t counts[SEGMENTS] = {};
  const int x0 = w / 5;
  const int x1 = w - w / 5;
  for (int i = 0; i < h; i++) {
    if (bitmap.readNextRow(packed.get(), row.get()) != BmpReaderError::Ok) return 0;
    const int y = bitmap.isTopDown() ? i : h - 1 - i;
    const int seg = std::min(SEGMENTS - 1, y * SEGMENTS / h);
    for (int x = x0; x < x1; x++) sums[seg] += (packed[x >> 2] >> (6 - (x & 3) * 2)) & 3;
    counts[seg] += x1 - x0;
  }
  uint16_t sig = SIG_VALID;
  for (int i = 0; i < SEGMENTS; i++) {
    const int tone = counts[i] ? static_cast<int>((sums[i] * 2 + counts[i]) / (counts[i] * 2)) : 3;
    sig |= static_cast<uint16_t>(std::min(3, tone) << (i * 2));
  }
  return sig;
}

void drawSpine(const GfxRenderer& r, const Spine& s) {
  const int top = s.baseY - s.height - (s.focused ? SPINE_RAISE : 0);
  const int x = s.x;
  const int w = s.width;
  const int h = s.height;
  const uint32_t h1 = mix(s.seed);
  const bool cloth = (h1 & 7) == 0;

  // The spine takes the cover's dominant tone; its top and bottom echo the
  // cover's top and bottom where they differ.
  int tones[SEGMENTS];
  int histogram[4] = {};
  for (int i = 0; i < SEGMENTS; i++) {
    tones[i] = (s.sig >> (i * 2)) & 3;
    histogram[tones[i]] += (i == 0 || i == SEGMENTS - 1) ? 1 : 2;
  }
  const int base = static_cast<int>(std::max_element(histogram, histogram + 4) - histogram);
  const int topBand = tones[0] != base ? h / 8 : 0;
  const int bottomBand = tones[SEGMENTS - 1] != base ? h / 6 : 0;

  fillTone(r, x, top, w, h, base, cloth);
  if (topBand) fillTone(r, x, top, w, topBand, tones[0], false);
  if (bottomBand) fillTone(r, x, top + h - bottomBand, w, bottomBand, tones[SEGMENTS - 1], false);

  // Gilt rules: a pair of hairlines near the head and the foot.
  const bool baseDark = darkTone(base);
  if ((h1 >> 3) % 3 != 0) {
    const int ruleTop = top + (topBand ? topBand + 3 : 8);
    const int ruleBottom = top + h - (bottomBand ? bottomBand + 4 : 9);
    const bool ink = !baseDark;
    r.fillRect(x + 2, ruleTop, w - 4, 1, ink);
    r.fillRect(x + 2, ruleTop + 3, w - 4, 1, ink);
    r.fillRect(x + 2, ruleBottom, w - 4, 1, ink);
    r.fillRect(x + 2, ruleBottom - 3, w - 4, 1, ink);
  }

  const int font = w >= 24 ? UI_10_FONT_ID : SMALL_FONT_ID;
  const int yTop = top + (topBand ? topBand + 4 : 8);
  const int yBottom = top + h - (bottomBand ? bottomBand + 4 : 8);
  const bool plate = base == 1 || base == 2;
  const bool textBlack = plate ? base == 1 : base >= 2;
  drawRotated(r, font, x, w, yTop, yBottom, s.title, textBlack, EpdFontFamily::BOLD, plate, base == 2);
  if (bottomBand >= 24 && w >= 22 && s.author && s.author[0] && !s.archived) {
    const std::string surname = surnameOf(s.author);
    drawRotated(r, SMALL_FONT_ID, x, w, top + h - bottomBand + 2, top + h - 2, surname.c_str(),
                !darkTone(tones[SEGMENTS - 1]), EpdFontFamily::REGULAR, false, false);
  }

  if (s.archived) {
    dim(r, x, top, w, h, 4);
    const int band = std::min(54, h / 2);
    const int by = top + h - band - 6;
    r.fillRect(x, by, w, band, true);
    r.fillRect(x + 2, by + 2, w - 4, 1, false);
    r.fillRect(x + 2, by + band - 3, w - 4, 1, false);
    drawRotated(r, SMALL_FONT_ID, x, w, by + 2, by + band - 2, tr(STR_LIB_READ_STAMP), false, EpdFontFamily::REGULAR,
                false, false);
  } else if (s.finished) {
    // Ribbon marker hanging from the head.
    const int rx = x + w / 2 - 3;
    const int ribbonX[] = {rx, rx + 6, rx + 6, rx + 3, rx};
    const int ribbonY[] = {top, top, top + 16, top + 12, top + 16};
    r.fillPolygon(ribbonX, ribbonY, 5, !baseDark);
  } else if (s.inProgress) {
    // A bookmark peeking out of the top edge.
    const int bx = x + w - 10;
    r.fillRect(bx, top - 7, 6, 14, false);
    r.drawRect(bx, top - 7, 6, 14, 1, true);
  }

  r.drawRect(x, top, w, h, s.focused ? 2 : 1, true);
  if (s.focused) {
    // Pulled out: a shadow on its right and a marker on the board below.
    for (int py = top + 3; py < s.baseY; py++) {
      for (int px = x + w; px < x + w + 3; px++) {
        if (BAYER4[py & 3][px & 3] < 8) r.drawPixel(px, py, true);
      }
    }
    const int cx = x + w / 2;
    const int markX[] = {cx - 6, cx + 6, cx};
    const int markY[] = {s.baseY + PLANK_HEIGHT - 2, s.baseY + PLANK_HEIGHT - 2, s.baseY + 5};
    r.fillPolygon(markX, markY, 3, false);
  }
}

void drawBookend(const GfxRenderer& r, const int x, const int baseY, const int height) {
  r.fillRect(x, baseY - height, 4, height, true);
  r.fillRect(x, baseY - 4, 22, 4, true);
  r.fillRect(x + 4, baseY - height + 2, 1, height - 6, false);
}

void dim(const GfxRenderer& r, const int x, const int y, const int width, const int height, const int level) {
  for (int py = y; py < y + height; py++) {
    for (int px = x; px < x + width; px++) {
      if (BAYER4[py & 3][px & 3] < level) r.drawPixel(px, py, false);
    }
  }
}

void drawArchiveStamp(const GfxRenderer& r, const int x, const int y, const int width, const int height) {
  dim(r, x, y, width, height, 5);
  const bool small = height < 120;
  const int font = small ? SMALL_FONT_ID : UI_12_FONT_ID;
  const int band = std::max(small ? 14 : 30, height / 7);
  const int by = y + height * 64 / 100;
  r.fillRect(x, by, width, band, true);
  if (!small) {
    r.fillRect(x + 3, by + 3, width - 6, 1, false);
    r.fillRect(x + 3, by + band - 4, width - 6, 1, false);
  }
  const char* label = tr(STR_LIB_READ_STAMP);
  const int tw = r.getTextWidth(font, label, EpdFontFamily::BOLD);
  const int th = r.getLineHeight(font);
  r.drawText(font, x + (width - tw) / 2, by + (band - th) / 2 + (small ? 0 : 1), label, false, EpdFontFamily::BOLD);
}

void drawProgressBar(const GfxRenderer& r, const int x, const int y, const int width, const int height,
                     const int percent) {
  if (percent < 0) return;
  r.fillRect(x, y, width, height, false);
  r.drawRect(x, y, width, height, 1, true);
  const int fill = (width - 2) * std::min(percent, 100) / 100;
  if (fill > 0) r.fillRect(x + 1, y + 1, fill, height - 2, true);
}

void drawCheck(const GfxRenderer& r, const int x, const int y, const int size) {
  r.drawLine(x, y + size / 2, x + size / 3, y + size, 2, true);
  r.drawLine(x + size / 3, y + size, x + size, y, 2, true);
}

void drawCoverPlaceholder(const GfxRenderer& r, const int x, const int y, const int width, const int height) {
  r.fillRect(x, y, width, height, false);
  r.fillRectDither(x, y, width, height, Color::LightGray);
  r.drawRect(x, y, width, height, 1, true);
}

void formatDuration(char* out, const size_t size, const uint32_t seconds) {
  if (seconds == 0) {
    out[0] = '\0';
  } else if (seconds >= 3600) {
    snprintf(out, size, tr(STR_LIB_HOURS_MINUTES), static_cast<int>(seconds / 3600),
             static_cast<int>(seconds / 60 % 60));
  } else {
    snprintf(out, size, tr(STR_LIB_MINUTES), static_cast<int>(std::max<uint32_t>(1, seconds / 60)));
  }
}

void formatLastRead(char* out, const size_t size, const uint32_t epoch) {
  if (epoch == 0) {
    snprintf(out, size, "%s", tr(STR_LIB_NEVER));
    return;
  }
  const time_t now = time(nullptr);
  if (now > CLOCK_VALID_AFTER && static_cast<uint32_t>(now) >= epoch) {
    const long days = static_cast<long>(now / 86400) - static_cast<long>(epoch / 86400);
    if (days == 0) {
      snprintf(out, size, "%s", tr(STR_LIB_TODAY));
      return;
    }
    if (days == 1) {
      snprintf(out, size, "%s", tr(STR_LIB_YESTERDAY));
      return;
    }
    if (days < 30) {
      snprintf(out, size, tr(STR_LIB_DAYS_AGO), static_cast<int>(days));
      return;
    }
  }
  const time_t t = epoch;
  struct tm tmv{};
  gmtime_r(&t, &tmv);
  snprintf(out, size, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

}  // namespace library_art
