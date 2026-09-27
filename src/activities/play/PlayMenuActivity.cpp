#include "PlayMenuActivity.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "BlackjackActivity.h"
#include "Game2048Activity.h"
#include "JokersActivity.h"
#include "JokersArt.h"
#include "MappedInputManager.h"
#include "MinesweeperActivity.h"
#include "PlayMenuGlyphs.h"
#include "SketchActivity.h"
#include "SnakeActivity.h"
#include "SokobanActivity.h"
#include "VoxelActivity.h"
#include "WordleActivity.h"
#include "activities/xkcd/XkcdActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

template <class T>
std::unique_ptr<Activity> make(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  return makeUniqueNoThrow<T>(renderer, mappedInput);
}

namespace glyphs = play::glyphs;

// Adapts the Jokers icon (no size argument) to the menu's glyph signature.
void jokersIcon(const GfxRenderer& renderer, const int x, const int y, int /*size*/) {
  jokers::art::drawMenuIcon(renderer, x, y);
}

// Menu order, grouped: arcade, puzzles, cards, creative. One row per game.
constexpr PlayMenuActivity::Entry GAMES[] = {
    {StrId::STR_PLAY_SNAKE, StrId::STR_PLAY_SNAKE_DESC, &glyphs::snake, &make<SnakeActivity>},
    {StrId::STR_PLAY_2048, StrId::STR_PLAY_2048_DESC, &glyphs::game2048, &make<Game2048Activity>},
    {StrId::STR_PLAY_MINES, StrId::STR_PLAY_MINES_DESC, &MinesweeperActivity::drawMenuIcon, &make<MinesweeperActivity>},
    {StrId::STR_PLAY_SOKOBAN, StrId::STR_PLAY_SOKOBAN_DESC, &glyphs::sokoban, &make<SokobanActivity>},
    {StrId::STR_PLAY_WORDLE, StrId::STR_PLAY_WORDLE_DESC, &glyphs::wordle, &make<WordleActivity>},
    {StrId::STR_PLAY_BLACKJACK, StrId::STR_PLAY_BLACKJACK_DESC, &glyphs::blackjack, &make<BlackjackActivity>},
    {StrId::STR_PLAY_JOKERS, StrId::STR_PLAY_JOKERS_DESC, &jokersIcon, &make<JokersActivity>},
    {StrId::STR_PLAY_VOXEL, StrId::STR_PLAY_VOXEL_DESC, &glyphs::voxel, &make<VoxelActivity>},
    {StrId::STR_PLAY_SKETCH, StrId::STR_PLAY_SKETCH_DESC, &glyphs::sketch, &make<SketchActivity>},
    {StrId::STR_PLAY_XKCD, StrId::STR_PLAY_XKCD_DESC, &XkcdActivity::drawMenuIcon, &make<XkcdActivity>},
};
constexpr int GAME_COUNT = sizeof(GAMES) / sizeof(GAMES[0]);

constexpr int SIDE = 28;
constexpr int TITLE_Y = 14;
constexpr int LIST_TOP = 56;
constexpr int CARD_HEIGHT = 116;
constexpr int CARD_GAP = 12;
constexpr int INDICATOR_HEIGHT = 28;
constexpr int GLYPH = 72;
constexpr int FOCUS_FRAME = 3;
constexpr int DOT_RADIUS = 5;
constexpr int DOT_PITCH = 18;
constexpr int ARROW_HALF = 7;

struct Layout {
  int listBottom;  // y just below the last card slot
  int perPage;
};

Layout layoutFor(const GfxRenderer& renderer) {
  const int bottom = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 12;
  const int available = bottom - INDICATOR_HEIGHT - LIST_TOP;
  const int perPage = std::max(1, (available + CARD_GAP) / (CARD_HEIGHT + CARD_GAP));
  return {bottom - INDICATOR_HEIGHT, perPage};
}

}  // namespace

void PlayMenuActivity::onEnter() {
  Activity::onEnter();
  lastRenderedPage = -1;
  requestUpdate();
}

