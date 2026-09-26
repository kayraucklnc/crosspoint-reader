#include "MinesweeperActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "MinesweeperArt.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
using mines::Outcome;
using mines::State;
using Button = MappedInputManager::Button;
namespace art = mines::art;

constexpr const char* SAVE_DIR = "/.crosspoint/games";
constexpr const char* SAVE_PATH = "/.crosspoint/games/minesweeper.json";

struct Level {
  uint8_t cols;
  uint8_t rows;
  uint8_t mines;
  StrId name;
};
constexpr Level LEVELS[] = {
    {9, 9, 10, StrId::STR_MS_EASY},
    {12, 16, 30, StrId::STR_MS_MEDIUM},
    {16, 24, 60, StrId::STR_MS_HARD},
};

struct Direction {
  Button button;
  int8_t dx;
  int8_t dy;
};
constexpr Direction DIRECTIONS[] = {
    {Button::Left, -1, 0},
    {Button::Right, 1, 0},
    {Button::Up, 0, -1},
    {Button::Down, 0, 1},
};

constexpr int HUD_HEIGHT = 42;
constexpr int HUD_DIGIT_SCALE = 3;
constexpr int SIDE_MARGIN = 12;
constexpr int BOTTOM_GAP = 2;
constexpr int FRAME_GAP = 4;  // white moat the cursor ring may overhang into
constexpr int FRAME_WIDTH = 2;
constexpr int ARROW_LANE = 16;  // margin needed outside the frame for row/column arrows
constexpr int MAX_TILE = 56;
constexpr unsigned long FLAG_HOLD_MS = 450;
constexpr unsigned long REPEAT_DELAY_MS = 380;
constexpr unsigned long REPEAT_MS = 150;
constexpr int FRAMES_PER_CLEAN = 30;
constexpr unsigned long FINALE_STEP_MS = 600;  // just over one X3 fast refresh
constexpr uint32_t MAX_SECONDS = 99 * 60 + 59;

void formatTime(char* out, const size_t size, const uint32_t seconds) {
  const uint32_t clamped = std::min(seconds, MAX_SECONDS);
  snprintf(out, size, "%lu:%02lu", static_cast<unsigned long>(clamped / 60), static_cast<unsigned long>(clamped % 60));
}

void formatBest(char* out, const size_t size, const uint16_t seconds) {
  char time[12] = "--";
  if (seconds > 0) formatTime(time, sizeof(time), seconds);
  snprintf(out, size, "%s %s", tr(STR_PLAY_BEST), time);
}
}  // namespace

// ---- lifecycle & persistence -------------------------------------------------

void MinesweeperActivity::onEnter() {
  Activity::onEnter();
  load();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
#ifdef SIMULATOR
  if (const char* seed = std::getenv("CROSSPOINT_SIM_MINES_SEED")) {
    fixedSeed = static_cast<uint32_t>(strtoul(seed, nullptr, 10));
  }
#endif
  newGame(difficulty);
}

void MinesweeperActivity::onExit() {
  save();
  Activity::onExit();
}

void MinesweeperActivity::load() {
  char buf[96] = {};
  if (!Storage.exists(SAVE_PATH) || Storage.readFileToBuffer(SAVE_PATH, buf, sizeof(buf) - 1) == 0) return;
  if (const char* d = strstr(buf, "\"difficulty\":")) {
    difficulty = std::clamp(atoi(d + 13), 0, DIFFICULTY_COUNT - 1);
  }
  if (const char* b = strstr(buf, "\"best\":[")) {
    const char* p = b + 8;
    for (int i = 0; i < DIFFICULTY_COUNT && *p; i++) {
      char* end = nullptr;
      const long value = strtol(p, &end, 10);
      if (end == p) break;
      bestSeconds[i] = static_cast<uint16_t>(std::clamp<long>(value, 0, MAX_SECONDS));
      p = *end == ',' ? end + 1 : end;
    }
  }
}

void MinesweeperActivity::save() {
  if (!saveNeeded) return;
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"difficulty\":%d,\"best\":[%u,%u,%u]}", difficulty, bestSeconds[0], bestSeconds[1],
           bestSeconds[2]);
  Storage.mkdir(SAVE_DIR);  // may already exist; a real failure surfaces in the write below
  if (!Storage.writeFile(SAVE_PATH, String(buf))) {
    LOG_ERR("MINES", "Failed to write %s", SAVE_PATH);
    return;
  }
  saveNeeded = false;
}

void MinesweeperActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int frame = FRAME_GAP + FRAME_WIDTH;
  const int top = HUD_HEIGHT + frame;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - BOTTOM_GAP - frame;
  const int availWidth = width - 2 * SIDE_MARGIN;
  const int availHeight = bottom - top;
  tile = std::min({availWidth / board.cols(), availHeight / board.rows(), MAX_TILE});
  boardX = (width - board.cols() * tile) / 2;
  boardY = top + (availHeight - board.rows() * tile) / 2;
}

void MinesweeperActivity::newGame(const int level) {
  const bool changed = level != difficulty;
  difficulty = std::clamp(level, 0, DIFFICULTY_COUNT - 1);
  if (changed) {
    saveNeeded = true;
    save();
  }
  const Level& spec = LEVELS[difficulty];
  board.reset(spec.cols, spec.rows, spec.mines);
  layout();
  cursor = (board.rows() / 2) * board.cols() + board.cols() / 2;
  heldDirection = -1;
  timerBankedMs = 0;
  timerRunning = false;
  newBest = false;
  overlay = Overlay::NONE;
  finaleStage = 0;
  fullRedraw = true;
  cleanRefresh = true;
  requestUpdate();
}

// ---- timer ---------------------------------------------------------------------

void MinesweeperActivity::startTimer() {
  if (timerRunning) return;
  timerStartMs = millis();
  timerRunning = true;
}

void MinesweeperActivity::stopTimer() {
  if (!timerRunning) return;
  timerBankedMs += static_cast<uint32_t>(millis() - timerStartMs);
  timerRunning = false;
}

uint32_t MinesweeperActivity::elapsedSeconds() const {
  const uint32_t ms = timerBankedMs + (timerRunning ? static_cast<uint32_t>(millis() - timerStartMs) : 0);
  return std::min(ms / 1000, MAX_SECONDS);
}

// ---- input ---------------------------------------------------------------------

void MinesweeperActivity::loop() {
  RenderLock lock(*this);
  if (finaleStage > 0) {
    handleFinale();
    return;
  }
  if (overlay == Overlay::MENU) {
    handleMenuInput(lock);
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    openMenu();
    return;
  }
  handleBoardInput();
}

