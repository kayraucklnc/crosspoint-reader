#include "Game2048Activity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Game2048Art.h"
#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"

using g2048::Board;
using g2048::CELLS;
using g2048::Dir;

namespace {
constexpr const char* SAVE_DIR = "/.crosspoint/games";
constexpr const char* SAVE_PATH = "/.crosspoint/games/2048.json";
constexpr unsigned long SAVE_DELAY_MS = 2000;
constexpr int MOVES_PER_CLEAN = 25;
constexpr int GAP = 10;
constexpr int BOARD_RADIUS = 16;
constexpr int BOARD_LINE = 3;
constexpr int SIDE_MARGIN = 12;
constexpr int HEADER_H = 146;
constexpr int SIDE_HINT_W = 34;  // keeps the ladder clear of the X3 edge-button hints
constexpr int LADDER_TILE = 35;
constexpr int LADDER_GAP = 6;
constexpr int LADDER_MIN_H = 60;  // smallest band the ladder needs between header and board

// One base-36 digit per cell exponent; the scan set caps exponents at MAX_EXP ('h').
constexpr const char* SAVE_SCAN =
    "{\"board\":\"%16[0-9a-h]\",\"score\":%lu,\"best\":%lu,\"moves\":%lu,\"won\":%d,\"undo\":\"%16[0-9a-h]\","
    "\"undoScore\":%lu,\"undoMoves\":%lu";
constexpr int SAVE_BYTES = 192;

void encodeBoard(const Board& board, char* out) {
  for (int i = 0; i < CELLS; i++) {
    const uint8_t exp = board.cells[i];
    out[i] = static_cast<char>(exp < 10 ? '0' + exp : 'a' + exp - 10);
  }
  out[CELLS] = '\0';
}

bool decodeBoard(const char* text, Board& out) {
  if (strlen(text) != CELLS) return false;
  for (int i = 0; i < CELLS; i++) {
    out.cells[i] = static_cast<uint8_t>(text[i] <= '9' ? text[i] - '0' : text[i] - 'a' + 10);
  }
  return true;
}
}  // namespace

// ---- lifecycle ------------------------------------------------------------------

void Game2048Activity::onEnter() {
  Activity::onEnter();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int hintsTop = height - UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int room = std::min(width - 2 * SIDE_MARGIN, hintsTop - 6 - HEADER_H - LADDER_MIN_H);
  tile = (room - (g2048::SIDE + 1) * GAP) / g2048::SIDE;
  boardSize = tile * g2048::SIDE + (g2048::SIDE + 1) * GAP;
  boardX = (width - boardSize) / 2;
  boardY = hintsTop - 6 - boardSize;
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  load();
  requestUpdate();
}

void Game2048Activity::onExit() {
  save();
  Activity::onExit();
}

// ---- rules ------------------------------------------------------------------------

void Game2048Activity::markChanged() {
  saveDirty = true;
  lastChangeMs = millis();
  requestUpdate();
}

void Game2048Activity::setMode(const Mode next) {
  if (mode == next) return;
  if (mode == Mode::WON) cleanRequested = true;  // scrub the black card's ghost
  mode = next;
  fullRedraw = true;
  requestUpdate();
}

void Game2048Activity::newGame() {
  board = Board{};
  newCell = -1;
  mergedCells = 0;
  g2048::spawn(board, g2048::nextRandom(rng));
  g2048::spawn(board, g2048::nextRandom(rng));
  score = moves = lastGain = 0;
  hasUndo = false;
  won = false;
  cleanRequested = true;
  setMode(Mode::PLAY);
  markChanged();
  save();
}

void Game2048Activity::move(const Dir dir) {
  const auto result = g2048::slide(board, dir);
  if (!result.moved) return;
  undoBoard = board;
  undoScore = score;
  undoMoves = moves;
  hasUndo = true;
  board = result.board;
  newCell = static_cast<int8_t>(g2048::spawn(board, g2048::nextRandom(rng)));
  mergedCells = result.merged;
  score += result.gained;
  best = std::max(best, score);
  // Moves coalesced into one frame show their combined gain.
  if (gainShown) lastGain = 0;
  gainShown = false;
  lastGain += result.gained;
  moves++;
  movesSinceClean++;
  markChanged();
  if (!won && g2048::maxExp(board) >= g2048::WIN_EXP) {
    won = true;
    setMode(Mode::WON);
  } else if (!g2048::canMove(board)) {
    setMode(Mode::OVER);
    save();
  }
}

