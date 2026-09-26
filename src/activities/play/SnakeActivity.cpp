#include "SnakeActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int CELL = 24;
constexpr int HEADER_HEIGHT = 40;
constexpr int MARGIN = 16;
constexpr int START_LENGTH = 4;
constexpr int SLOWEST_STEP_MS = 520;
constexpr int FASTEST_STEP_MS = 320;  // about one X3 fast refresh; faster would skip frames
constexpr int SPEEDUP_PER_FOOD_MS = 12;
constexpr float TILT_STEER_G = 0.25f;
constexpr int DX[4] = {1, 0, -1, 0};
constexpr int DY[4] = {0, 1, 0, -1};

int bestScore = 0;  // survives until reboot
}  // namespace

void SnakeActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight() - metrics.buttonHintsHeight - HEADER_HEIGHT - MARGIN;
  cols = (width - 2 * MARGIN) / CELL;
  rows = height / CELL;
  boardX = (width - cols * CELL) / 2;
  boardY = HEADER_HEIGHT + (height - rows * CELL) / 2;
  capacity = cols * rows;
  // ~1.2 KB on the X3: one slot per board cell so the snake can never overflow.
  body = makeUniqueNoThrow<uint16_t[]>(capacity);
  if (!body) {
    LOG_ERR("SNAKE", "OOM: body %d cells", capacity);
    finish();
    return;
  }
  rng = static_cast<uint32_t>(millis()) | 1u;
  tilt.begin();
  reset();
}

void SnakeActivity::onExit() {
  tilt.end();
  body.reset();
  Activity::onExit();
}

void SnakeActivity::reset() {
  length = START_LENGTH;
  dir = pendingDir = 0;
  const int startRow = rows / 2;
  const int startCol = cols / 4;
  for (int i = 0; i < length; i++) body[i] = static_cast<uint16_t>(startRow * cols + startCol + i);
  head = length - 1;
  score = 0;
  state = State::RUNNING;
  placeFood();
  lastStepMs = millis();
  cleanRequested = true;
  requestUpdate();
}

bool SnakeActivity::occupied(const int cell) const {
  for (int i = 0; i < length; i++) {
    if (body[(head - i + capacity) % capacity] == cell) return true;
  }
  return false;
}

void SnakeActivity::placeFood() {
  if (length >= capacity) return;
  do {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    food = static_cast<int>(rng % static_cast<uint32_t>(capacity));
  } while (occupied(food));
}

int SnakeActivity::stepInterval() const {
  return std::max(FASTEST_STEP_MS, SLOWEST_STEP_MS - score * SPEEDUP_PER_FOOD_MS);
}

void SnakeActivity::steerByTilt() {
  float tx = 0, ty = 0;
  if (!tilt.read(renderer, tx, ty)) return;
  int wanted = -1;
  if (std::fabs(tx) >= std::fabs(ty) && std::fabs(tx) > TILT_STEER_G) wanted = tx > 0 ? 0 : 2;
  if (std::fabs(ty) > std::fabs(tx) && std::fabs(ty) > TILT_STEER_G) wanted = ty > 0 ? 1 : 3;
  if (wanted >= 0 && wanted != (dir + 2) % 4) pendingDir = wanted;
}

void SnakeActivity::advance() {
  dir = pendingDir;
  const int cell = body[head];
  const int col = cell % cols + DX[dir];
  const int row = cell / cols + DY[dir];
  const int next = row * cols + col;
  // The tail cell frees up this step unless the snake is eating.
  const bool eating = next == food;
  const int tail = body[(head - length + 1 + capacity) % capacity];
  if (col < 0 || col >= cols || row < 0 || row >= rows || (occupied(next) && (eating || next != tail))) {
    state = State::OVER;
    bestScore = std::max(bestScore, score);
    return;
  }
  head = (head + 1) % capacity;
  body[head] = static_cast<uint16_t>(next);
  if (eating) {
    length++;
    score++;
    placeFood();
  }
}

void SnakeActivity::loop() {
  if (!body) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  RenderLock lock(*this);
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == State::OVER) {
      reset();
      return;
    }
    state = state == State::PAUSED ? State::RUNNING : State::PAUSED;
    lastStepMs = millis();
    requestUpdate();
  }
  if (state != State::RUNNING) return;

  if (tilt.available() && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    tiltSteer = !tiltSteer;
    requestUpdate();
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) pendingDir = (dir + 3) % 4;
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) pendingDir = (dir + 1) % 4;
  if (tiltSteer) steerByTilt();

  if (millis() - lastStepMs >= static_cast<unsigned long>(stepInterval())) {
    lastStepMs = millis();
    advance();
    requestUpdate();
  }
}

void SnakeActivity::drawCell(const int cell, const bool isHead) const {
  const int x = boardX + (cell % cols) * CELL;
  const int y = boardY + (cell / cols) * CELL;
  renderer.fillRect(x + 2, y + 2, CELL - 4, CELL - 4, true);
  if (!isHead) return;
  // Eyes face the heading.
  const int cx = x + CELL / 2 + DX[dir] * 4;
  const int cy = y + CELL / 2 + DY[dir] * 4;
  const int ox = DY[dir] * 5, oy = DX[dir] * 5;
  renderer.fillRect(cx + ox - 2, cy + oy - 2, 4, 4, false);
  renderer.fillRect(cx - ox - 2, cy - oy - 2, 4, 4, false);
}

void SnakeActivity::render(RenderLock&& lock) {
  if (!body) return;
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();

  renderer.drawText(UI_12_FONT_ID, MARGIN, 8, tr(STR_PLAY_SNAKE), true, EpdFontFamily::BOLD);
  char status[64];
  snprintf(status, sizeof(status), "%s %d  |  %s %d%s%s", tr(STR_PLAY_SCORE), score, tr(STR_PLAY_BEST),
           std::max(bestScore, score), tiltSteer ? "  |  " : "", tiltSteer ? tr(STR_PLAY_TILT) : "");
  const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, status);
  renderer.drawText(UI_10_FONT_ID, width - statusWidth - MARGIN, 11, status);
  play::drawFrameCost(renderer,
                      MARGIN + 14 + renderer.getTextWidth(UI_12_FONT_ID, tr(STR_PLAY_SNAKE), EpdFontFamily::BOLD), 14);

  renderer.drawRect(boardX - 3, boardY - 3, cols * CELL + 6, rows * CELL + 6, 2, true);
  const int fx = boardX + (food % cols) * CELL + CELL / 2;
  const int fy = boardY + (food / cols) * CELL + CELL / 2;
  renderer.drawRoundedRect(fx - 8, fy - 8, 16, 16, 2, 8, true);
  for (int i = 0; i < length; i++) drawCell(body[(head - i + capacity) % capacity], i == 0);

  if (state != State::RUNNING) {
    const char* message = state == State::OVER ? tr(STR_PLAY_GAME_OVER) : tr(STR_PLAY_PAUSED);
    const int boxWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD) + 48;
    const int boxX = (width - boxWidth) / 2;
    const int boxY = boardY + rows * CELL / 2 - 26;
    renderer.fillRect(boxX, boxY, boxWidth, 52, false);
    renderer.drawRect(boxX, boxY, boxWidth, 52, 2, true);
    renderer.drawCenteredText(UI_12_FONT_ID, boxY + 12, message, true, EpdFontFamily::BOLD);
  }

  const char* confirm = state == State::OVER ? tr(STR_PLAY_AGAIN) : tr(STR_PLAY_PAUSE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const HalDisplay::RefreshMode mode = pacer.next(cleanRequested);
  cleanRequested = false;
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