void MinesweeperActivity::handleMenuInput(RenderLock& lock) {
  if (mappedInput.wasReleased(Button::Back)) {
    closeOverlay();
    return;
  }
  if (mappedInput.wasPressed(Button::NavPrevious)) {
    menuIndex = (menuIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  }
  if (mappedInput.wasPressed(Button::NavNext)) {
    menuIndex = (menuIndex + 1) % MENU_ITEMS;
    requestUpdate();
  }
  if (!mappedInput.wasReleased(Button::Confirm)) return;
  if (menuIndex == 0) {
    closeOverlay();
  } else if (menuIndex <= DIFFICULTY_COUNT) {
    newGame(menuIndex - 1);
  } else {
    lock.unlock();
    finish();
  }
}

void MinesweeperActivity::handleBoardInput() {
  if (overlay == Overlay::RESULT) {
    if (mappedInput.wasReleased(Button::Confirm)) {
      newGame(difficulty);
      return;
    }
    // Any direction hides the card so the finished board can be inspected.
    for (const auto& direction : DIRECTIONS) {
      if (mappedInput.wasPressed(direction.button)) {
        closeOverlay();
        return;
      }
    }
    return;
  }
  if (board.over() && mappedInput.wasReleased(Button::Confirm)) {
    newGame(difficulty);
    return;
  }

  for (int i = 0; i < 4; i++) {
    if (!mappedInput.wasPressed(DIRECTIONS[i].button)) continue;
    moveCursor(DIRECTIONS[i].dx, DIRECTIONS[i].dy, false);
    heldDirection = i;
    nextRepeatMs = millis() + REPEAT_DELAY_MS;
  }
  if (heldDirection >= 0) {
    const Direction& held = DIRECTIONS[heldDirection];
    if (!mappedInput.isPressed(held.button)) {
      heldDirection = -1;
    } else if (static_cast<long>(millis() - nextRepeatMs) >= 0) {
      moveCursor(held.dx, held.dy, true);
      nextRepeatMs = millis() + REPEAT_MS;
    }
  }

  if (board.over()) return;
  if (mappedInput.wasLongPressed(Button::Confirm, FLAG_HOLD_MS)) {
    toggleFlag();
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    press();
  }
}

// A tap wraps around the edge; a held repeat glides across opened blanks and
// parks at the edge, so it never lands on a cell with nothing to do.
void MinesweeperActivity::moveCursor(const int dx, const int dy, const bool glide) {
  const int cols = board.cols(), rows = board.rows();
  int col = cursor % cols, row = cursor / cols;
  if (!glide) {
    col = (col + dx + cols) % cols;
    row = (row + dy + rows) % rows;
  } else {
    do {
      const int nextCol = col + dx, nextRow = row + dy;
      if (nextCol < 0 || nextCol >= cols || nextRow < 0 || nextRow >= rows) break;
      col = nextCol;
      row = nextRow;
    } while (board.isBlank(row * cols + col));
  }
  const int next = row * cols + col;
  if (next == cursor) return;
  cursor = next;
  requestUpdate();
}

void MinesweeperActivity::press() {
  const bool wasReady = board.state() == State::READY;
  Outcome outcome;
  if (board.isOpen(cursor)) {
    outcome = board.pressNumber(cursor);
  } else {
    if (wasReady) rng = fixedSeed != 0 ? fixedSeed : (rng ^ static_cast<uint32_t>(millis()) * 2654435761u) | 1u;
    outcome = board.reveal(cursor, rng);
  }
  afterMove(outcome, wasReady);
}

void MinesweeperActivity::toggleFlag() {
  if (board.toggleFlag(cursor)) requestUpdate();
}

void MinesweeperActivity::afterMove(const Outcome outcome, const bool wasReady) {
  if (outcome == Outcome::NONE) return;
  if (wasReady) startTimer();
  if (outcome == Outcome::WON || outcome == Outcome::LOST) {
    stopTimer();
    if (outcome == Outcome::WON) {
      const auto seconds = static_cast<uint16_t>(std::max<uint32_t>(1, elapsedSeconds()));
      uint16_t& best = bestSeconds[difficulty];
      if (best == 0 || seconds < best) {
        best = seconds;
        newBest = true;
        saveNeeded = true;
        save();
      }
    }
    finaleStage = 1;
    finaleStepMs = millis();
  }
  requestUpdate();
}

void MinesweeperActivity::handleFinale() {
  bool skip = mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Back);
  for (const auto& direction : DIRECTIONS) skip = mappedInput.wasPressed(direction.button) || skip;
  if (skip || millis() - finaleStepMs >= FINALE_STEP_MS) advanceFinale(skip);
}

void MinesweeperActivity::advanceFinale(const bool skip) {
  const bool lost = board.state() == State::LOST;
  const int last = lost ? 3 : 2;
  const int next = skip ? last : finaleStage + 1;
  if (lost && next >= 2) {
    for (int i = 0; i < board.cellCount(); i++) {
      if (board.isMine(i) || board.isFlagged(i)) board.markDirty(i);
    }
  }
  if (next >= last) {
    finaleStage = 0;
    overlay = Overlay::RESULT;
  } else {
    finaleStage = static_cast<uint8_t>(next);
  }
  finaleStepMs = millis();
  requestUpdate();
}

void MinesweeperActivity::openMenu() {
  stopTimer();
  heldDirection = -1;
  overlay = Overlay::MENU;
  // After a finished game the likely next step is a fresh board.
  menuIndex = board.over() ? 1 + difficulty : 0;
  requestUpdate();
}

void MinesweeperActivity::closeOverlay() {
  overlay = Overlay::NONE;
  fullRedraw = true;
  if (board.state() == State::PLAYING) startTimer();
  requestUpdate();
}

// ---- drawing -------------------------------------------------------------------

