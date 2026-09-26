#include "IncomeChart.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "IncomeGraphics.h"
#include "fontIds.h"

namespace {
constexpr int LINE_WIDTH = 3;
constexpr int DOT_RADIUS = 5;
constexpr int VALUE_GAP = 30;     // value label baseline offset above its point
constexpr int LABEL_HEIGHT = 30;  // day labels under the baseline
constexpr int TOP_PAD = 36;       // room for the peak's value label
constexpr int SIDE_PAD = 32;      // keeps edge labels on screen

// Stepped fill bands, measured downward from the curve: a dense-ish band
// hugging the line, then sparser dots. Bayer levels 4/2/1 of 16 are isolated,
// evenly spaced dots — no 50% checkerboard, so it stays crisp on e-ink.
struct FillBand {
  int depth;
  int level;
};
constexpr FillBand FILL_BANDS[] = {{22, 4}, {60, 2}, {100000, 1}};

// Monotone cubic (Fritsch-Carlson) tangents: smooth, never overshooting a peak or dipping below zero.
void monotoneTangents(const float* ys, const int n, float* m) {
  float d[IncomeChartData::MAX_POINTS] = {};
  for (int i = 0; i < n - 1; i++) d[i] = ys[i + 1] - ys[i];
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (int i = 1; i < n - 1; i++) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
  for (int i = 0; i < n - 1; i++) {
    if (d[i] == 0) {
      m[i] = m[i + 1] = 0;
      continue;
    }
    const float a = m[i] / d[i];
    const float b = m[i + 1] / d[i];
    const float s = a * a + b * b;
    if (s > 9) {
      const float t = 3 / std::sqrt(s);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
}

float sampleCurve(const float* ys, const float* m, const int n, const float u) {
  const int i = std::min(static_cast<int>(u), n - 2);
  const float t = u - i;
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * ys[i] + (t3 - 2 * t2 + t) * m[i] + (-2 * t3 + 3 * t2) * ys[i + 1] +
         (t3 - t2) * m[i + 1];
}

void fillColumn(const GfxRenderer& renderer, const int px, const int top, const int baseline) {
  int y = top;
  for (const FillBand& band : FILL_BANDS) {
    const int bandEnd = std::min(baseline, top + band.depth);
    if (bandEnd > y) income_gfx::fillDither(renderer, px, y, 1, bandEnd - y, band.level);
    y = bandEnd;
    if (y >= baseline) return;
  }
}

void drawDot(const GfxRenderer& renderer, const int cx, const int cy, const bool hollow) {
  const int r = hollow ? DOT_RADIUS + 2 : DOT_RADIUS;
  for (int dy = -r; dy <= r; dy++) {
    for (int dx = -r; dx <= r; dx++) {
      const int d2 = dx * dx + dy * dy;
      if (d2 > r * r) continue;
      const bool ring = d2 >= (r - 2) * (r - 2);
      renderer.drawPixel(cx + dx, cy + dy, !hollow || ring);
    }
  }
}

void dayLabel(const time_t dayStart, const bool isToday, char* buf, const int bufSize) {
  if (isToday) {
    snprintf(buf, bufSize, "%s", tr(STR_INCOME_TODAY));
    return;
  }
  struct tm local;
  localtime_r(&dayStart, &local);
  strftime(buf, bufSize, "%a", &local);
}
}  // namespace

void drawIncomeChart(const GfxRenderer& renderer, const IncomeChartData& data, const int x, const int y,
                     const int width, const int height) {
  const int n = data.count;
  if (n < 2) return;

  const int plotLeft = x + SIDE_PAD;
  const int plotWidth = width - SIDE_PAD * 2;
  const int plotTop = y + TOP_PAD;
  const int baseline = y + height - LABEL_HEIGHT;
  const int plotHeight = baseline - plotTop;
  const int64_t maxCents = std::max<int64_t>(1, *std::max_element(data.cents, data.cents + n));

  float ys[IncomeChartData::MAX_POINTS];
  float m[IncomeChartData::MAX_POINTS];
  for (int i = 0; i < n; i++) ys[i] = static_cast<float>(data.cents[i]) / maxCents;
  monotoneTangents(ys, n, m);

  int prevX = plotLeft;
  int prevY = baseline - static_cast<int>(ys[0] * plotHeight);
  for (int px = plotLeft; px <= plotLeft + plotWidth; px++) {
    const float u = static_cast<float>(px - plotLeft) * (n - 1) / plotWidth;
    const float v = std::clamp(sampleCurve(ys, m, n, u), 0.0f, 1.0f);
    const int py = baseline - static_cast<int>(v * plotHeight);
    fillColumn(renderer, px, py, baseline);
    for (int w = -LINE_WIDTH / 2; w <= LINE_WIDTH / 2; w++) renderer.drawLine(prevX, prevY + w, px, py + w, true);
    prevX = px;
    prevY = py;
  }
  renderer.fillRect(plotLeft, baseline, plotWidth + 1, 2, true);

  // Skip a value label when it would crowd its left neighbour's.
  int lastLabelRight = -1000;
  for (int i = 0; i < n; i++) {
    const int cx = plotLeft + plotWidth * i / (n - 1);
    const int cy = baseline - static_cast<int>(ys[i] * plotHeight);
    const bool isToday = i == n - 1;
    drawDot(renderer, cx, cy, isToday);

    char value[16];
    income_gfx::formatUsdShort(data.cents[i], value, sizeof(value));
    const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
    const int valueX = cx - valueWidth / 2;
    if (valueX > lastLabelRight + 8) {
      renderer.drawText(UI_10_FONT_ID, valueX, cy - VALUE_GAP, value, true, EpdFontFamily::BOLD);
      lastLabelRight = valueX + valueWidth;
    }

    char label[16];
    dayLabel(data.dayStart[i], isToday, label, sizeof(label));
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, cx - labelWidth / 2, baseline + 10, label);
  }
}
