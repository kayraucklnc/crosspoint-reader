// Drawing for Connect4Activity: header card, drop lane, board, legend, menu.
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Connect4Activity.h"
#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE_ROOM = 36;  // keeps the board clear of the X3 edge-button hints
constexpr int PAD = 8;         // board frame around the holes
constexpr int MAX_CELL = 72;
constexpr int BOARD_RADIUS = 14;
constexpr int HEADER_H = 150;
constexpr int FRAMES_PER_CLEAN = 30;
constexpr int MARK_R = 5;     // last-move dot
constexpr int HOLE_EDGE = 2;  // outline around each hole

constexpr StrId LEVEL_NAMES[c4::LEVEL_COUNT] = {StrId::STR_MS_EASY, StrId::STR_MS_MEDIUM, StrId::STR_MS_HARD};
constexpr StrId STARTER_NAMES[] = {StrId::STR_C4_YOU, StrId::STR_C4_COMPUTER, StrId::STR_C4_ALTERNATE};

// Solid circle from horizontal spans; the half-pixel keeps the rim round.
void fillDisc(const GfxRenderer& r, const int cx, const int cy, const int radius, const bool black) {
  const float rr = (radius + 0.5f) * (radius + 0.5f);
  for (int dy = -radius; dy <= radius; dy++) {
    const int dx = static_cast<int>(std::sqrt(rr - static_cast<float>(dy * dy)));
    r.fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, black);
  }
}

void ringDisc(const GfxRenderer& r, const int cx, const int cy, const int radius, const int width) {
  fillDisc(r, cx, cy, radius, true);
  fillDisc(r, cx, cy, radius - width, false);
}

// Human discs are solid; the computer's are thick rings, so the two read
// apart at a glance without colour.
void drawDisc(const GfxRenderer& r, const int cx, const int cy, const int radius, const int side) {
  if (side == c4::HUMAN) {
    fillDisc(r, cx, cy, radius, true);
  } else {
    ringDisc(r, cx, cy, radius, std::max(3, radius * 2 / 5));
  }
}

// Round-capped thick line, stamped from discs.
void thickLine(const GfxRenderer& r, const int x1, const int y1, const int x2, const int y2, const int radius,
               const bool black) {
  const int steps = std::max(std::abs(x2 - x1), std::abs(y2 - y1));
  for (int i = 0; i <= steps; i += 2) {
    fillDisc(r, x1 + (x2 - x1) * i / std::max(1, steps), y1 + (y2 - y1) * i / std::max(1, steps), radius, black);
  }
  fillDisc(r, x2, y2, radius, black);
}

void downArrow(const GfxRenderer& r, const int cx, const int top, const int half) {
  const int xs[3] = {cx - half, cx + half, cx};
  const int ys[3] = {top, top, top + half};
  r.fillPolygon(xs, ys, 3, true);
}
}  // namespace

// Header card on top; the drop lane and the board centred in the rest.
void Connect4Activity::layout() {
  const int width = renderer.getScreenWidth();
  const int hintsTop = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
  cell = std::min((width - 2 * SIDE_ROOM - 2 * PAD) / c4::COLS, MAX_CELL) & ~1;
  const int boardW = c4::COLS * cell + 2 * PAD;
  const int boardH = c4::ROWS * cell + 2 * PAD;
  boardX = (width - boardW) / 2;
  headerY = 10;
  headerH = HEADER_H;
  const int laneH = cell + 20;
  const int top = headerY + headerH + std::max(0, (hintsTop - 8 - headerY - headerH - laneH - boardH) / 2);
  laneY = top + cell / 2 + 4;
  boardY = top + laneH;
}

int Connect4Activity::columnX(const int col) const { return boardX + PAD + col * cell + cell / 2; }

int Connect4Activity::rowY(const int row) const { return boardY + PAD + (c4::ROWS - 1 - row) * cell + cell / 2; }

