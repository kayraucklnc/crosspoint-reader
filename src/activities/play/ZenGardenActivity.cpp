#include "ZenGardenActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int HEADER_HEIGHT = 36;
constexpr int BALL_RADIUS = 10;
constexpr int BRUSH_RADIUS_CELLS = 3;
constexpr unsigned long STEP_MS = 20;
constexpr unsigned long MAX_CATCHUP_MS = 200;
constexpr float GRAVITY_PX = 1400.0f;  // px/s^2 per g of tilt
constexpr float BUTTON_PUSH_G = 0.5f;
constexpr float DEAD_ZONE_G = 0.04f;
constexpr float FRICTION = 0.985f;  // per step
constexpr float BOUNCE = 0.5f;
constexpr float MAX_SPEED = 700.0f;
constexpr float STAMP_MIN_MOVE = 0.6f;
constexpr unsigned long CLEAR_HOLD_MS = 700;

float deadZone(const float g) { return std::fabs(g) < DEAD_ZONE_G ? 0.0f : g; }

// Pixel texture per cell value, keyed on absolute coordinates so cells tile seamlessly.
bool sandInk(const uint8_t value, const int x, const int y) {
  switch (value) {
    case 1: {
      // Hashed stipple reads as loose grain rather than hatching.
      uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
      h = (h ^ (h >> 13)) * 1274126177u;
      return (h >> 29) < 2;
    }
    case 2:
      return y % 4 == 0 || (x * 5 + y * 3) % 11 == 0;
    case 3:
      return x % 4 == 0 || (x * 3 + y * 5) % 11 == 0;
    default:
      return false;
  }
}
}  // namespace

void ZenGardenActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  fieldWidth = renderer.getScreenWidth();
  fieldTop = HEADER_HEIGHT;
  fieldBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  cols = fieldWidth / CELL;
  rows = (fieldBottom - fieldTop) / CELL;
  // ~6.5 KB on the X3; allocated once here instead of per frame.
  const size_t bytes = (static_cast<size_t>(cols) * rows + 3) / 4;
  sand = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!sand) {
    LOG_ERR("ZEN", "OOM: sand %u bytes", static_cast<unsigned>(bytes));
    finish();
    return;
  }
  memset(sand.get(), 0, bytes);
  ballX = fieldWidth / 2.0f;
  ballY = (fieldTop + fieldBottom) / 2.0f;
  tilt.begin();
  LOG_INF("ZEN", "tilt %s, %dx%d cells", tilt.available() ? "on" : "off (buttons)", cols, rows);
  lastStepMs = millis();
  requestUpdate();
}

void ZenGardenActivity::onExit() {
  tilt.end();
  sand.reset();
  Activity::onExit();
}

uint8_t ZenGardenActivity::cellAt(const int col, const int row) const {
  const int index = row * cols + col;
  return (sand[index >> 2] >> ((index & 3) * 2)) & 3;
}

void ZenGardenActivity::setCell(const int col, const int row, const uint8_t value) {
  const int index = row * cols + col;
  const int shift = (index & 3) * 2;
  sand[index >> 2] = static_cast<uint8_t>((sand[index >> 2] & ~(3 << shift)) | (value << shift));
}

void ZenGardenActivity::markDirty(const int x0, const int y0, const int x1, const int y1) {
  if (!hasDirty) {
    dirtyX0 = x0, dirtyY0 = y0, dirtyX1 = x1, dirtyY1 = y1;
    hasDirty = true;
    return;
  }
  dirtyX0 = std::min(dirtyX0, x0);
  dirtyY0 = std::min(dirtyY0, y0);
  dirtyX1 = std::max(dirtyX1, x1);
  dirtyY1 = std::max(dirtyY1, y1);
}

