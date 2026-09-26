#pragma once

#include <GfxRenderer.h>

#include <cstdint>

// 1-bit Sokoban tile art, drawn procedurally at any tile size.
namespace sokoban {

// Hint overlay bit OR-ed onto Board::visual(): on a crate it adds an arrow for
// the push direction (in the facing bits), on floor it marks the walk there.
constexpr uint8_t TILE_HINT = 0x80;

// Draws one tile. `visual` is Board::visual() plus TILE_HINT; for walls `openSides` is
// Board::openSides(). (originX, originY) anchors the wall hatch so
// neighbouring walls join seamlessly.
void drawTile(const GfxRenderer& renderer, int x, int y, int size, uint8_t visual, int originX, int originY,
              uint8_t openSides = 0);

void drawStar(const GfxRenderer& renderer, int cx, int cy, int radius, bool filled);

// A small light bulb: "solved with a hint".
void drawBulb(const GfxRenderer& renderer, int cx, int cy, int size, bool black);

}  // namespace sokoban
