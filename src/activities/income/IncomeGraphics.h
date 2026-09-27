#pragma once

#include <cstdint>

class GfxRenderer;

namespace income_gfx {

// 4x4 ordered (Bayer) dither: level 0 = white, 16 = black. Low levels give
// evenly spaced isolated dots, which stay crisp on a 1-bit e-ink panel.
void fillDither(const GfxRenderer& renderer, int x, int y, int w, int h, int level);

// Width in pixels of `text` drawn with pixelSize-sized blocks.
int pixelTextWidth(const char* text, int pixelSize);

// Chunky 5x7 pixel digits ("0-9$,.-:") as solid blocks with a dithered drop shadow.
void drawPixelText(const GfxRenderer& renderer, int x, int y, const char* text, int pixelSize);

// Small-caps style label: uppercase text with extra letter spacing.
void drawTrackedLabel(const GfxRenderer& renderer, int fontId, int x, int y, const char* text, int tracking);
int trackedLabelWidth(const GfxRenderer& renderer, int fontId, const char* text, int tracking);

// Three-step refresh indicator (connecting, fetching, done) followed by a label,
// in SMALL_FONT. Finished steps are solid, the current one dithered. Returns the bottom y.
int drawStepStatus(const GfxRenderer& renderer, int x, int y, int width, bool alignRight, int currentStep,
                   const char* label);

// Greedy word wrap into up to maxLines lines of maxWidth; the last line is
// truncated with "..." if text remains. Returns the number of lines written.
int wrapText(const GfxRenderer& renderer, int fontId, const char* text, int maxWidth, char (*lines)[96], int maxLines);

// Whole dollars with thousands separators ("$1,234"), cents dropped (rounded toward zero).
void formatUsd(int64_t cents, char* buf, int bufSize);

// Chart labels: "$950", "$4,321", then "$34k" from $10,000; rounded toward zero.
void formatUsdShort(int64_t cents, char* buf, int bufSize);

}  // namespace income_gfx