void PlayMenuActivity::open() {
  const Entry& entry = GAMES[selected];
  std::unique_ptr<Activity> game = entry.create(renderer, mappedInput);
  if (!game) {
    LOG_ERR("PLAY", "OOM: game %d", selected);
    return;
  }
  // Back from a game: the menu redraws over the game's screen, so clean it.
  startActivityForResult(std::move(game), [this](const ActivityResult&) { lastRenderedPage = -1; });
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
  // requestUpdate() only flags a render; the render task drains all pending
  // requests at once, so presses made during a refresh collapse into one frame.
  const auto step = [this](const int direction) {
    selected = (selected + GAME_COUNT + direction) % GAME_COUNT;
    requestUpdate();
  };
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavPrevious}, [&step] { step(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::NavNext}, [&step] { step(+1); });
}

void PlayMenuActivity::renderCard(const Entry& entry, const bool focused, const int x, const int y, const int width,
                                  const int height) const {
  renderer.drawRoundedRect(x, y, width, height, focused ? FOCUS_FRAME : 1, 10, true);
  entry.glyph(renderer, x + 16, y + (height - GLYPH) / 2, GLYPH);
  const int textX = x + 16 + GLYPH + 20;
  const int textWidth = x + width - 16 - textX;
  renderer.drawText(UI_12_FONT_ID, textX, y + height / 2 - 30, I18N.get(entry.title), true,
                    focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, textX, y + height / 2 + 4,
                    renderer.truncatedText(UI_10_FONT_ID, I18N.get(entry.description), textWidth).c_str());
}

// Page dots with up/down arrows on the sides that have more games.
void PlayMenuActivity::renderPageIndicator(const int page, const int pageCount, const int centerX, const int y) const {
  const int cy = y + INDICATOR_HEIGHT / 2;
  const int dotsWidth = (pageCount - 1) * DOT_PITCH;
  const int firstX = centerX - dotsWidth / 2;
  for (int i = 0; i < pageCount; i++) {
    const int cx = firstX + i * DOT_PITCH;
    if (i == page) {
      renderer.fillRoundedRect(cx - DOT_RADIUS, cy - DOT_RADIUS, 2 * DOT_RADIUS, 2 * DOT_RADIUS, DOT_RADIUS,
                               Color::Black);
    } else {
      renderer.drawRoundedRect(cx - DOT_RADIUS, cy - DOT_RADIUS, 2 * DOT_RADIUS, 2 * DOT_RADIUS, 1, DOT_RADIUS, true);
    }
  }
  if (page > 0) {
    const int ax = firstX - DOT_PITCH - ARROW_HALF;
    const int xs[3] = {ax - ARROW_HALF, ax + ARROW_HALF, ax};
    const int ys[3] = {cy + ARROW_HALF / 2, cy + ARROW_HALF / 2, cy - ARROW_HALF / 2 - 2};
    renderer.fillPolygon(xs, ys, 3, true);
  }
  if (page < pageCount - 1) {
    const int ax = firstX + dotsWidth + DOT_PITCH + ARROW_HALF;
    const int xs[3] = {ax - ARROW_HALF, ax + ARROW_HALF, ax};
    const int ys[3] = {cy - ARROW_HALF / 2, cy - ARROW_HALF / 2, cy + ARROW_HALF / 2 + 2};
    renderer.fillPolygon(xs, ys, 3, true);
  }
}

void PlayMenuActivity::render(RenderLock&&) {
  const int current = selected;  // loop() may move it while this frame draws
  const Layout layout = layoutFor(renderer);
  const int pageCount = (GAME_COUNT + layout.perPage - 1) / layout.perPage;
  const int page = current / layout.perPage;
  const int width = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawText(UI_12_FONT_ID, SIDE, TITLE_Y, tr(STR_PLAY), true, EpdFontFamily::BOLD);
  if (pageCount > 1) {
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", page + 1, pageCount);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, width - SIDE - counterWidth, TITLE_Y + 4, counter);
  }

  const int first = page * layout.perPage;
  const int last = std::min(GAME_COUNT, first + layout.perPage);
  for (int i = first; i < last; i++) {
    const int y = LIST_TOP + (i - first) * (CARD_HEIGHT + CARD_GAP);
    renderCard(GAMES[i], i == current, SIDE, y, width - 2 * SIDE, CARD_HEIGHT);
  }
  if (pageCount > 1) renderPageIndicator(page, pageCount, width / 2, layout.listBottom);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Entering, returning from a game and flipping pages replace most of the
  // screen, so they get a HALF refresh; moving within a page is FAST.
  const bool pageChanged = page != lastRenderedPage;
  lastRenderedPage = page;
  renderer.displayBuffer(pageChanged ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