// The cursor's tile is drawn inverted so it stands out on any tile type.
void MinesweeperActivity::drawCell(const int cell) const {
  const int x = boardX + (cell % board.cols()) * tile;
  const int y = boardY + (cell / board.cols()) * tile;
  const bool focused = cell == cursor;
  const bool lost = board.state() == State::LOST;
  // During the first loss frame only the tripped mine is shown.
  const bool revealAll = lost && finaleStage != 1;
  const bool mine = board.isMine(cell);
  const bool flagged = board.isFlagged(cell);
  if (board.isOpen(cell)) {
    art::openTile(renderer, x, y, tile);
    if (focused) renderer.fillRect(x + 1, y + 1, tile - 1, tile - 1, true);
    art::number(renderer, x, y, tile, board.number(cell), !focused);
    return;
  }
  const bool tripped = cell == board.trippedCell();
  if (lost && mine && !flagged && (revealAll || tripped)) {
    const bool dark = tripped || focused;
    art::openTile(renderer, x, y, tile);
    if (dark) renderer.fillRect(x + 1, y + 1, tile - 1, tile - 1, true);
    art::mine(renderer, x + (tile + 1) / 2, y + (tile + 1) / 2, tile * 2 / 9, !dark);
    return;
  }
  art::hiddenTile(renderer, x, y, tile, flagged, focused);
  if (revealAll && flagged && !mine) art::cross(renderer, x, y, tile, !focused);
}

// Frame with the cursor's row and column marked on all four edges: solid
// ticks across the frame, plus arrows where the margin has room for them.
void MinesweeperActivity::drawFrame() const {
  const int w = board.cols() * tile, h = board.rows() * tile;
  const int band = FRAME_GAP + FRAME_WIDTH;
  const int left = boardX - FRAME_GAP, top = boardY - FRAME_GAP;
  const int outerW = w + 2 * FRAME_GAP, outerH = h + 2 * FRAME_GAP;
  const int screenW = renderer.getScreenWidth();
  const int hintsTop = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
  const bool sideArrows = boardX - band >= ARROW_LANE;
  const bool topArrows = boardY - band - HUD_HEIGHT >= ARROW_LANE;
  const bool bottomArrows = hintsTop - (boardY + h + band) >= ARROW_LANE;

  renderer.fillRect(left, top, outerW, FRAME_GAP, false);
  renderer.fillRect(left, boardY + h, outerW, FRAME_GAP, false);
  renderer.fillRect(left, boardY, FRAME_GAP, h, false);
  renderer.fillRect(boardX + w, boardY, FRAME_GAP, h, false);
  if (sideArrows) {
    renderer.fillRect(boardX - band - ARROW_LANE, boardY, ARROW_LANE, h, false);
    renderer.fillRect(boardX + w + band, boardY, std::min(ARROW_LANE, screenW - boardX - w - band), h, false);
  }
  if (topArrows) renderer.fillRect(boardX, boardY - band - ARROW_LANE, w, ARROW_LANE, false);
  if (bottomArrows) renderer.fillRect(boardX, boardY + h + band, w, ARROW_LANE, false);
  renderer.drawRect(left - FRAME_WIDTH, top - FRAME_WIDTH, outerW + 2 * FRAME_WIDTH, outerH + 2 * FRAME_WIDTH,
                    FRAME_WIDTH, true);

  const int cx = boardX + (cursor % board.cols()) * tile;
  const int cy = boardY + (cursor / board.cols()) * tile;
  renderer.fillRect(cx + 2, boardY - band, tile - 3, band, true);
  renderer.fillRect(cx + 2, boardY + h, tile - 3, band, true);
  renderer.fillRect(boardX - band, cy + 2, band, tile - 3, true);
  renderer.fillRect(boardX + w, cy + 2, band, tile - 3, true);
  const int midX = cx + tile / 2, midY = cy + tile / 2;
  constexpr int A = ARROW_LANE - 6;  // arrow length; its base is 2A wide
  const auto arrow = [this](const int x0, const int y0, const int x1, const int y1, const int x2, const int y2) {
    const int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    renderer.fillPolygon(xs, ys, 3, true);
  };
  if (sideArrows) {
    const int l = boardX - band - 2, r = boardX + w + band + 1;
    arrow(l, midY, l - A, midY - A, l - A, midY + A);
    arrow(r, midY, r + A, midY - A, r + A, midY + A);
  }
  if (topArrows) {
    const int t = boardY - band - 2;
    arrow(midX, t, midX - A, t - A, midX + A, t - A);
  }
  if (bottomArrows) {
    const int b = boardY + h + band + 1;
    arrow(midX, b, midX - A, b + A, midX + A, b + A);
  }
}

