#include "BubbleWrapActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int PITCH = 66;  // bubble spacing
constexpr int RADIUS = 27;
constexpr int HEADER_HEIGHT = 40;
constexpr unsigned long BURST_MS = 220;
constexpr unsigned long NEW_SHEET_DELAY_MS = 900;
constexpr unsigned long NEW_SHEET_HOLD_MS = 700;
}  // namespace

void BubbleWrapActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight() - metrics.buttonHintsHeight - HEADER_HEIGHT;
  cols = std::max(1, (width - 16) / PITCH);
  rows = std::max(1, std::min((height - 8) / PITCH, MAX_BUBBLES / cols));
  // Alternate rows are offset by half a pitch, so leave room for that.
  gridX = (width - cols * PITCH - PITCH / 2) / 2 + PITCH / 2;
  gridY = HEADER_HEIGHT + (height - rows * PITCH) / 2 + PITCH / 2;
  newSheet();
}

void BubbleWrapActivity::newSheet() {
  memset(popped, 0, sizeof(popped));
  remaining = cols * rows;
  bursting = -1;
  sheetDoneMs = 0;
  cleanRequested = true;
  requestUpdate();
}

void BubbleWrapActivity::bubbleCenter(const int index, int& x, int& y) const {
  const int row = index / cols;
  x = gridX + (index % cols) * PITCH + (row % 2 ? PITCH / 2 : 0);
  y = gridY + row * PITCH;
}

void BubbleWrapActivity::pop() {
  if (isPopped(cursor)) return;
  popped[cursor >> 3] |= static_cast<uint8_t>(1 << (cursor & 7));
  remaining--;
  bursting = cursor;
  burstMs = millis();
  if (remaining == 0) sheetDoneMs = millis();
}

void BubbleWrapActivity::moveCursor(const int delta) {
  const int count = cols * rows;
  cursor = (cursor + delta + count) % count;
}

void BubbleWrapActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  RenderLock lock(*this);
  using B = MappedInputManager::Button;
  bool changed = false;
  if (mappedInput.wasLongPressed(B::Confirm, NEW_SHEET_HOLD_MS)) {
    newSheet();
    return;
  }
  if (mappedInput.wasPressed(B::Confirm)) {
    pop();
    changed = true;
  }
  if (mappedInput.wasPressed(B::Left)) moveCursor(-1), changed = true;
  if (mappedInput.wasPressed(B::Right)) moveCursor(+1), changed = true;
  if (mappedInput.wasPressed(B::Up)) moveCursor(-cols), changed = true;
  if (mappedInput.wasPressed(B::Down)) moveCursor(+cols), changed = true;

  const unsigned long now = millis();
  if (bursting >= 0 && now - burstMs >= BURST_MS) {
    bursting = -1;
    changed = true;
  }
  if (sheetDoneMs != 0 && now - sheetDoneMs >= NEW_SHEET_DELAY_MS) {
    newSheet();
    return;
  }
  if (changed) requestUpdate();
}

void BubbleWrapActivity::drawBubble(const int index) const {
  int x = 0, y = 0;
  bubbleCenter(index, x, y);
  if (index == bursting) {
    // Burst: radiating dashes where the bubble was.
    static constexpr int8_t RAYS[8][2] = {{1, 0}, {2, 2}, {0, 1}, {-2, 2}, {-1, 0}, {-2, -2}, {0, -1}, {2, -2}};
    for (const auto& r : RAYS) {
      const int scale = (r[0] != 0 && r[1] != 0) ? 7 : 10;
      renderer.drawLine(x + r[0] * scale, y + r[1] * scale, x + r[0] * scale * 5 / 2, y + r[1] * scale * 5 / 2, 3,
                        true);
    }
  } else if (isPopped(index)) {
    // Flattened plastic: a thin ring with a crinkle.
    renderer.drawRoundedRect(x - RADIUS + 4, y - RADIUS + 4, 2 * (RADIUS - 4), 2 * (RADIUS - 4), 1, RADIUS - 4, true);
    renderer.drawLine(x - 10, y - 3, x - 3, y + 4, true);
    renderer.drawLine(x - 3, y + 4, x + 4, y - 4, true);
    renderer.drawLine(x + 4, y - 4, x + 11, y + 3, true);
  } else {
    renderer.drawRoundedRect(x - RADIUS, y - RADIUS, 2 * RADIUS, 2 * RADIUS, 2, RADIUS, true);
    constexpr int INNER = RADIUS - 3;
    renderer.fillRoundedRect(x - INNER, y - INNER, 2 * INNER, 2 * INNER, INNER, Color::LightGray);
    renderer.fillRoundedRect(x - 15, y - 15, 9, 9, 4, Color::White);  // shine
  }
  if (index == cursor) {
    renderer.drawRoundedRect(x - RADIUS - 5, y - RADIUS - 5, 2 * RADIUS + 10, 2 * RADIUS + 10, 3, 10, true);
  }
}

void BubbleWrapActivity::render(RenderLock&& lock) {
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();
  renderer.drawText(UI_12_FONT_ID, 16, 8, tr(STR_PLAY_BUBBLES), true, EpdFontFamily::BOLD);
  char status[32];
  snprintf(status, sizeof(status), "%d %s", remaining, tr(STR_PLAY_LEFT));
  const int statusWidth = renderer.getTextWidth(UI_10_FONT_ID, status);
  renderer.drawText(UI_10_FONT_ID, width - statusWidth - 16, 11, status);
  play::drawFrameCost(renderer, 30 + renderer.getTextWidth(UI_12_FONT_ID, tr(STR_PLAY_BUBBLES), EpdFontFamily::BOLD),
                      14);

  for (int i = 0; i < cols * rows; i++) drawBubble(i);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_PLAY_POP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const HalDisplay::RefreshMode mode = pacer.next(cleanRequested);
  cleanRequested = false;
  lock.unlock();
  play::FramePacer::present(renderer, mode);
}
