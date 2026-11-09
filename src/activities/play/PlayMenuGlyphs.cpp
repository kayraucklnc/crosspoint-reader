#include "PlayMenuGlyphs.h"

#include <algorithm>

#include <cstdint>
#include <utility>

#include "Game2048Art.h"
#include "SokobanArt.h"
#include "SokobanBoard.h"
#include "fontIds.h"

namespace play::glyphs {

void snake(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  constexpr int CELL = 12;
  static constexpr int8_t PATH[][2] = {{0, 4}, {1, 4}, {2, 4}, {2, 3}, {2, 2}, {3, 2}, {4, 2}, {4, 1}};
  for (const auto& p : PATH) renderer.fillRect(x + 6 + p[0] * CELL, y + 6 + p[1] * CELL, CELL - 2, CELL - 2, true);
  renderer.fillRect(x + 6 + 4 * CELL + 3, y + 6 + 4 * CELL + 3, CELL - 6, CELL - 6, true);
}

void blackjack(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  // Two fanned cards.
  renderer.fillRoundedRect(x + 10, y + 12, 30, 44, 4, Color::White);
  renderer.drawRoundedRect(x + 10, y + 12, 30, 44, 2, 4, true);
  renderer.fillRoundedRect(x + 30, y + 18, 30, 44, 4, Color::White);
  renderer.drawRoundedRect(x + 30, y + 18, 30, 44, 2, 4, true);
  renderer.drawText(SMALL_FONT_ID, x + 36, y + 22, "A", true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, x + 14, y + 16, "K", true, EpdFontFamily::BOLD);
}

void sketch(const GfxRenderer& renderer, const int x, const int y, const int size) {
  // A quick pencil face.
  const int c = size / 2;
  renderer.drawRoundedRect(x + c - 22, y + 8, 44, 54, 2, 22, true);
  renderer.fillRect(x + c - 11, y + 28, 4, 4, true);
  renderer.fillRect(x + c + 7, y + 28, 4, 4, true);
  renderer.drawLine(x + c - 8, y + 46, x + c + 8, y + 46, 2, true);
  renderer.drawLine(x + c - 30, y + 10, x + c + 30, y + 4, 2, true);
  renderer.drawLine(x + c - 20, y + 70, x + c - 30, y + 72, 2, true);
  renderer.drawLine(x + c + 20, y + 70, x + c + 30, y + 72, 2, true);
}

void sokoban(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  // A walled corridor: keeper, crate, goal - drawn with the game's own tiles.
  static constexpr uint8_t ROW[3] = {
      ::sokoban::CELL_FLOOR | ::sokoban::CELL_PLAYER | (::sokoban::RIGHT << ::sokoban::FACING_SHIFT),
      ::sokoban::CELL_FLOOR | ::sokoban::CELL_BOX, ::sokoban::CELL_FLOOR | ::sokoban::CELL_GOAL};
  constexpr int T = 24;
  for (int i = 0; i < 3; i++) {
    // Walls: open to the corridor and the card, closed between neighbours.
    const int ends = (i == 0 ? 1 << ::sokoban::LEFT : 0) | (i == 2 ? 1 << ::sokoban::RIGHT : 0);
    const uint8_t open = 1 << ::sokoban::UP | 1 << ::sokoban::DOWN | ends;
    ::sokoban::drawTile(renderer, x + i * T, y, T, ::sokoban::CELL_WALL, x, y, open);
    ::sokoban::drawTile(renderer, x + i * T, y + T, T, ROW[i], x, y);
    ::sokoban::drawTile(renderer, x + i * T, y + 2 * T, T, ::sokoban::CELL_WALL, x, y, open);
  }
}

void game2048(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  // A 2x2 of tiles climbing the texture ladder.
  constexpr int T = 32, G = 4;
  static constexpr uint8_t EXPS[4] = {1, 4, 7, 11};
  for (int i = 0; i < 4; i++) {
    g2048::drawTile(renderer, x + 2 + (i % 2) * (T + G), y + 2 + (i / 2) * (T + G), T, EXPS[i], false);
  }
}

void voxel(const GfxRenderer& renderer, const int x, const int y, const int size) {
  // An isometric block with a shaded side.
  const int c = x + size / 2, t = y + 10;
  const int topX[4] = {c, c + 26, c, c - 26}, topY[4] = {t, t + 13, t + 26, t + 13};
  renderer.fillPolygon(topX, topY, 4, false);
  const int rightX[4] = {c, c + 26, c + 26, c}, rightY[4] = {t + 26, t + 13, t + 43, t + 56};
  renderer.fillPolygon(rightX, rightY, 4, true);
  for (int i = 0; i < 4; i++) renderer.drawLine(topX[i], topY[i], topX[(i + 1) % 4], topY[(i + 1) % 4], 2, true);
  renderer.drawLine(c - 26, t + 13, c - 26, t + 43, 2, true);
  renderer.drawLine(c - 26, t + 43, c, t + 56, 2, true);
  renderer.drawLine(c, t + 26, c, t + 56, 2, true);
}

void wordle(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  // Solved, misplaced and unused tiles.
  constexpr int TILE = 32;
  static constexpr uint8_t LOOK[4] = {2, 1, 0, 2};  // 2 solid, 1 hatched, 0 outline
  for (int i = 0; i < 4; i++) {
    const int tx = x + 4 + (i % 2) * (TILE + 4), ty = y + 4 + (i / 2) * (TILE + 4);
    if (LOOK[i] == 2) {
      renderer.fillRoundedRect(tx, ty, TILE, TILE, 4, Color::Black);
      continue;
    }
    renderer.drawRoundedRect(tx, ty, TILE, TILE, LOOK[i] ? 2 : 1, 4, true);
    for (int d = 6; LOOK[i] && d < 2 * TILE - 4; d += 6) {
      renderer.drawLine(tx + std::max(2, d - TILE + 2), ty + std::min(d, TILE - 2), tx + std::min(d, TILE - 2),
                        ty + std::max(2, d - TILE + 2), true);
    }
  }
}

void cards(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  // An ID card: a QR code on the left, name lines on the right.
  renderer.drawRoundedRect(x + 2, y + 12, 68, 48, 2, 6, true);
  constexpr int FINDER = 10;
  for (const auto [fx, fy] : {std::pair{0, 0}, std::pair{18, 0}, std::pair{0, 18}}) {
    renderer.drawRect(x + 10 + fx, y + 20 + fy, FINDER, FINDER, 2, true);
    renderer.fillRect(x + 13 + fx, y + 23 + fy, 4, 4, true);
  }
  renderer.fillRect(x + 22, y + 32, 4, 4, true);
  renderer.fillRect(x + 28, y + 38, 4, 4, true);
  renderer.fillRect(x + 40, y + 24, 22, 4, true);
  renderer.fillRect(x + 40, y + 33, 16, 3, true);
  renderer.fillRect(x + 40, y + 41, 19, 3, true);
}

}  // namespace play::glyphs
