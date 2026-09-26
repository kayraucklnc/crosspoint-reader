#pragma once

#include <GfxRenderer.h>

#include <cstdint>

// 1-bit Sokoban tile art, drawn procedurally at any tile size.
namespace sokoban {

// Draws one tile. `visual` is Board::visual(); for walls `openSides` is
// Board::openSides(). (originX, originY) anchors the wall hatch so
// neighbouring walls join seamlessly.
void drawTile(const GfxRenderer& renderer, int x, int y, int size, uint8_t visual, int originX, int originY,
              uint8_t openSides = 0);

void drawStar(const GfxRenderer& renderer, int cx, int cy, int radius, bool filled);

}  // namespace sokoban
