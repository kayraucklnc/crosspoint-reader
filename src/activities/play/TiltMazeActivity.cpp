#include "TiltMazeActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int CELL = 48;
constexpr int WALL = 4;  // wall thickness, centred on the cell boundary
constexpr int HALF_WALL = WALL / 2;
constexpr int HEADER_HEIGHT = 36;
constexpr int BALL_RADIUS = 11;
constexpr unsigned long STEP_MS = 20;
constexpr unsigned long MAX_CATCHUP_MS = 200;
constexpr float GRAVITY_PX = 1200.0f;
constexpr float BUTTON_PUSH_G = 0.5f;
constexpr float DEAD_ZONE_G = 0.04f;
constexpr float FRICTION = 0.985f;
constexpr float BOUNCE = 0.35f;
constexpr float MAX_SPEED = 400.0f;  // < BALL_RADIUS per step, so walls can't be tunnelled
constexpr int EXIT_REACH = 12;       // ball centre this close to an exit's centre wins

unsigned long bestMs = 0;  // survives until reboot

float deadZone(const float g) { return std::fabs(g) < DEAD_ZONE_G ? 0.0f : g; }
}  // namespace

void TiltMazeActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight() - metrics.buttonHintsHeight - HEADER_HEIGHT;
  cols = std::max(2, (width - 8) / CELL);
  rows = std::max(2, std::min((height - 8) / CELL, MAX_CELLS / cols));
  mazeX = (width - cols * CELL) / 2;
  mazeY = HEADER_HEIGHT + (height - rows * CELL) / 2;
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  tilt.begin();
  newMaze();
}

void TiltMazeActivity::onExit() {
  tilt.end();
  Activity::onExit();
}

uint32_t TiltMazeActivity::nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

bool TiltMazeActivity::rightWall(const int col, const int row) const {
  if (col >= cols - 1) return true;
  const int bit = (row * cols + col) * 2;
  return walls[bit >> 3] & (1 << (bit & 7));
}

bool TiltMazeActivity::bottomWall(const int col, const int row) const {
  if (row >= rows - 1) return true;
  const int bit = (row * cols + col) * 2 + 1;
  return walls[bit >> 3] & (1 << (bit & 7));
}

void TiltMazeActivity::clearWall(const int cell, const int bit) {
  const int index = cell * 2 + bit;
  walls[index >> 3] = static_cast<uint8_t>(walls[index >> 3] & ~(1 << (index & 7)));
}

// Recursive backtracker with an explicit stack: a spanning tree, so every cell
// (and so every exit) is reachable from the start.
void TiltMazeActivity::generate() {
  const int count = cols * rows;
  memset(walls, 0xFF, sizeof(walls));
  uint8_t visited[MAX_CELLS / 8] = {};
  uint8_t stack[MAX_CELLS];
  int depth = 0;
  const auto visit = [&visited](const int c) { visited[c >> 3] |= static_cast<uint8_t>(1 << (c & 7)); };
  const auto seen = [&visited](const int c) { return (visited[c >> 3] & (1 << (c & 7))) != 0; };
  const int start = static_cast<int>(nextRandom() % count);
  visit(start);
  stack[depth++] = static_cast<uint8_t>(start);
  while (depth > 0) {
    const int cell = stack[depth - 1];
    const int col = cell % cols, row = cell / cols;
    int options[4];
    int optionCount = 0;
    if (col > 0 && !seen(cell - 1)) options[optionCount++] = cell - 1;
    if (col < cols - 1 && !seen(cell + 1)) options[optionCount++] = cell + 1;
    if (row > 0 && !seen(cell - cols)) options[optionCount++] = cell - cols;
    if (row < rows - 1 && !seen(cell + cols)) options[optionCount++] = cell + cols;
    if (optionCount == 0) {
      depth--;
      continue;
    }
    const int next = options[nextRandom() % optionCount];
    const int low = std::min(cell, next);
    clearWall(low, std::abs(next - cell) == 1 ? 0 : 1);
    visit(next);
    stack[depth++] = static_cast<uint8_t>(next);
  }
}

void TiltMazeActivity::newMaze() {
  generate();
  // Two exits in the bottom row, one in each half.
  const int half = std::max(1, cols / 2);
  exits[0] = (rows - 1) * cols + static_cast<int>(nextRandom() % half);
  exits[1] = (rows - 1) * cols + half + static_cast<int>(nextRandom() % std::max(1, cols - half));
  const int startCol = static_cast<int>(nextRandom() % cols);
  ballX = mazeX + startCol * CELL + CELL / 2.0f;
  ballY = mazeY + CELL / 2.0f;
  velX = velY = 0;
  won = false;
  startMs = lastStepMs = millis();
  fullRedraw = true;
  drawnBallX = drawnBallY = -1;
  LOG_INF("MAZE", "new %dx%d maze, tilt %s", cols, rows, tilt.available() ? "on" : "off");
  requestUpdate();
}

