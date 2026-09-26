#pragma once

#include <GfxRenderer.h>

#include <cstdint>

// 1-bit artwork for 2048: a procedural rounded numeral face (no font assets) and
// one texture per tile value, from a plain white 2 to a crowned black 2048.
namespace g2048 {

// 8x8 fill textures; row bytes, MSB is the leftmost pixel.
enum Pattern : uint8_t { PAT_WHITE, PAT_BLACK, PAT_BOARD, PAT_COUNT };

// Fills a rounded rect with a texture anchored at (anchorX, anchorY). Patterns
// beyond PAT_COUNT are the tile textures from tilePattern().
void fillRounded(const GfxRenderer& renderer, int x, int y, int w, int h, int radius, uint8_t pattern, int anchorX,
                 int anchorY);

// Draws `value` in the numeral face, centred on (cx, cy) and as large as fits in
// maxW x maxH. `halo` px of the opposite colour keeps digits legible on textures.
void drawNumber(const GfxRenderer& renderer, int cx, int cy, uint32_t value, int maxW, int maxH, bool black, int halo);

// A whole tile for exponent `exp` (1 = 2 ... 11 = 2048, higher values share the
// 4096+ style). Without numerals it is a texture swatch, used for the ladder.
void drawTile(const GfxRenderer& renderer, int x, int y, int size, uint8_t exp, bool numerals);

// One-frame hint that a tile has just appeared.
void drawNewMark(const GfxRenderer& renderer, int x, int y, int size);

// One-frame ring around a tile that a merge just produced.
void drawMergeMark(const GfxRenderer& renderer, int x, int y, int size);

// Small crown, width w, centred on cx with its base at `baseY`.
void drawCrown(const GfxRenderer& renderer, int cx, int baseY, int w, bool black);

}  // namespace g2048
