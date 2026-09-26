#include "SleepSnapshot.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace sleep_snapshot {
namespace {

constexpr int HATCH_PERIOD = 6;  // diagonal stripes: 2 px ink, 4 px paper
constexpr int HATCH_INK = 2;
constexpr int RULE_WIDTH = 2;
constexpr int ICON_RADIUS = 6;
constexpr int ICON_SIZE = ICON_RADIUS * 2 + 2;
constexpr int PAD_X = 9;
constexpr int PAD_Y = 4;
constexpr int ICON_GAP = 6;
constexpr int STACK_GAP = 6;
constexpr int FONT = UI_10_FONT_ID;

void hatchRow(const GfxRenderer& renderer, const int y, const int fromX, const int toX) {
  for (int x = fromX; x < toX; x++) {
    if ((x + y) % HATCH_PERIOD < HATCH_INK) renderer.drawPixel(x, y, true);
  }
}

// Crescent opening to the upper right: a disc minus an offset disc.
void drawMoon(const GfxRenderer& renderer, const int cx, const int cy, const bool black) {
  constexpr int r = ICON_RADIUS;
  constexpr int ox = r / 2 + 1;
  constexpr int oy = -(r / 2);
  constexpr int cut = r - 1;
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      const bool inDisc = dx * dx + dy * dy <= r * r;
      const bool inCut = (dx - ox) * (dx - ox) + (dy - oy) * (dy - oy) <= cut * cut;
      if (inDisc && !inCut) renderer.drawPixel(cx + dx, cy + dy, black);
    }
  }
}

// IEC power symbol: a ring broken at the top with a bar through the gap.
void drawPower(const GfxRenderer& renderer, const int cx, const int cy, const bool black) {
  constexpr int r = ICON_RADIUS;
  constexpr int inner = r - 2;
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      const int d2 = dx * dx + dy * dy;
      const bool inGap = dy < 0 && dx >= -2 && dx <= 2;
      if (d2 <= r * r && d2 > inner * inner && !inGap) renderer.drawPixel(cx + dx, cy + dy, black);
    }
  }
  renderer.fillRect(cx - 1, cy - r - 1, 2, r + 1, black);
}

int segmentWidth(const GfxRenderer& renderer, const char* text, const EpdFontFamily::Style style) {
  return PAD_X + ICON_SIZE + ICON_GAP + renderer.getTextWidth(FONT, text, style) + PAD_X;
}

}  // namespace

void drawFrame(const GfxRenderer& renderer) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  constexpr int band = FRAME_WIDTH;
  renderer.fillRect(0, 0, width, band, false);
  renderer.fillRect(0, height - band, width, band, false);
  renderer.fillRect(0, 0, band, height, false);
  renderer.fillRect(width - band, 0, band, height, false);
  for (int y = 0; y < height; y++) {
    if (y < band || y >= height - band) {
      hatchRow(renderer, y, 0, width);
    } else {
      hatchRow(renderer, y, 0, band);
      hatchRow(renderer, y, width - band, width);
    }
  }
  renderer.drawRect(band, band, width - band * 2, height - band * 2, RULE_WIDTH, true);
}

int drawBadge(const GfxRenderer& renderer, const int x, const int y, const int width, const bool alignRight,
              const char* asOfClock) {
  char asleep[48];
  snprintf(asleep, sizeof(asleep), "%s %s", tr(STR_ASLEEP_AS_OF), asOfClock);
  // Click-to-wake needs only a press; otherwise the wake is verified as a hold.
  const char* wake = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ? tr(STR_PRESS_TO_WAKE)
                                                                                      : tr(STR_HOLD_TO_WAKE);

  const int lineHeight = renderer.getLineHeight(FONT);
  const int chipHeight = lineHeight + PAD_Y * 2;
  const int radius = chipHeight / 2;
  const int chipWidth = segmentWidth(renderer, asleep, EpdFontFamily::BOLD);
  const int wakeWidth = segmentWidth(renderer, wake, EpdFontFamily::REGULAR);
  const bool inline_ = chipWidth + wakeWidth <= width;
  const auto startX = [&](const int w) { return alignRight ? x + width - w : x; };

  const int chipX = startX(inline_ ? chipWidth + wakeWidth : chipWidth);
  const int iconCy = y + chipHeight / 2;
  const int textY = y + PAD_Y;
  if (inline_) {
    // One pill: the black "asleep" half, then an outlined "wake" half.
    renderer.drawRoundedRect(chipX, y, chipWidth + wakeWidth, chipHeight, 2, radius, true);
    renderer.fillRoundedRect(chipX, y, chipWidth, chipHeight, radius, true, false, true, false, Color::Black);
  } else {
    renderer.fillRoundedRect(chipX, y, chipWidth, chipHeight, radius, Color::Black);
  }
  drawMoon(renderer, chipX + PAD_X + ICON_SIZE / 2, iconCy, false);
  renderer.drawText(FONT, chipX + PAD_X + ICON_SIZE + ICON_GAP, textY, asleep, false, EpdFontFamily::BOLD);

  // Stacked, the wake line is plain text inset like the chip's content.
  const int wakeY = inline_ ? y : y + chipHeight + STACK_GAP;
  const int wakeIconX = inline_ ? chipX + chipWidth + PAD_X : startX(wakeWidth - PAD_X) + (alignRight ? 0 : PAD_X);
  drawPower(renderer, wakeIconX + ICON_SIZE / 2, wakeY + chipHeight / 2, true);
  renderer.drawText(FONT, wakeIconX + ICON_SIZE + ICON_GAP, wakeY + PAD_Y, wake, true);
  return wakeY + chipHeight;
}

}  // namespace sleep_snapshot