void ZenGardenActivity::stamp() {
  uint8_t value = 0;
  if (brush == SAND) value = 1;
  if (brush == RAKE) value = std::fabs(velX) >= std::fabs(velY) ? 2 : 3;
  const int cc = static_cast<int>(ballX) / CELL;
  const int cr = (static_cast<int>(ballY) - fieldTop) / CELL;
  for (int r = cr - BRUSH_RADIUS_CELLS; r <= cr + BRUSH_RADIUS_CELLS; r++) {
    for (int c = cc - BRUSH_RADIUS_CELLS; c <= cc + BRUSH_RADIUS_CELLS; c++) {
      if (r < 0 || r >= rows || c < 0 || c >= cols) continue;
      const int dc = c - cc, dr = r - cr;
      if (dc * dc + dr * dr > BRUSH_RADIUS_CELLS * BRUSH_RADIUS_CELLS) continue;
      setCell(c, r, value);
    }
  }
  const int reach = (BRUSH_RADIUS_CELLS + 1) * CELL;
  markDirty(static_cast<int>(ballX) - reach, static_cast<int>(ballY) - reach, static_cast<int>(ballX) + reach,
            static_cast<int>(ballY) + reach);
}

void ZenGardenActivity::step(const float dt, const float pushX, const float pushY) {
  velX = std::clamp((velX + pushX * GRAVITY_PX * dt) * FRICTION, -MAX_SPEED, MAX_SPEED);
  velY = std::clamp((velY + pushY * GRAVITY_PX * dt) * FRICTION, -MAX_SPEED, MAX_SPEED);
  const float oldX = ballX, oldY = ballY;
  ballX += velX * dt;
  ballY += velY * dt;
  const float minX = BALL_RADIUS, maxX = fieldWidth - BALL_RADIUS - 1;
  const float minY = fieldTop + BALL_RADIUS, maxY = fieldBottom - BALL_RADIUS - 1;
  if (ballX < minX || ballX > maxX) {
    ballX = std::clamp(ballX, minX, maxX);
    velX = -velX * BOUNCE;
  }
  if (ballY < minY || ballY > maxY) {
    ballY = std::clamp(ballY, minY, maxY);
    velY = -velY * BOUNCE;
  }
  if (std::fabs(ballX - oldX) + std::fabs(ballY - oldY) >= STAMP_MIN_MOVE) stamp();
}

void ZenGardenActivity::clearGarden() {
  memset(sand.get(), 0, (static_cast<size_t>(cols) * rows + 3) / 4);
  velX = velY = 0;
  fullRedraw = true;
  cleanRequested = true;
}

void ZenGardenActivity::loop() {
  if (!sand) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  RenderLock lock(*this);
  bool changed = false;
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, CLEAR_HOLD_MS)) {
    clearGarden();
    changed = true;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    brush = static_cast<Brush>((brush + 1) % BRUSH_COUNT);
    changed = true;
  }

  float pushX = 0, pushY = 0;
  if (tilt.available()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      brush = static_cast<Brush>((brush + BRUSH_COUNT - 1) % BRUSH_COUNT);
      changed = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      brush = static_cast<Brush>((brush + 1) % BRUSH_COUNT);
      changed = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      cleanRequested = changed = true;
    }
    float tx = 0, ty = 0;
    if (tilt.read(renderer, tx, ty)) {
      pushX = deadZone(tx);
      pushY = deadZone(ty);
    }
  } else {
    // Held buttons stand in for tilt (and are the simulator's tilt control).
    using B = MappedInputManager::Button;
    const auto held = [this](const B button) { return mappedInput.isPressed(button) ? BUTTON_PUSH_G : 0.0f; };
    pushX = held(B::Right) - held(B::Left);
    pushY = held(B::Down) - held(B::Up);
  }

  const unsigned long now = millis();
  unsigned long elapsed = std::min(now - lastStepMs, MAX_CATCHUP_MS);
  lastStepMs = now;
  for (; elapsed >= STEP_MS; elapsed -= STEP_MS) step(STEP_MS / 1000.0f, pushX, pushY);
  lastStepMs -= elapsed;  // carry the remainder into the next pass
  const bool ballMoved = static_cast<int>(ballX) != drawnBallX || static_cast<int>(ballY) != drawnBallY;
  if (changed || ballMoved || hasDirty) requestUpdate();
}

