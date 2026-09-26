#include "PlayMenuActivity.h"

#include <I18n.h>
#include <Memory.h>

#include "BlackjackActivity.h"
#include "Game2048Activity.h"
#include "Game2048Art.h"
#include "MappedInputManager.h"
#include "MinesweeperActivity.h"
#include "SketchActivity.h"
#include "SnakeActivity.h"
#include "SokobanActivity.h"
#include "SokobanArt.h"
#include "VoxelActivity.h"
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
    case 3:
      game = makeUniqueNoThrow<SokobanActivity>(renderer, mappedInput);
      break;
    case 4:
      game = makeUniqueNoThrow<Game2048Activity>(renderer, mappedInput);
      break;
    case 5:
      game = makeUniqueNoThrow<MinesweeperActivity>(renderer, mappedInput);
      break;
    case 6:
      game = makeUniqueNoThrow<VoxelActivity>(renderer, mappedInput);
      break;
    default:
      game = makeUniqueNoThrow<SketchActivity>(renderer, mappedInput);
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
    case 3: {
      // A walled corridor: keeper, crate, goal - drawn with the game's own tiles.
      static constexpr uint8_t ROW[3] = {
          sokoban::CELL_FLOOR | sokoban::CELL_PLAYER | (sokoban::RIGHT << sokoban::FACING_SHIFT),
          sokoban::CELL_FLOOR | sokoban::CELL_BOX, sokoban::CELL_FLOOR | sokoban::CELL_GOAL};
      constexpr int T = 24;
      for (int i = 0; i < 3; i++) {
        // Walls: open to the corridor and the card, closed between neighbours.
        const int ends = (i == 0 ? 1 << sokoban::LEFT : 0) | (i == 2 ? 1 << sokoban::RIGHT : 0);
        const uint8_t open = 1 << sokoban::UP | 1 << sokoban::DOWN | ends;
        sokoban::drawTile(renderer, x + i * T, y, T, sokoban::CELL_WALL, x, y, open);
        sokoban::drawTile(renderer, x + i * T, y + T, T, ROW[i], x, y);
        sokoban::drawTile(renderer, x + i * T, y + 2 * T, T, sokoban::CELL_WALL, x, y, open);
      }
      break;
    }
    case 4: {
      // A 2x2 of tiles climbing the texture ladder.
      constexpr int T = 32, G = 4;
      static constexpr uint8_t EXPS[4] = {1, 4, 7, 11};
      for (int i = 0; i < 4; i++) {
        g2048::drawTile(renderer, x + 2 + (i % 2) * (T + G), y + 2 + (i / 2) * (T + G), T, EXPS[i], false);
      }
      break;
    }
    case 5:
      MinesweeperActivity::drawMenuIcon(renderer, x, y, size);
      break;
    case 6: {
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
      break;
    }
    default: {
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
  }
}

void PlayMenuActivity::renderItem(const int index, const int x, const int y, const int width, const int height) const {
  static constexpr StrId TITLES[ITEM_COUNT] = {StrId::STR_PLAY_SNAKE, StrId::STR_PLAY_BLACKJACK, StrId::STR_PLAY_SKETCH,
                                               StrId::STR_PLAY_SOKOBAN, StrId::STR_PLAY_2048,
                                               StrId::STR_PLAY_MINES, StrId::STR_PLAY_VOXEL};
  static constexpr StrId DESCRIPTIONS[ITEM_COUNT] = {StrId::STR_PLAY_SNAKE_DESC, StrId::STR_PLAY_BLACKJACK_DESC,
                                                     StrId::STR_PLAY_SKETCH_DESC, StrId::STR_PLAY_SOKOBAN_DESC,
                                                     StrId::STR_PLAY_2048_DESC,
                                                     StrId::STR_PLAY_MINES_DESC, StrId::STR_PLAY_VOXEL_DESC};
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
