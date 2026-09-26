#include "PlayMenuActivity.h"

#include <I18n.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "SnakeActivity.h"
#include "TiltMazeActivity.h"
#include "ZenGardenActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 28;
constexpr int TOP = 40;
constexpr int GAP = 16;
constexpr int GLYPH = 72;
constexpr int FOCUS_FRAME = 3;
}  // namespace

void PlayMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PlayMenuActivity::open() {
  std::unique_ptr<Activity> game;
  switch (selected) {
    case 0:
      game = makeUniqueNoThrow<ZenGardenActivity>(renderer, mappedInput);
      break;
    case 1:
      game = makeUniqueNoThrow<TiltMazeActivity>(renderer, mappedInput);
      break;
    default:
      game = makeUniqueNoThrow<SnakeActivity>(renderer, mappedInput);
      break;
  }
  if (!game) {
    LOG_ERR("PLAY", "OOM: game %d", selected);
    return;
  }
  startActivityForResult(std::move(game), [](const ActivityResult&) {});
}

void PlayMenuActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    open();
    return;
  }
  const auto step = [this](const int direction) {
    selected = (selected + ITEM_COUNT + direction) % ITEM_COUNT;
    requestUpdate();
  };
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavPrevious}, [&step] { step(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavNext}, [&step] { step(+1); });
}

// Tiny procedural previews: no bitmap assets.
void PlayMenuActivity::drawGlyph(const int index, const int x, const int y, const int size) const {
  const int c = size / 2;
  switch (index) {
    case 0: {
      for (int row = 6; row < size - 6; row += 6) renderer.drawLine(x + 4, y + row, x + size - 4, y + row, true);
      renderer.fillRect(x + c - 14, y + c - 14, 28, 28, false);
      renderer.fillRoundedRect(x + c - 11, y + c - 11, 22, 22, 11, Color::Black);
      break;
    }
    case 1: {
      // A fixed little maze.
      static constexpr int8_t WALLS[][4] = {{0, 0, 6, 0}, {0, 6, 6, 6}, {0, 0, 0, 5}, {6, 1, 6, 6}, {2, 0, 2, 4},
                                            {4, 2, 4, 6}, {2, 4, 3, 4}, {4, 2, 5, 2}};
      const int u = size / 6;
      for (const auto& w : WALLS) {
        renderer.fillRect(x + w[0] * u - 1, y + w[1] * u - 1, (w[2] - w[0]) * u + 3, (w[3] - w[1]) * u + 3, true);
      }
      renderer.fillRoundedRect(x + u / 2 - 5, y + u / 2 + u - 5, 10, 10, 5, Color::Black);
      break;
    }
    default: {
      constexpr int CELL = 12;
      static constexpr int8_t PATH[][2] = {{0, 4}, {1, 4}, {2, 4}, {2, 3}, {2, 2}, {3, 2}, {4, 2}, {4, 1}};
      for (const auto& p : PATH) renderer.fillRect(x + 6 + p[0] * CELL, y + 6 + p[1] * CELL, CELL - 2, CELL - 2, true);
      renderer.fillRect(x + 6 + 4 * CELL + 3, y + 6 + 4 * CELL + 3, CELL - 6, CELL - 6, true);
      break;
    }
  }
}

void PlayMenuActivity::renderItem(const int index, const int x, const int y, const int width,
                                  const int height) const {
  static constexpr StrId TITLES[ITEM_COUNT] = {StrId::STR_PLAY_ZEN, StrId::STR_PLAY_MAZE, StrId::STR_PLAY_SNAKE};
  static constexpr StrId DESCRIPTIONS[ITEM_COUNT] = {StrId::STR_PLAY_ZEN_DESC, StrId::STR_PLAY_MAZE_DESC,
                                                     StrId::STR_PLAY_SNAKE_DESC};
  const bool focused = index == selected;
  renderer.drawRoundedRect(x, y, width, height, focused ? FOCUS_FRAME : 1, 10, true);
  const int glyphY = y + (height - GLYPH) / 2;
  drawGlyph(index, x + 16, glyphY, GLYPH);
  const int textX = x + 16 + GLYPH + 20;
  renderer.drawText(UI_12_FONT_ID, textX, y + height / 2 - 30, I18N.get(TITLES[index]), true,
                    focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, textX, y + height / 2 + 4, I18N.get(DESCRIPTIONS[index]));
}

void PlayMenuActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - 16;
  renderer.clearScreen();
  renderer.drawText(UI_12_FONT_ID, SIDE, TOP - 26, tr(STR_PLAY), true, EpdFontFamily::BOLD);

  const int top = TOP + 16;
  const int itemHeight = std::min(140, (bottom - top - GAP * (ITEM_COUNT - 1)) / ITEM_COUNT);
  for (int i = 0; i < ITEM_COUNT; i++) renderItem(i, SIDE, top + i * (itemHeight + GAP), width - 2 * SIDE, itemHeight);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  firstRenderDone = true;
}
