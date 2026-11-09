#pragma once

#include <GfxRenderer.h>

// Procedural 72x72 previews for the Play menu cards (no bitmap assets).
// Each draws inside the square (x, y, size, size). A game may instead expose
// its own static icon function with the same signature (see Minesweeper).
namespace play::glyphs {

void snake(const GfxRenderer& renderer, int x, int y, int size);
void blackjack(const GfxRenderer& renderer, int x, int y, int size);
void sketch(const GfxRenderer& renderer, int x, int y, int size);
void sokoban(const GfxRenderer& renderer, int x, int y, int size);
void game2048(const GfxRenderer& renderer, int x, int y, int size);
void voxel(const GfxRenderer& renderer, int x, int y, int size);
void wordle(const GfxRenderer& renderer, int x, int y, int size);
void cards(const GfxRenderer& renderer, int x, int y, int size);

}  // namespace play::glyphs
