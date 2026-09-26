#include "PlayMenuActivity.h"

#include <I18n.h>
#include <Memory.h>

#include "BlackjackActivity.h"
#include "MappedInputManager.h"
#include "SketchActivity.h"
#include "SnakeActivity.h"
#include "WordleActivity.h"
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
      game = makeUniqueNoThrow<SnakeActivity>(renderer, mappedInput);
      break;
    case 1:
      game = makeUniqueNoThrow<BlackjackActivity>(renderer, mappedInput);
      break;
    case 2:
      game = makeUniqueNoThrow<SketchActivity>(renderer, mappedInput);
      break;
    default:
      game = makeUniqueNoThrow<WordleActivity>(renderer, mappedInput);
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
  switch (index) {
    case 0: {
      constexpr int CELL = 12;
      static constexpr int8_t PATH[][2] = {{0, 4}, {1, 4}, {2, 4}, {2, 3}, {2, 2}, {3, 2}, {4, 2}, {4, 1}};
      for (const auto& p : PATH) renderer.fillRect(x + 6 + p[0] * CELL, y + 6 + p[1] * CELL, CELL - 2, CELL - 2, true);
      renderer.fillRect(x + 6 + 4 * CELL + 3, y + 6 + 4 * CELL + 3, CELL - 6, CELL - 6, true);
      break;
    }
    case 1: {
      // Two fanned cards.
      renderer.fillRoundedRect(x + 10, y + 12, 30, 44, 4, Color::White);
      renderer.drawRoundedRect(x + 10, y + 12, 30, 44, 2, 4, true);
      renderer.fillRoundedRect(x + 30, y + 18, 30, 44, 4, Color::White);
      renderer.drawRoundedRect(x + 30, y + 18, 30, 44, 2, 4, true);
      renderer.drawText(SMALL_FONT_ID, x + 36, y + 22, "A", true, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, x + 14, y + 16, "K", true, EpdFontFamily::BOLD);
      break;
    }
    case 2: {
      // A quick pencil face.
      const int c = size / 2;
      renderer.drawRoundedRect(x + c - 22, y + 8, 44, 54, 2, 22, true);
      renderer.fillRect(x + c - 11, y + 28, 4, 4, true);
      renderer.fillRect(x + c + 7, y + 28, 4, 4, true);
      renderer.drawLine(x + c - 8, y + 46, x + c + 8, y + 46, 2, true);
      renderer.drawLine(x + c - 30, y + 10, x + c + 30, y + 4, 2, true);
      renderer.drawLine(x + c - 20, y + 70, x + c - 30, y + 72, 2, true);
      renderer.drawLine(x + c + 20, y + 70, x + c + 30, y + 72, 2, true);
      break;
    }
    default: {
      // Wordle: solved, misplaced and unused tiles.
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
      break;
    }
  }
}

void PlayMenuActivity::renderItem(const int index, const int x, const int y, const int width, const int height) const {
  static constexpr StrId TITLES[ITEM_COUNT] = {StrId::STR_PLAY_SNAKE, StrId::STR_PLAY_BLACKJACK, StrId::STR_PLAY_SKETCH,
                                               StrId::STR_PLAY_WORDLE};
  static constexpr StrId DESCRIPTIONS[ITEM_COUNT] = {StrId::STR_PLAY_SNAKE_DESC, StrId::STR_PLAY_BLACKJACK_DESC,
                                                     StrId::STR_PLAY_SKETCH_DESC, StrId::STR_PLAY_WORDLE_DESC};
  const bool focused = index == selected;
  renderer.drawRoundedRect(x, y, width, height, focused ? FOCUS_FRAME : 1, 10, true);
  const int glyphY = y + (height - GLYPH) / 2;
  drawGlyph(index, x + 16, glyphY, GLYPH);
  const int textX = x + 16 + GLYPH + 20;
  renderer.drawText(UI_12_FONT_ID, textX, y + height / 2 - 30, I18N.get(TITLES[index]), true,
                    focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  const int textWidth = x + width - 16 - textX;
  renderer.drawText(UI_10_FONT_ID, textX, y + height / 2 + 4,
                    renderer.truncatedText(UI_10_FONT_ID, I18N.get(DESCRIPTIONS[index]), textWidth).c_str());
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