// Status card; it turns black with the result when the game ends.
void Connect4Activity::drawHeader() const {
  const int x = boardX + PAD, w = c4::COLS * cell, y = headerY, h = headerH;
  const bool over = phase == Phase::OVER;
  if (over) {
    renderer.fillRoundedRect(x, y, w, h, 12, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 2, 12, true);
  }
  const bool ink = !over;
  renderer.drawText(UI_10_FONT_ID, x + 16, y + 12, tr(STR_PLAY_CONNECT4), ink, EpdFontFamily::BOLD);
  const char* levelName = I18N.get(LEVEL_NAMES[gameLevel]);
  const int levelW = renderer.getTextWidth(UI_10_FONT_ID, levelName);
  renderer.drawText(UI_10_FONT_ID, x + w - 16 - levelW, y + 12, levelName, ink);

  const char* status;
  if (over) {
    const int winner = board.winner();
    status = winner == c4::HUMAN ? tr(STR_C4_YOU_WIN) : winner == c4::COMPUTER ? tr(STR_C4_AI_WINS) : tr(STR_C4_DRAW);
  } else if (phase != Phase::HUMAN || runningJob == Job::HINT || pendingJob == Job::HINT) {
    status = tr(STR_C4_THINKING);
  } else {
    status = tr(STR_C4_YOUR_TURN);
  }
  // The card is centred on the screen, so screen-centred text is card-centred.
  renderer.drawCenteredText(over ? NOTOSANS_18_FONT_ID : UI_12_FONT_ID, y + h / 2 - (over ? 26 : 20), status, ink,
                            EpdFontFamily::BOLD);
  char line[64];
  snprintf(line, sizeof(line), tr(STR_C4_STATS), stats[gameLevel][WON], stats[gameLevel][LOST],
           stats[gameLevel][DRAWN]);
  renderer.drawCenteredText(UI_10_FONT_ID, y + h - 36, line, ink, over ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

// The disc about to drop: the human's over the cursor (hollow while the
// computer thinks), the computer's over its column, and the hint halo.
void Connect4Activity::drawLane() const {
  const int radius = cell / 2 - 8;
  const int arrowTop = laneY + radius + 4;
  if (phase == Phase::AI_HOVER && aiColumn >= 0) {
    drawDisc(renderer, columnX(aiColumn), laneY, radius, c4::COMPUTER);
    downArrow(renderer, columnX(aiColumn), arrowTop, 6);
    return;
  }
  if (phase == Phase::OVER) return;
  if (hintColumn >= 0 && phase == Phase::HUMAN) {
    const int hx = columnX(hintColumn);
    ringDisc(renderer, hx, laneY, radius + 7, 3);
    if (hintColumn != cursor) downArrow(renderer, hx, arrowTop, 6);
  }
  const int cx = columnX(cursor);
  if (phase == Phase::HUMAN) {
    drawDisc(renderer, cx, laneY, radius, c4::HUMAN);
    downArrow(renderer, cx, arrowTop, 6);
  } else {
    ringDisc(renderer, cx, laneY, radius, 2);
  }
}

// A mid-grey board, so solid discs stand apart from it, with outlined holes.
void Connect4Activity::drawBoard() const {
  const int boardW = c4::COLS * cell + 2 * PAD;
  const int boardH = c4::ROWS * cell + 2 * PAD;
  renderer.fillRoundedRect(boardX, boardY, boardW, boardH, BOARD_RADIUS, Color::DarkGray);
  renderer.drawRoundedRect(boardX, boardY, boardW, boardH, 3, BOARD_RADIUS, true);
  const int hole = cell / 2 - 4;
  const int radius = hole - 5;
  const uint64_t line = board.winLine();
  const int lastCol = board.lastColumn(), lastRow = board.lastRow();
  for (int col = 0; col < c4::COLS; col++) {
    for (int row = 0; row < c4::ROWS; row++) {
      const int cx = columnX(col), cy = rowY(row);
      // Winning discs sit on white tiles instead of round holes.
      if (line & c4::cellBit(col, row)) {
        const int tx = cx - cell / 2 + 2, ty = cy - cell / 2 + 2;
        renderer.fillRoundedRect(tx, ty, cell - 4, cell - 4, 10, Color::White);
        renderer.drawRoundedRect(tx, ty, cell - 4, cell - 4, 3, 10, true);
      } else {
        fillDisc(renderer, cx, cy, hole, true);
        fillDisc(renderer, cx, cy, hole - HOLE_EDGE, false);
      }
      const int owner = board.cell(col, row);
      if (owner < 0) continue;
      drawDisc(renderer, cx, cy, radius, owner);
      if (col == lastCol && row == lastRow && !line) fillDisc(renderer, cx, cy, MARK_R, owner != c4::HUMAN);
    }
  }
  drawWinLine();
}

// A white stroke with a black edge through the four, visible over both kinds.
// The line's end cells are its lowest and highest bits.
void Connect4Activity::drawWinLine() const {
  const uint64_t line = board.winLine();
  if (!line) return;
  const int low = __builtin_ctzll(line), high = 63 - __builtin_clzll(line);
  const int x1 = columnX(low / c4::STRIDE), y1 = rowY(low % c4::STRIDE);
  const int x2 = columnX(high / c4::STRIDE), y2 = rowY(high % c4::STRIDE);
  thickLine(renderer, x1, y1, x2, y2, 7, true);
  thickLine(renderer, x1, y1, x2, y2, 3, false);
}

void Connect4Activity::drawHints() const {
  const bool over = phase == Phase::OVER;
  const char* back = menuOpen ? tr(STR_RESUME) : tr(STR_MS_MENU);
  const char* confirm = menuOpen                ? tr(STR_SELECT)
                        : over                  ? tr(STR_PLAY_AGAIN)
                        : phase == Phase::HUMAN ? tr(STR_C4_DROP)
                                                : "";
  const char* left = menuOpen ? tr(STR_DIR_UP) : over ? "" : tr(STR_DIR_LEFT);
  const char* right = menuOpen ? tr(STR_DIR_DOWN) : over ? "" : tr(STR_DIR_RIGHT);
  const auto labels = mappedInput.mapLabels(back, confirm, left, right);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (!menuOpen && phase != Phase::OVER) {
    const bool canUndo = board.moveCount() >= (phase == Phase::HUMAN ? 2 : 1);
    GUI.drawSideButtonHints(renderer, canUndo ? tr(STR_2048_UNDO) : "", phase == Phase::HUMAN ? tr(STR_SOKO_HINT) : "");
  }
}

void Connect4Activity::drawMenu() const {
  constexpr int TITLE_H = 60, ROW_H = 52;
  const int w = c4::COLS * cell - 24;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int h = TITLE_H + MENU_ITEMS * ROW_H + 12;
  const int y = boardY + (c4::ROWS * cell + 2 * PAD - h) / 2;
  renderer.fillRoundedRect(x - 6, y - 6, w + 12, h + 12, 16, Color::White);
  renderer.drawRoundedRect(x, y, w, h, 3, 12, true);
  renderer.drawRoundedRect(x + 6, y + 6, w - 12, h - 12, 1, 8, true);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 20, tr(STR_PLAY_CONNECT4), true, EpdFontFamily::BOLD);

  static constexpr StrId LABELS[MENU_ITEMS] = {StrId::STR_2048_NEW_GAME, StrId::STR_TRIVIA_DIFFICULTY,
                                               StrId::STR_C4_STARTS, StrId::STR_MS_EXIT};
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int rowY = y + TITLE_H + i * ROW_H;
    const bool selected = i == menuIndex;
    if (selected) renderer.fillRoundedRect(x + 16, rowY, w - 32, ROW_H - 6, 8, Color::Black);
    renderer.drawText(UI_12_FONT_ID, x + 32, rowY + 9, I18N.get(LABELS[i]), !selected, EpdFontFamily::BOLD);
    const char* value = i == ITEM_LEVEL     ? I18N.get(LEVEL_NAMES[level])
                        : i == ITEM_STARTER ? I18N.get(STARTER_NAMES[starter])
                                            : nullptr;
    if (!value) continue;
    const int valueW = renderer.getTextWidth(UI_12_FONT_ID, value);
    renderer.drawText(UI_12_FONT_ID, x + w - 32 - valueW, rowY + 9, value, !selected);
  }
}