bool TiltMazeActivity::inExit() const {
  for (const int cell : exits) {
    const float cx = mazeX + (cell % cols) * CELL + CELL / 2.0f;
    const float cy = mazeY + (cell / cols) * CELL + CELL / 2.0f;
    if (std::fabs(ballX - cx) < EXIT_REACH && std::fabs(ballY - cy) < EXIT_REACH) return true;
  }
  return false;
}

// Walls are thick lines along cell edges; every interior corner of a perfect
// maze touches at least one wall, so corners are treated as round posts.
void TiltMazeActivity::collide() {
  const int col = std::clamp(static_cast<int>((ballX - mazeX) / CELL), 0, cols - 1);
  const int row = std::clamp(static_cast<int>((ballY - mazeY) / CELL), 0, rows - 1);
  const float left = mazeX + col * CELL, top = mazeY + row * CELL;
  const float reach = BALL_RADIUS + HALF_WALL;
  if ((col == 0) && ballX < left + reach) ballX = left + reach, velX = std::fabs(velX) * BOUNCE;
  if (rightWall(col, row) && ballX > left + CELL - reach) ballX = left + CELL - reach, velX = -std::fabs(velX) * BOUNCE;
  if (col > 0 && rightWall(col - 1, row) && ballX < left + reach) ballX = left + reach, velX = std::fabs(velX) * BOUNCE;
  if ((row == 0) && ballY < top + reach) ballY = top + reach, velY = std::fabs(velY) * BOUNCE;
  if (bottomWall(col, row) && ballY > top + CELL - reach) ballY = top + CELL - reach, velY = -std::fabs(velY) * BOUNCE;
  if (row > 0 && bottomWall(col, row - 1) && ballY < top + reach) ballY = top + reach, velY = std::fabs(velY) * BOUNCE;
  for (int corner = 0; corner < 4; corner++) {
    const float px = left + (corner & 1 ? CELL : 0);
    const float py = top + (corner & 2 ? CELL : 0);
    const float dx = ballX - px, dy = ballY - py;
    const float dist2 = dx * dx + dy * dy;
    if (dist2 >= reach * reach || dist2 < 0.0001f) continue;
    const float dist = std::sqrt(dist2);
    const float nx = dx / dist, ny = dy / dist;
    ballX = px + nx * reach;
    ballY = py + ny * reach;
    const float into = velX * nx + velY * ny;
    if (into < 0) velX -= (1 + BOUNCE) * into * nx, velY -= (1 + BOUNCE) * into * ny;
  }
}

void TiltMazeActivity::step(const float dt, const float pushX, const float pushY) {
  velX = std::clamp((velX + pushX * GRAVITY_PX * dt) * FRICTION, -MAX_SPEED, MAX_SPEED);
  velY = std::clamp((velY + pushY * GRAVITY_PX * dt) * FRICTION, -MAX_SPEED, MAX_SPEED);
  ballX += velX * dt;
  ballY += velY * dt;
  collide();
}

void TiltMazeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  RenderLock lock(*this);
  if (won) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) newMaze();
    return;
  }

  float pushX = 0, pushY = 0;
  float tx = 0, ty = 0;
  if (tilt.available() && tilt.read(renderer, tx, ty)) {
    pushX = deadZone(tx);
    pushY = deadZone(ty);
  } else if (!tilt.available()) {
    using B = MappedInputManager::Button;
    const auto held = [this](const B button) { return mappedInput.isPressed(button) ? BUTTON_PUSH_G : 0.0f; };
    pushX = held(B::Right) - held(B::Left);
    pushY = held(B::Down) - held(B::Up);
  }

  const unsigned long now = millis();
  unsigned long elapsed = std::min(now - lastStepMs, MAX_CATCHUP_MS);
  lastStepMs = now;
  for (; elapsed >= STEP_MS; elapsed -= STEP_MS) step(STEP_MS / 1000.0f, pushX, pushY);
  lastStepMs -= elapsed;

  if (inExit()) {
    won = true;
    solvedMs = now - startMs;
    if (bestMs == 0 || solvedMs < bestMs) bestMs = solvedMs;
    fullRedraw = true;
    requestUpdate();
    return;
  }
  const bool ballMoved = static_cast<int>(ballX) != drawnBallX || static_cast<int>(ballY) != drawnBallY;
  const bool tick = static_cast<int>((now - startMs) / 1000) != drawnSeconds;
  if (ballMoved || tick) requestUpdate();
}