void MinesweeperActivity::drawHud() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HUD_HEIGHT, false);
  const int midY = HUD_HEIGHT / 2;
  const int digitY = midY - art::DIGIT_HEIGHT * HUD_DIGIT_SCALE / 2;
  char text[16];

  art::mine(renderer, SIDE_MARGIN + 10, midY, 6, true);
  snprintf(text, sizeof(text), "%d", board.minesLeft());
  art::digits(renderer, text, SIDE_MARGIN + 30, digitY, HUD_DIGIT_SCALE, true, true);

  formatTime(text, sizeof(text), elapsedSeconds());
  const int timeWidth = art::digitsWidth(text, HUD_DIGIT_SCALE, true);
  art::digits(renderer, text, width - SIDE_MARGIN - timeWidth, digitY, HUD_DIGIT_SCALE, true, true);
  art::clock(renderer, width - SIDE_MARGIN - timeWidth - 18, midY, 9);

  renderer.drawCenteredText(UI_10_FONT_ID, 1, I18N.get(LEVELS[difficulty].name), true, EpdFontFamily::BOLD);
  // Until the first move the second line teaches the one hidden gesture.
  char best[32];
  formatBest(best, sizeof(best), bestSeconds[difficulty]);
  renderer.drawCenteredText(SMALL_FONT_ID, 20, board.state() == State::READY ? tr(STR_MS_TIP) : best);
}