void Game2048Activity::undo() {
  if (!hasUndo) return;
  board = undoBoard;
  score = undoScore;
  moves = undoMoves;
  hasUndo = false;
  newCell = -1;
  mergedCells = 0;
  lastGain = 0;
  setMode(Mode::PLAY);
  markChanged();
}

void Game2048Activity::openMenu() {
  menuReturn = mode == Mode::OVER ? Mode::OVER : Mode::PLAY;
  menuIndex = mode == Mode::OVER ? 1 : 0;
  setMode(Mode::MENU);
}

void Game2048Activity::loopMenu() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    setMode(menuReturn);
    return;
  }
  const bool prev = mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Left);
  const bool next = mappedInput.wasPressed(B::Down) || mappedInput.wasPressed(B::Right);
  if (prev || next) {
    menuIndex = static_cast<uint8_t>((menuIndex + (next ? 1 : MENU_ITEMS - 1)) % MENU_ITEMS);
    requestUpdate();
  }
  if (!mappedInput.wasReleased(B::Confirm)) return;
  switch (menuIndex) {
    case 0:
      setMode(menuReturn);
      break;
    case 1:
      newGame();
      break;
    default:
      save();
      finish();
      break;
  }
}

void Game2048Activity::loop() {
  using B = MappedInputManager::Button;
  RenderLock lock(*this);
  if (saveDirty && millis() - lastChangeMs >= SAVE_DELAY_MS) save();

  switch (mode) {
    case Mode::MENU:
      loopMenu();
      return;
    case Mode::WON: {
      if (mappedInput.wasReleased(B::Back)) {
        openMenu();
        return;
      }
      const bool slide = mappedInput.wasPressed(B::Left) || mappedInput.wasPressed(B::Right) ||
                         mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down);
      if (mappedInput.wasReleased(B::Confirm) || slide) setMode(g2048::canMove(board) ? Mode::PLAY : Mode::OVER);
      if (!slide || mode != Mode::PLAY) return;
      break;  // a slide dismisses the card and is played at once
    }
    case Mode::OVER:
      if (mappedInput.wasReleased(B::Confirm)) undo();
      if (mappedInput.wasReleased(B::Back)) openMenu();
      return;
    case Mode::PLAY:
      break;
  }
  if (mappedInput.wasReleased(B::Back)) {
    openMenu();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) undo();
  // The panel refreshes on the render task, so presses made meanwhile apply at
  // once and the next frame shows only the final board.
  if (mappedInput.wasPressed(B::Left)) move(Dir::LEFT);
  if (mode == Mode::PLAY && mappedInput.wasPressed(B::Right)) move(Dir::RIGHT);
  if (mode == Mode::PLAY && mappedInput.wasPressed(B::Up)) move(Dir::UP);
  if (mode == Mode::PLAY && mappedInput.wasPressed(B::Down)) move(Dir::DOWN);
}

// ---- persistence ----------------------------------------------------------------

void Game2048Activity::load() {
  char json[SAVE_BYTES] = {};
  char cells[CELLS + 1] = {}, undoCells[CELLS + 1] = {};
  unsigned long savedScore = 0, savedBest = 0, savedMoves = 0, savedUndoScore = 0, savedUndoMoves = 0;
  int savedWon = 0;
  int fields = 0;
  if (Storage.exists(SAVE_PATH) && Storage.readFileToBuffer(SAVE_PATH, json, sizeof(json)) > 0) {
    fields = sscanf(json, SAVE_SCAN, cells, &savedScore, &savedBest, &savedMoves, &savedWon, undoCells, &savedUndoScore,
                    &savedUndoMoves);
  }
  best = fields >= 3 ? savedBest : 0;
  if (fields < 5 || !decodeBoard(cells, board) || g2048::emptyCount(board) == CELLS) {
    newGame();
    return;
  }
  score = savedScore;
  moves = savedMoves;
  won = savedWon != 0;
  hasUndo = fields >= 6 && decodeBoard(undoCells, undoBoard);
  undoScore = savedUndoScore;
  undoMoves = savedUndoMoves;
  best = std::max(best, score);
  mode = g2048::canMove(board) ? Mode::PLAY : Mode::OVER;
  cleanRequested = true;
}