void TiltMazeActivity::drawHeader(const bool force) {
  const int seconds = static_cast<int>(((won ? solvedMs : millis() - startMs)) / 1000);
  if (!force && seconds == drawnSeconds) return;
  drawnSeconds = seconds;
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_HEIGHT - 2, false);
  renderer.drawText(UI_12_FONT_ID, 16, 6, tr(STR_PLAY_MAZE), true, EpdFontFamily::BOLD);
  play::drawFrameCost(renderer, 30 + renderer.getTextWidth(UI_12_FONT_ID, tr(STR_PLAY_MAZE), EpdFontFamily::BOLD),
                      12);
  char status[48];
  if (bestMs > 0) {
    snprintf(status, sizeof(status), "%d s  |  %s %lu s", seconds, tr(STR_PLAY_BEST), bestMs / 1000);
  } else {
    snprintf(status, sizeof(status), "%d s", seconds);
  }
  const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, status);
  renderer.drawText(UI_10_FONT_ID, width - statusWidth - 16, 9, status);
}

// Repaints walls, posts and exits intersecting the rectangle (after clearing it).
void TiltMazeActivity::drawMazeArea(const int x0, const int y0, const int x1, const int y1) const {
  renderer.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, false);
  const auto saved = renderer.getClipRect();
  renderer.setClipRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  const int c0 = std::max(0, (x0 - mazeX) / CELL - 1), c1 = std::min(cols - 1, (x1 - mazeX) / CELL + 1);
  const int r0 = std::max(0, (y0 - mazeY) / CELL - 1), r1 = std::min(rows - 1, (y1 - mazeY) / CELL + 1);
  for (const int cell : exits) {
    const int ex = mazeX + (cell % cols) * CELL, ey = mazeY + (cell / cols) * CELL;
    renderer.fillRectDither(ex + 8, ey + 8, CELL - 16, CELL - 16, Color::LightGray);
    renderer.drawRect(ex + 8, ey + 8, CELL - 16, CELL - 16, 2, true);
  }
  for (int r = r0; r <= r1; r++) {
    for (int c = c0; c <= c1; c++) {
      const int left = mazeX + c * CELL, top = mazeY + r * CELL;
      if (c == 0) renderer.fillRect(left - HALF_WALL, top - HALF_WALL, WALL, CELL + WALL, true);
      if (r == 0) renderer.fillRect(left - HALF_WALL, top - HALF_WALL, CELL + WALL, WALL, true);
      if (rightWall(c, r)) renderer.fillRect(left + CELL - HALF_WALL, top - HALF_WALL, WALL, CELL + WALL, true);
      if (bottomWall(c, r)) renderer.fillRect(left - HALF_WALL, top + CELL - HALF_WALL, CELL + WALL, WALL, true);
    }
  }
  renderer.setClipRect(saved[0], saved[1], saved[2], saved[3]);
}

void TiltMazeActivity::drawBall(const int x, const int y) const {
  renderer.fillRoundedRect(x - BALL_RADIUS, y - BALL_RADIUS, BALL_RADIUS * 2, BALL_RADIUS * 2, BALL_RADIUS,
                           Color::Black);
  renderer.fillRoundedRect(x - 7, y - 7, 5, 5, 2, Color::White);
}

void TiltMazeActivity::drawWin() const {
  const int width = renderer.getScreenWidth();
  char line[48];
  snprintf(line, sizeof(line), "%s %lu s", tr(STR_PLAY_SOLVED), solvedMs / 1000);
  char best[32];
  snprintf(best, sizeof(best), "%s %lu s", tr(STR_PLAY_BEST), bestMs / 1000);
  const int boxWidth = std::max(CELL * 6 + 8, renderer.getTextWidth(UI_12_FONT_ID, line, EpdFontFamily::BOLD) + 60);
  const int boxX = (width - boxWidth) / 2;
  const int boxY = mazeY + rows * CELL / 2 - 50;
  renderer.fillRect(boxX, boxY, boxWidth, 100, false);
  renderer.drawRect(boxX, boxY, boxWidth, 100, 3, true);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 20, line, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, boxY + 58, best);
}

void TiltMazeActivity::render(RenderLock&& lock) {
  const int bx = static_cast<int>(ballX);
  const int by = static_cast<int>(ballY);
  constexpr int PAD = BALL_RADIUS + 2;
  bool clean = false;
  if (fullRedraw) {
    renderer.clearScreen();
    drawHeader(true);
    drawMazeArea(0, HEADER_HEIGHT, renderer.getScreenWidth() - 1,
                 renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 1);
    const char* confirm = won ? tr(STR_PLAY_NEW_MAZE) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tilt.available() ? "" : tr(STR_DIR_LEFT),
                                              tilt.available() ? "" : tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    clean = true;
  } else {
    drawHeader(false);
    if (drawnBallX >= 0) drawMazeArea(drawnBallX - PAD, drawnBallY - PAD, drawnBallX + PAD, drawnBallY + PAD);
  }
  drawBall(bx, by);
  drawnBallX = bx;
  drawnBallY = by;
  if (won) drawWin();
  fullRedraw = false;
  const HalDisplay::RefreshMode mode = pacer.next(clean);
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