void MinesweeperActivity::drawHints() const {
  const int height = renderer.getScreenHeight();
  const int hintsHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  renderer.fillRect(0, height - hintsHeight, renderer.getScreenWidth(), hintsHeight, false);
  MappedInputManager::Labels labels{};
  if (overlay == Overlay::MENU) {
    labels = mappedInput.mapLabels(tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  } else {
    const char* confirm = board.over() ? tr(STR_PLAY_AGAIN) : tr(STR_MS_OPEN);
    labels = mappedInput.mapLabels(tr(STR_MS_MENU), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void MinesweeperActivity::drawMenu() const {
  static constexpr StrId HELP[] = {StrId::STR_MS_HELP1, StrId::STR_MS_HELP2, StrId::STR_MS_HELP3};
  constexpr int TITLE_H = 58, ROW_H = 50, LINE_H = 22;
  const int width = renderer.getScreenWidth();
  const int boxW = std::min(width - 40, 452);
  const int boxX = (width - boxW) / 2;
  constexpr int HELP_LINES = sizeof(HELP) / sizeof(HELP[0]);
  const int boxH = TITLE_H + MENU_ITEMS * ROW_H + 22 + HELP_LINES * LINE_H + 18;
  const int boxY = std::max(HUD_HEIGHT + 4, (renderer.getScreenHeight() - boxH) / 2);

  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, 12, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 3, 12, true);
  renderer.drawRoundedRect(boxX + 6, boxY + 6, boxW - 12, boxH - 12, 1, 8, true);
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 20, tr(STR_PLAY_MINES), true, EpdFontFamily::BOLD);

  for (int i = 0; i < MENU_ITEMS; i++) {
    const int rowY = boxY + TITLE_H + i * ROW_H;
    const bool selected = i == menuIndex;
    const int level = i - 1;
    const bool isLevel = level >= 0 && level < DIFFICULTY_COUNT;
    if (selected) renderer.fillRoundedRect(boxX + 16, rowY, boxW - 32, ROW_H - 6, 8, Color::Black);
    const char* label = i == 0 ? tr(STR_RESUME) : isLevel ? I18N.get(LEVELS[level].name) : tr(STR_MS_EXIT);
    const bool current = isLevel && level == difficulty;
    renderer.drawText(UI_12_FONT_ID, boxX + 32, rowY + 9, label, !selected,
                      current ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (!isLevel) continue;
    char best[32], detail[64];
    formatBest(best, sizeof(best), bestSeconds[level]);
    snprintf(detail, sizeof(detail), "%dx%d  %d %s  |  %s", LEVELS[level].cols, LEVELS[level].rows,
             LEVELS[level].mines, tr(STR_MS_MINES), best);
    const int detailW = renderer.getTextWidth(UI_10_FONT_ID, detail);
    renderer.drawText(UI_10_FONT_ID, boxX + boxW - 32 - detailW, rowY + 12, detail, !selected);
  }

  int y = boxY + TITLE_H + MENU_ITEMS * ROW_H + 8;
  renderer.drawLine(boxX + 24, y, boxX + boxW - 25, y, true);
  y += 14;
  for (const StrId id : HELP) {
    const std::string line = renderer.truncatedText(SMALL_FONT_ID, I18N.get(id), boxW - 48);
    renderer.drawText(SMALL_FONT_ID, boxX + 24, y, line.c_str());
    y += LINE_H;
  }
}

void MinesweeperActivity::drawResult() const {
  const bool won = board.state() == State::WON;
  const int width = renderer.getScreenWidth();
  const int boxW = std::min(width - 60, 400);
  constexpr int BOX_H = 156;
  const int boxX = (width - boxW) / 2;
  const int boxY = boardY + board.rows() * tile / 2 - BOX_H / 2;
  renderer.fillRoundedRect(boxX, boxY, boxW, BOX_H, 12, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, BOX_H, 3, 12, true);
  renderer.drawRoundedRect(boxX + 6, boxY + 6, boxW - 12, BOX_H - 12, 1, 8, true);

  const char* title = won ? tr(STR_MS_WON) : tr(STR_MS_LOST);
  const int titleW = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  const int iconX = (width - titleW) / 2 - 26;
  if (won) {
    art::flag(renderer, iconX, boxY + 34, 40);
  } else {
    art::mine(renderer, iconX, boxY + 34, 8, true);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, boxY + 22, title, true, EpdFontFamily::BOLD);

  char time[12], line[64];
  formatTime(time, sizeof(time), elapsedSeconds());
  if (won && newBest) {
    snprintf(line, sizeof(line), "%s %s  |  %s", tr(STR_MS_TIME), time, tr(STR_MS_NEW_BEST));
  } else {
    char best[32];
    formatBest(best, sizeof(best), bestSeconds[difficulty]);
    snprintf(line, sizeof(line), "%s %s  |  %s", tr(STR_MS_TIME), time, best);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, boxY + 66, line, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, boxY + 104, tr(STR_MS_RESULT_HINT));
}

void MinesweeperActivity::render(RenderLock&& lock) {
  const bool clean = cleanRefresh || ++framesSinceClean >= FRAMES_PER_CLEAN;
  if (fullRedraw) {
    renderer.clearScreen();
    board.markAllDirty();
    fullRedraw = false;
  }
  // The cursor frame overhangs its neighbours, so they repaint when it leaves.
  if (drawnCursor != cursor) {
    if (drawnCursor >= 0 && drawnCursor < board.cellCount()) {
      const int cols = board.cols(), col = drawnCursor % cols, row = drawnCursor / cols;
      for (int r = std::max(0, row - 1); r <= std::min(board.rows() - 1, row + 1); r++) {
        for (int c = std::max(0, col - 1); c <= std::min(cols - 1, col + 1); c++) board.markDirty(r * cols + c);
      }
    }
    board.markDirty(cursor);
    drawnCursor = cursor;
  }
  for (int i = 0; i < board.cellCount(); i++) {
    if (!board.isDirty(i)) continue;
    drawCell(i);
    board.clearDirty(i);
  }
  drawFrame();
  art::cursor(renderer, boardX + (cursor % board.cols()) * tile, boardY + (cursor / board.cols()) * tile, tile);
  drawHud();
  drawHints();
  if (overlay == Overlay::MENU) {
    drawMenu();
  } else if (overlay == Overlay::RESULT) {
    drawResult();
  }
  if (clean) {
    cleanRefresh = false;
    framesSinceClean = 0;
  }
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void MinesweeperActivity::drawMenuIcon(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int t = (size - 12) / 3;
  const int ox = x + (size - 3 * t) / 2, oy = y + (size - 3 * t) / 2;
  // 0 blank, 1-8 number, 9 hidden, 10 flag, 11 mine.
  static constexpr uint8_t CELLS[9] = {0, 1, 10, 1, 2, 9, 9, 11, 9};
  for (int i = 0; i < 9; i++) {
    const int cx = ox + (i % 3) * t, cy = oy + (i / 3) * t;
    const uint8_t kind = CELLS[i];
    if (kind >= 9 && kind <= 10) {
      art::hiddenTile(renderer, cx, cy, t, kind == 10);
      continue;
    }
    art::openTile(renderer, cx, cy, t);
    if (kind == 11) {
      art::mine(renderer, cx + (t + 1) / 2, cy + (t + 1) / 2, t / 4, true);
    } else {
      art::number(renderer, cx, cy, t, kind);
    }
  }
  renderer.drawRect(ox - 1, oy - 1, 3 * t + 2, 3 * t + 2, 2, true);
}
