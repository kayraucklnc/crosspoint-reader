#include "SokobanArt.h"

#include <algorithm>

#include "SokobanBoard.h"

namespace sokoban {
namespace {
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};
constexpr int HATCH = 4;
// Five-point star, outer radius 1000, inner 450, starting at the top.
constexpr int16_t STAR[10][2] = {{0, -1000}, {265, -364}, {951, -309}, {428, 139},   {588, 809},
                                 {0, 450},   {-588, 809}, {-428, 139}, {-951, -309}, {-265, -364}};

void disc(const GfxRenderer& r, const int cx, const int cy, const int radius, const Color color) {
  r.fillRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, color);
}

// Diagonal hatch anchored to the origin so walls join seamlessly, with a bold
// rule along every side that faces floor or the outside: a plan-drawing wall.
void drawWall(const GfxRenderer& r, const int x, const int y, const int s, const uint8_t edges, const int ox,
              const int oy) {
  r.fillRect(x, y, s, s, false);
  for (int py = y; py < y + s; py++) {
    const int phase = ((py - oy) - (x - ox)) % HATCH;
    for (int px = x + (phase + HATCH) % HATCH; px < x + s; px += HATCH) r.drawPixel(px, py, true);
  }
  const int rule = std::max(2, s / 12);
  if (edges & (1 << UP)) r.fillRect(x, y, s, rule, true);
  if (edges & (1 << DOWN)) r.fillRect(x, y + s - rule, s, rule, true);
  if (edges & (1 << LEFT)) r.fillRect(x, y, rule, s, true);
  if (edges & (1 << RIGHT)) r.fillRect(x + s - rule, y, rule, s, true);
  // Inside corners: close the notch where two walled sides meet an open diagonal.
  for (int c = 0; c < 4; c++) {
    const int a = c, b = (c + 1) % 4;
    if (!(edges & (0x10 << c)) || (edges & (1 << a)) || (edges & (1 << b))) continue;
    const int cx = (a == RIGHT || b == RIGHT) ? x + s - rule : x;
    const int cy = (a == DOWN || b == DOWN) ? y + s - rule : y;
    r.fillRect(cx, cy, rule, rule, true);
  }
}

void drawGoal(const GfxRenderer& r, const int cx, const int cy, const int s) {
  const int ring = std::max(4, s * 6 / 25);
  r.drawRoundedRect(cx - ring, cy - ring, ring * 2, ring * 2, std::max(1, s / 14), ring, true);
  disc(r, cx, cy, std::max(1, s / 12), Color::Black);
}

void drawCrate(const GfxRenderer& r, const int x, const int y, const int s, const bool onGoal) {
  const int inset = std::max(1, s / 10);
  const int bx = x + inset, by = y + inset, bs = s - 2 * inset;
  const int frame = std::max(2, s / 9);
  if (onGoal) {
    // Solid crate with a white check: done at a glance.
    r.fillRect(bx, by, bs, bs, true);
    r.drawRect(bx + frame, by + frame, bs - 2 * frame, bs - 2 * frame, 1, false);
    const int w = std::max(2, s / 9);
    r.drawLine(bx + bs * 24 / 100, by + bs * 52 / 100, bx + bs * 43 / 100, by + bs * 70 / 100, w, false);
    r.drawLine(bx + bs * 43 / 100, by + bs * 70 / 100, bx + bs * 77 / 100, by + bs * 30 / 100, w, false);
    return;
  }
  r.fillRect(bx, by, bs, bs, false);
  r.drawRect(bx, by, bs, bs, frame, true);
  // Cross braces inside the frame.
  const int in0 = frame, in1 = bs - frame - 1;
  const int brace = std::max(1, frame - 1);
  r.drawLine(bx + in0, by + in0, bx + in1, by + in1, brace, true);
  r.drawLine(bx + in0, by + in1, bx + in1, by + in0, brace, true);
}

void drawPlayer(const GfxRenderer& r, const int cx, const int cy, const int s, const int facing, const bool onGoal) {
  const int radius = std::max(3, s * 36 / 100);
  if (onGoal) {
    const int ring = radius + std::max(2, s / 10);
    r.drawRoundedRect(cx - ring, cy - ring, ring * 2, ring * 2, 1, ring, true);
  }
  disc(r, cx, cy, radius, Color::Black);
  // Two eyes looking along the last move.
  const int eye = std::max(1, s / 13);
  const int forward = radius * 42 / 100, side = radius * 42 / 100;
  const int fx = DX[facing] * forward, fy = DY[facing] * forward;
  const int px = -DY[facing] * side, py = DX[facing] * side;
  disc(r, cx + fx + px, cy + fy + py, eye, Color::White);
  disc(r, cx + fx - px, cy + fy - py, eye, Color::White);
}
}  // namespace

void drawTile(const GfxRenderer& renderer, const int x, const int y, const int size, const uint8_t visual,
              const int originX, const int originY, const uint8_t openSides) {
  if (visual & CELL_WALL) {
    drawWall(renderer, x, y, size, openSides, originX, originY);
    return;
  }
  renderer.fillRect(x, y, size, size, false);
  if (!(visual & CELL_FLOOR)) return;
  // A single corner speck per floor tile: a faint grid for counting squares.
  renderer.drawPixel(x, y, true);
  const int cx = x + size / 2, cy = y + size / 2;
  const bool goal = visual & CELL_GOAL;
  if (visual & CELL_BOX) {
    drawCrate(renderer, x, y, size, goal);
  } else if (visual & CELL_PLAYER) {
    drawPlayer(renderer, cx, cy, size, (visual >> FACING_SHIFT) & 3, goal);
  } else if (goal) {
    drawGoal(renderer, cx, cy, size);
  }
}

void drawStar(const GfxRenderer& renderer, const int cx, const int cy, const int radius, const bool filled) {
  int xs[10], ys[10];
  for (int i = 0; i < 10; i++) {
    xs[i] = cx + STAR[i][0] * radius / 1000;
    ys[i] = cy + STAR[i][1] * radius / 1000;
  }
  if (filled) {
    renderer.fillPolygon(xs, ys, 10, true);
    return;
  }
  for (int i = 0; i < 10; i++) renderer.drawLine(xs[i], ys[i], xs[(i + 1) % 10], ys[(i + 1) % 10], 2, true);
}

}  // namespace sokoban
