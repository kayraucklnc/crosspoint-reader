// Hint, off-path rewind and solution step-through for SokobanActivity.
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SokobanActivity.h"
#include "SokobanArt.h"
#include "components/UITheme.h"
#include "fontIds.h"

using sokoban::Board;
using sokoban::Dir;

bool SokobanActivity::ensureGuide() {
  if (!guideTried) {
    guideTried = true;
    if (!guide.build(pack, level, transposed)) LOG_ERR("SOKO", "No solution guide for level %d", level);
  }
  return guide.ready();
}

bool SokobanActivity::locateHint() {
  hintShown = ensureGuide() && guide.find(*board, hint, route);
  return hintShown;
}

void SokobanActivity::clearHint() {
  if (!hintShown) return;
  hintShown = false;
  requestUpdate();
}

// Shows the next push, or asks to rewind first when the position has left
// the solution. With `stepThrough`, continues into the solution walkthrough.
void SokobanActivity::requestHint(const bool stepThrough) {
  if (board->solved() || !ensureGuide()) {
    mode = Mode::PLAY;
    fullRedraw = true;
    requestUpdate();
    return;
  }
  fullRedraw = mode != Mode::PLAY;  // lift the menu card
  if (locateHint()) {
    hintsUsed++;
    mode = stepThrough ? Mode::SOLUTION : Mode::PLAY;
  } else {
    solutionAfterRewind = stepThrough;
    mode = Mode::OFF_PATH;
  }
  requestUpdate();
}

// Undoes to the most recent position on the solution path; if the undo
// history runs out first, the level starts over (its start is always on it).
void SokobanActivity::rewindToPath() {
  while (!locateHint()) {
    if (board->undo() == Board::Step::BLOCKED) {
      uint16_t par = 0, parPushes = 0;
      if (!pack.load(level, *board, par, parPushes)) return;
      if (transposed) board->transpose();
      locateHint();
      break;
    }
  }
  stuck = board->stuck();
  movesSinceClean += 8;  // a long rewind leaves more ghosting than one step
}

void SokobanActivity::solutionStep(const int direction) {
  if (direction > 0) {
    if (!hintShown && !locateHint()) rewindToPath();
    if (!hintShown) return;
    // The route is a shortest path, so each step has exactly one marked neighbour.
    for (bool walked = true; walked;) {
      walked = false;
      for (int d = 0; d < 4; d++) {
        const int n = board->neighbour(board->playerCell(), static_cast<Dir>(d));
        if (n < 0 || !(route[n / 8] & (1 << (n % 8)))) continue;
        route[n / 8] &= static_cast<uint8_t>(~(1 << (n % 8)));
        board->move(static_cast<Dir>(d));
        walked = true;
        break;
      }
    }
    board->move(hint.dir);
  } else {
    // Back over the previous push and the walk that led to it.
    Board::Step undone;
    do {
      undone = board->undo();
    } while (undone == Board::Step::WALKED);
    while (board->canUndo() && !board->lastWasPush()) board->undo();
  }
  movesSinceClean += 2;
  stuck = board->stuck();
  if (board->solved()) {
    hintShown = false;
    completeLevel();
  } else {
    locateHint();
  }
  requestUpdate();
}

uint8_t SokobanActivity::tileLook(const int cell) const {
  const uint8_t look = board->visual(cell);
  if (!hintShown) return look;
  if (cell == hint.box) return static_cast<uint8_t>(look | sokoban::TILE_HINT | (hint.dir << sokoban::FACING_SHIFT));
  if (route[cell / 8] & (1 << (cell % 8))) return static_cast<uint8_t>(look | sokoban::TILE_HINT);
  return look;
}

void SokobanActivity::handleOffPath() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    mode = Mode::PLAY;
    fullRedraw = true;
    requestUpdate();
  } else if (mappedInput.wasReleased(B::Confirm)) {
    rewindToPath();
    hintsUsed++;
    mode = solutionAfterRewind ? Mode::SOLUTION : Mode::PLAY;
    fullRedraw = true;
    requestUpdate();
  }
}

void SokobanActivity::handleSolution() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) {
    mode = Mode::PLAY;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(B::Right) || mappedInput.wasPressed(B::Down)) {
    solutionStep(+1);
  } else if (mappedInput.wasPressed(B::Left) || mappedInput.wasPressed(B::Up)) {
    solutionStep(-1);
  }
}

void SokobanActivity::drawOffPathCard() const {
  const int width = renderer.getScreenWidth();
  const int cardW = std::min(420, width - 40);
  constexpr int CARD_H = 150;
  const int x = (width - cardW) / 2;
  const int boardMid = boardY + board->height() * tile / 2;
  const int y = std::clamp(boardMid - CARD_H / 2, 80, renderer.getScreenHeight() - CARD_H - 60);
  renderer.fillRoundedRect(x, y, cardW, CARD_H, 12, Color::White);
  renderer.drawRoundedRect(x, y, cardW, CARD_H, 3, 12, true);
  renderer.drawRoundedRect(x + 6, y + 6, cardW - 12, CARD_H - 12, 1, 8, true);
  sokoban::drawBulb(renderer, x + 40, y + 40, 28, true);
  renderer.drawText(UI_12_FONT_ID, x + 64, y + 24, tr(STR_SOKO_OFF_PATH), true, EpdFontFamily::BOLD);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_SOKO_OFF_PATH_BODY), cardW - 60, 2);
  int ty = y + 70;
  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, x + 30, ty, line.c_str());
    ty += 26;
  }
}