void Game2048Activity::save() {
  if (!saveDirty) return;
  char cells[CELLS + 1], undoCells[CELLS + 1];
  encodeBoard(board, cells);
  encodeBoard(hasUndo ? undoBoard : board, undoCells);
  char json[SAVE_BYTES];
  snprintf(json, sizeof(json),
           "{\"board\":\"%s\",\"score\":%lu,\"best\":%lu,\"moves\":%lu,\"won\":%d,\"undo\":\"%s\",\"undoScore\":%lu,"
           "\"undoMoves\":%lu}",
           cells, static_cast<unsigned long>(score), static_cast<unsigned long>(best),
           static_cast<unsigned long>(moves), won ? 1 : 0, hasUndo ? undoCells : "",
           static_cast<unsigned long>(hasUndo ? undoScore : 0), static_cast<unsigned long>(hasUndo ? undoMoves : 0));
  if (!Storage.ensureDirectoryExists(SAVE_DIR) || !Storage.writeFile(SAVE_PATH, String(json))) {
    LOG_ERR("2048", "Failed to write %s", SAVE_PATH);
  }
  saveDirty = false;
}

// ---- drawing ----------------------------------------------------------------------

int Game2048Activity::cellX(const int cell) const { return boardX + GAP + (cell % g2048::SIDE) * (tile + GAP); }

int Game2048Activity::cellY(const int cell) const { return boardY + GAP + (cell / g2048::SIDE) * (tile + GAP); }

void Game2048Activity::drawHeader() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_H, false);
  // Logo: "20" over "48" on a black badge.
  constexpr int LOGO = 104, LOGO_X = 20, LOGO_Y = 18;
  renderer.fillRoundedRect(LOGO_X, LOGO_Y, LOGO, LOGO, 14, Color::Black);
  g2048::drawNumber(renderer, LOGO_X + LOGO / 2, LOGO_Y + 30, 20, LOGO - 30, 34, false, 0);
  g2048::drawNumber(renderer, LOGO_X + LOGO / 2, LOGO_Y + LOGO - 30, 48, LOGO - 30, 34, false, 0);

  // Score and best boxes.
  const int boxW = (width - (LOGO_X + LOGO + 20) - 20 - 12) / 2;
  const int boxH = 76;
  const StrId labels[2] = {StrId::STR_PLAY_SCORE, StrId::STR_PLAY_BEST};
  const uint32_t values[2] = {score, best};
  for (int i = 0; i < 2; i++) {
    const int x = LOGO_X + LOGO + 20 + i * (boxW + 12);
    renderer.drawRoundedRect(x, LOGO_Y, boxW, boxH, 2, 12, true);
    const char* label = I18N.get(labels[i]);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(SMALL_FONT_ID, x + (boxW - lw) / 2, LOGO_Y + 8, label, true, EpdFontFamily::BOLD);
    g2048::drawNumber(renderer, x + boxW / 2, LOGO_Y + 50, values[i], boxW - 24, 28, true, 0);
  }

  // Under the boxes: the last move's gain and the move count.
  const int rowY = LOGO_Y + boxH + 8;
  const int scoreX = LOGO_X + LOGO + 20;
  if (lastGain > 0) {
    char gain[16];
    snprintf(gain, sizeof(gain), "+%lu", static_cast<unsigned long>(lastGain));
    const int gw = renderer.getTextWidth(UI_12_FONT_ID, gain, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, scoreX + (boxW - gw) / 2, rowY, gain, true, EpdFontFamily::BOLD);
  }
  char count[32];
  snprintf(count, sizeof(count), "%s %lu", tr(STR_2048_MOVES), static_cast<unsigned long>(moves));
  const int cw = renderer.getTextWidth(UI_10_FONT_ID, count);
  renderer.drawText(UI_10_FONT_ID, scoreX + boxW + 12 + (boxW - cw) / 2, rowY + 3, count);
}