void Connect4Activity::render(RenderLock&& lock) {
  const bool clean = cleanRefresh || ++framesSinceClean >= FRAMES_PER_CLEAN;
  renderer.clearScreen();
  drawHeader();
  drawLane();
  drawBoard();
  drawHints();
  if (menuOpen) drawMenu();
  const uint32_t shownHover = phase == Phase::AI_HOVER ? hoverId : 0;
  if (clean) {
    cleanRefresh = false;
    framesSinceClean = 0;
  }
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  if (shownHover != 0) {
    presentedAtMs.store(static_cast<uint32_t>(millis()));
    presentedHover.store(shownHover);
  }
}

void Connect4Activity::drawMenuIcon(const GfxRenderer& renderer, const int x, const int y, const int size) {
  constexpr int N = 4;
  const int c = (size - 8) / N;
  const int ox = x + (size - N * c) / 2, oy = y + (size - N * c) / 2;
  renderer.fillRoundedRect(ox - 4, oy - 4, N * c + 8, N * c + 8, 8, Color::DarkGray);
  renderer.drawRoundedRect(ox - 4, oy - 4, N * c + 8, N * c + 8, 2, 8, true);
  // Top row first: -1 empty, 0 solid (human), 1 ring (computer).
  static constexpr int8_t CELLS[N * N] = {-1, -1, -1, -1, -1, 0, -1, -1, -1, 1, 0, -1, 1, 0, 1, 0};
  for (int i = 0; i < N * N; i++) {
    const int cx = ox + (i % N) * c + c / 2, cy = oy + (i / N) * c + c / 2;
    fillDisc(renderer, cx, cy, c / 2 - 1, true);
    fillDisc(renderer, cx, cy, c / 2 - 2, false);
    if (CELLS[i] >= 0) drawDisc(renderer, cx, cy, c / 2 - 3, CELLS[i]);
  }
}
