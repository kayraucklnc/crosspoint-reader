#pragma once

class GfxRenderer;

// 1-bit vector art for Minesweeper: bevelled tiles, mines, flags, the cursor
// ring and a blocky 5x7 digit face that scales cleanly with the tile size.
// A tile occupies [x, x + size) and owns the grid line on its top/left edge.
namespace mines::art {

// `focused` draws the cursor's tile inverted (solid black, white flag).
void hiddenTile(const GfxRenderer& renderer, int x, int y, int size, bool flagged, bool focused = false);
void openTile(const GfxRenderer& renderer, int x, int y, int size);
// 1 plain, 2 bold, 3 bold in a ring, 4-8 inverted badge: weight grows with danger.
// `ink` false draws white-on-black for the focused tile.
void number(const GfxRenderer& renderer, int x, int y, int size, int value, bool ink = true);
void mine(const GfxRenderer& renderer, int cx, int cy, int radius, bool black);
void flag(const GfxRenderer& renderer, int cx, int cy, int size, bool ink = true);
// Diagonal strike over a wrongly flagged tile after a loss.
void cross(const GfxRenderer& renderer, int x, int y, int size, bool ink = true);
// Frame around the (inverted) focused tile; reaches 4 px outside it.
void cursor(const GfxRenderer& renderer, int x, int y, int size);
void clock(const GfxRenderer& renderer, int cx, int cy, int radius);

// Digits, ':' and '-' only; other characters are skipped.
int digitsWidth(const char* text, int scale, bool bold);
void digits(const GfxRenderer& renderer, const char* text, int x, int y, int scale, bool black, bool bold);
constexpr int DIGIT_HEIGHT = 7;

}  // namespace mines::art