// The road to 2048: one swatch per tile value, filled in once reached.
void Game2048Activity::drawLadder(const uint8_t top) const {
  const int width = renderer.getScreenWidth();
  const int bandBottom = boardY - 4;
  renderer.fillRect(SIDE_HINT_W, HEADER_H, width - 2 * SIDE_HINT_W, bandBottom - HEADER_H, false);
  constexpr int STEPS = g2048::WIN_EXP;
  const int total = STEPS * LADDER_TILE + (STEPS - 1) * LADDER_GAP;
  const int x0 = (width - total) / 2;
  const int y = HEADER_H + 14;
  for (int i = 0; i < STEPS; i++) {
    const uint8_t exp = static_cast<uint8_t>(i + 1);
    const int x = x0 + i * (LADDER_TILE + LADDER_GAP);
    if (exp <= top) {
      g2048::drawTile(renderer, x, y, LADDER_TILE, exp, false);
    } else {
      renderer.drawRoundedRect(x + 2, y + 2, LADDER_TILE - 4, LADDER_TILE - 4, 1, 4, true);
    }
    char label[8];
    const uint32_t value = g2048::tileValue(exp);
    if (value >= 1024) {
      snprintf(label, sizeof(label), "%luK", static_cast<unsigned long>(value / 1024));
    } else {
      snprintf(label, sizeof(label), "%lu", static_cast<unsigned long>(value));
    }
    const bool current = exp == std::min<uint8_t>(top, STEPS);
    const auto style = current ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label, style);
    renderer.drawText(SMALL_FONT_ID, x + (LADDER_TILE - lw) / 2, y + LADDER_TILE + 5, label, true, style);
    if (current) renderer.fillRect(x + 6, y + LADDER_TILE + 25, LADDER_TILE - 12, 3, true);
  }
}

void Game2048Activity::drawBoardFrame() const {
  renderer.drawRoundedRect(boardX, boardY, boardSize, boardSize, BOARD_LINE, BOARD_RADIUS, true);
}

void Game2048Activity::drawCell(const int cell, const bool isNew, const bool isMerged) const {
  const int x = cellX(cell), y = cellY(cell);
  constexpr int M = GAP / 2;
  // Restore the board texture around the cell, half the gap included, so last
  // frame's new-tile brackets or merge ring disappear.
  g2048::fillRounded(renderer, x - M, y - M, tile + 2 * M, tile + 2 * M, 0, g2048::PAT_BOARD, 0, 0);
  const uint8_t exp = board.cells[cell];
  if (exp == 0) {
    g2048::fillRounded(renderer, x + 3, y + 3, tile - 6, tile - 6, tile / 10, g2048::PAT_WHITE, x, y);
    renderer.drawRoundedRect(x + 3, y + 3, tile - 6, tile - 6, 1, tile / 10, true);
    return;
  }
  g2048::drawTile(renderer, x, y, tile, exp, true);
  if (isNew) g2048::drawNewMark(renderer, x, y, tile);
  if (isMerged) g2048::drawMergeMark(renderer, x, y, tile);
}