void ZenGardenActivity::drawHeader() const {
  static constexpr StrId BRUSH_NAMES[BRUSH_COUNT] = {StrId::STR_PLAY_SAND, StrId::STR_PLAY_RAKE,
                                                     StrId::STR_PLAY_SMOOTH};
  renderer.fillRect(0, 0, fieldWidth, fieldTop, false);
  renderer.drawText(UI_12_FONT_ID, 16, 6, tr(STR_PLAY_ZEN), true, EpdFontFamily::BOLD);
  play::drawFrameCost(renderer, 30 + renderer.getTextWidth(UI_12_FONT_ID, tr(STR_PLAY_ZEN), EpdFontFamily::BOLD), 12);
  char status[64];
  snprintf(status, sizeof(status), "%s: %s  |  %s", tr(STR_PLAY_BRUSH), I18N.get(BRUSH_NAMES[brush]),
           tilt.available() ? tr(STR_PLAY_TILT) : tr(STR_PLAY_BUTTONS));
  const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, status);
  renderer.drawText(UI_10_FONT_ID, fieldWidth - statusWidth - 16, 9, status);
  renderer.drawLine(0, fieldTop - 1, fieldWidth - 1, fieldTop - 1, true);
}

void ZenGardenActivity::drawSand(int x0, int y0, int x1, int y1) const {
  x0 = std::max(x0, 0);
  y0 = std::max(y0, fieldTop);
  x1 = std::min(x1, cols * CELL - 1);
  y1 = std::min(y1, fieldTop + rows * CELL - 1);
  if (x0 > x1 || y0 > y1) return;
  renderer.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, false);
  for (int y = y0; y <= y1; y++) {
    const int row = (y - fieldTop) / CELL;
    for (int x = x0; x <= x1; x++) {
      const uint8_t value = cellAt(x / CELL, row);
      if (value != 0 && sandInk(value, x, y)) renderer.drawPixel(x, y, true);
    }
  }
}

void ZenGardenActivity::drawBall(const int x, const int y) const {
  renderer.fillRoundedRect(x - BALL_RADIUS, y - BALL_RADIUS, BALL_RADIUS * 2, BALL_RADIUS * 2, BALL_RADIUS,
                           Color::Black);
  renderer.fillRoundedRect(x - 6, y - 6, 5, 5, 2, Color::White);
}

void ZenGardenActivity::render(RenderLock&& lock) {
  if (!sand) return;
  const int bx = static_cast<int>(ballX);
  const int by = static_cast<int>(ballY);
  constexpr int PAD = BALL_RADIUS + 2;
  if (fullRedraw) {
    renderer.clearScreen();
    drawHeader();
    drawSand(0, fieldTop, fieldWidth - 1, fieldBottom - 1);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_PLAY_BRUSH),
                                              tilt.available() ? tr(STR_PLAY_REFRESH) : tr(STR_DIR_LEFT),
                                              tilt.available() ? "" : tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    drawHeader();  // cheap, and keeps the frame-cost readout current
    if (drawnBallX >= 0) markDirty(drawnBallX - PAD, drawnBallY - PAD, drawnBallX + PAD, drawnBallY + PAD);
    if (hasDirty) drawSand(dirtyX0, dirtyY0, dirtyX1, dirtyY1);
  }
  drawBall(bx, by);
  drawnBallX = bx;
  drawnBallY = by;
  fullRedraw = hasDirty = false;
  const HalDisplay::RefreshMode mode = pacer.next(cleanRequested);
  cleanRequested = false;
  // Let the loop keep stepping physics while the panel refreshes.
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
