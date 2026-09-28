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
  float d[IncomeView::MAX_BUCKETS] = {};
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

// Draws one contiguous run of points [first, last] as a smoothed, filled curve.
void drawRun(const GfxRenderer& renderer, const float* values, const int first, const int last, const int plotLeft,
             const int plotWidth, const int count, const int baseline, const int plotHeight) {
  const int n = last - first + 1;
  float m[IncomeView::MAX_BUCKETS];
  monotoneTangents(values + first, n, m);
  const int startX = plotLeft + plotWidth * first / (count - 1);
  const int endX = plotLeft + plotWidth * last / (count - 1);
  int prevX = startX;
  int prevY = baseline - static_cast<int>(values[first] * plotHeight);
  for (int px = startX; px <= endX; px++) {
    const float u = static_cast<float>(px - startX) * (n - 1) / std::max(1, endX - startX);
    const float v = std::clamp(sampleCurve(values + first, m, n, u), 0.0f, 1.0f);
    const int py = baseline - static_cast<int>(v * plotHeight);
    fillColumn(renderer, px, py, baseline);
    for (int w = -LINE_WIDTH / 2; w <= LINE_WIDTH / 2; w++) renderer.drawLine(prevX, prevY + w, px, py + w, true);
    prevX = px;
    prevY = py;
  }
}
}  // namespace

void drawIncomeChart(const GfxRenderer& renderer, const IncomeView& view, const int x, const int y, const int width,
                     const int height) {
  const int n = view.count;
  if (n < 2) return;
  using State = IncomeView::State;

  const int plotLeft = x + SIDE_PAD;
  const int plotWidth = width - SIDE_PAD * 2;
  const int plotTop = y + TOP_PAD;
  const int baseline = y + height - LABEL_HEIGHT;
  const int plotHeight = baseline - plotTop;
  int64_t maxCents = 1;
  for (int i = 0; i < n; i++) {
    if (view.state[i] == State::DATA) maxCents = std::max(maxCents, view.cents[i]);
  }

  // Static, not stack: the chart only draws on the render task, one frame at a time.
  static float ys[IncomeView::MAX_BUCKETS];
  for (int i = 0; i < n; i++) ys[i] = static_cast<float>(view.cents[i]) / maxCents;
  const auto pointX = [&](const int i) { return plotLeft + plotWidth * i / (n - 1); };

  // The curve covers each run of buckets with data; future and missing ones leave a gap.
  for (int first = 0; first < n;) {
    if (view.state[first] != State::DATA) {
      first++;
      continue;
    }
    int last = first;
    while (last + 1 < n && view.state[last + 1] == State::DATA) last++;
    if (last > first) drawRun(renderer, ys, first, last, plotLeft, plotWidth, n, baseline, plotHeight);
    first = last + 1;
  }
  // Solid baseline under buckets that have data, dotted under the rest.
  for (int i = 0; i < n; i++) {
    const int left = i == 0 ? plotLeft : (pointX(i - 1) + pointX(i)) / 2;
    const int right = i == n - 1 ? plotLeft + plotWidth + 1 : (pointX(i) + pointX(i + 1)) / 2;
    if (view.state[i] == State::DATA) {
      renderer.fillRect(left, baseline, right - left, 2, true);
    } else {
      income_gfx::fillDither(renderer, left, baseline, right - left, 2, 6);
    }
  }

  // Value labels by priority: the present bucket, the peak, then left to right,
  // each skipped when it would crowd one already placed.
  static int order[IncomeView::MAX_BUCKETS];
  int orderCount = 0;
  int peak = -1;
  for (int i = 0; i < n; i++) {
    if (view.state[i] == State::DATA && (peak < 0 || view.cents[i] > view.cents[peak])) peak = i;
  }
  if (view.current >= 0 && view.state[view.current] == State::DATA) order[orderCount++] = view.current;
  if (peak >= 0 && peak != view.current) order[orderCount++] = peak;
  for (int i = 0; i < n; i++) {
    if (view.state[i] == State::DATA && i != view.current && i != peak) order[orderCount++] = i;
  }
  static int placedLeft[IncomeView::MAX_BUCKETS];
  static int placedRight[IncomeView::MAX_BUCKETS];
  int placed = 0;
  for (int k = 0; k < orderCount; k++) {
    const int i = order[k];
    const int cx = pointX(i);
    const int cy = baseline - static_cast<int>(std::clamp(ys[i], 0.0f, 1.0f) * plotHeight);
    drawDot(renderer, cx, cy, i == view.current);
    char value[16];
    income_gfx::formatUsdShort(view.cents[i], value, sizeof(value));
    const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
    const int valueX = cx - valueWidth / 2;
    bool crowded = false;
    for (int j = 0; j < placed && !crowded; j++) {
      crowded = valueX < placedRight[j] + 8 && valueX + valueWidth + 8 > placedLeft[j];
    }
    // Zero buckets (before an account's first sale, say) keep their dot but not a "$0" label.
    if (crowded || (view.cents[i] == 0 && i != view.current)) continue;
    renderer.drawText(UI_10_FONT_ID, valueX, cy - VALUE_GAP, value, true, EpdFontFamily::BOLD);
    placedLeft[placed] = valueX;
    placedRight[placed] = valueX + valueWidth;
    placed++;
  }

  for (int i = 0; i < n; i++) {
    const int cx = pointX(i);
    const bool hasData = view.state[i] == State::DATA;
    const bool highlighted = i == view.current;
    const char* label = view.labels[i];
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
    const int labelX = cx - labelWidth / 2;
    renderer.drawText(SMALL_FONT_ID, labelX, baseline + 10, label);
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    if (highlighted) renderer.fillRect(labelX, baseline + 10 + lineHeight, labelWidth, 2, true);
    if (!hasData) income_gfx::fadeRect(renderer, labelX, baseline + 10, labelWidth, lineHeight);
  }
}