void Game2048Activity::drawHints() const {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int hintsH = UITheme::getInstance().getMetrics().buttonHintsHeight;
  renderer.fillRect(0, height - hintsH, width, hintsH, false);
  const char* back = tr(STR_2048_MENU);
  const char* confirm = "";
  const char* left = "";
  const char* right = "";
  switch (mode) {
    case Mode::PLAY:
      confirm = hasUndo ? tr(STR_2048_UNDO) : "";
      left = tr(STR_DIR_LEFT);
      right = tr(STR_DIR_RIGHT);
      break;
    case Mode::MENU:
      back = tr(STR_BACK);
      confirm = tr(STR_SELECT);
      left = tr(STR_DIR_UP);
      right = tr(STR_DIR_DOWN);
      break;
    case Mode::WON:
      confirm = tr(STR_2048_KEEP_GOING);
      break;
    case Mode::OVER:
      confirm = hasUndo ? tr(STR_2048_UNDO) : "";
      break;
  }
  const auto labels = mappedInput.mapLabels(back, confirm, left, right);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Menu, win and game-over cards share one frame over the board; the win card is inverted.
void Game2048Activity::drawCard() const {
  const bool dark = mode == Mode::WON;
  const int w = boardSize - 2 * 44;
  const int h = mode == Mode::MENU ? 300 : 250;
  const int x = boardX + (boardSize - w) / 2;
  const int y = boardY + (boardSize - h) / 2;
  const int cx = x + w / 2;
  renderer.fillRoundedRect(x - 6, y - 6, w + 12, h + 12, 22, Color::White);
  renderer.fillRoundedRect(x, y, w, h, 16, dark ? Color::Black : Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 16, true);
  renderer.drawRoundedRect(x + 7, y + 7, w - 14, h - 14, 1, 11, !dark);

  if (mode == Mode::MENU) {
    g2048::drawNumber(renderer, cx, y + 44, 2048, w / 2, 40, true, 0);
    static constexpr StrId ITEMS[MENU_ITEMS] = {StrId::STR_RESUME, StrId::STR_2048_NEW_GAME, StrId::STR_2048_EXIT};
    constexpr int ROW_H = 54, ROW_STEP = 66;
    for (int i = 0; i < MENU_ITEMS; i++) {
      const int ry = y + 90 + i * ROW_STEP;
      const bool selected = i == menuIndex;
      if (selected) {
        renderer.fillRoundedRect(x + 30, ry, w - 60, ROW_H, ROW_H / 2, Color::Black);
      } else {
        renderer.drawRoundedRect(x + 30, ry, w - 60, ROW_H, 2, ROW_H / 2, true);
      }
      renderer.drawCenteredText(UI_12_FONT_ID, ry + 13, I18N.get(ITEMS[i]), !selected, EpdFontFamily::BOLD);
    }
    return;
  }

  char line[48];
  if (dark) {
    g2048::drawCrown(renderer, cx, y + 70, 70, false);
    g2048::drawNumber(renderer, cx, y + 118, 2048, w - 80, 60, false, 0);
    renderer.drawCenteredText(UI_12_FONT_ID, y + 164, tr(STR_2048_WON), false, EpdFontFamily::BOLD);
    snprintf(line, sizeof(line), "%s  %lu", tr(STR_PLAY_SCORE), static_cast<unsigned long>(score));
    renderer.drawCenteredText(UI_10_FONT_ID, y + 202, line, false);
    return;
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y + 32, tr(STR_PLAY_GAME_OVER), true, EpdFontFamily::BOLD);
  g2048::drawNumber(renderer, cx, y + 120, score, w - 80, 56, true, 0);
  const bool record = score > 0 && score >= best;
  snprintf(line, sizeof(line), "%s  %lu", tr(STR_PLAY_BEST), static_cast<unsigned long>(best));
  renderer.drawCenteredText(UI_10_FONT_ID, y + 180, record ? tr(STR_2048_NEW_BEST) : line, true,
                            record ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

void Game2048Activity::render(RenderLock&& lock) {
  const bool full = fullRedraw;
  if (full) {
    renderer.clearScreen();
    g2048::fillRounded(renderer, boardX, boardY, boardSize, boardSize, BOARD_RADIUS, g2048::PAT_BOARD, 0, 0);
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  drawHeader();
  gainShown = true;
  const uint8_t top = g2048::maxExp(board);
  if (full || top != shownMax) drawLadder(top);
  shownMax = top;
  bool cellsChanged = false;
  for (int i = 0; i < CELLS; i++) {
    const bool isNew = i == newCell;
    const bool isMerged = (mergedCells >> i) & 1;
    if (full || shownCells[i] != board.cells[i] || (i == shownNew) != isNew || ((shownMerged >> i) & 1) != isMerged) {
      drawCell(i, isNew, isMerged);
      shownCells[i] = board.cells[i];
      cellsChanged = true;
    }
  }
  shownNew = newCell;
  shownMerged = mergedCells;
  if (cellsChanged) drawBoardFrame();
  drawHints();
  if (mode != Mode::PLAY) drawCard();
  fullRedraw = false;

  const bool clean = cleanRequested || movesSinceClean >= MOVES_PER_CLEAN;
  if (clean) movesSinceClean = 0;
  cleanRequested = false;
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
