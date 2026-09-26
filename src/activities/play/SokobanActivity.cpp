#include "SokobanActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "SokobanArt.h"
#include "components/UITheme.h"
#include "fontIds.h"

using sokoban::Board;
using sokoban::Dir;

namespace {
constexpr const char* SAVE_DIR = "/.crosspoint/games";
constexpr const char* SAVE_PATH = "/.crosspoint/games/sokoban.json";
constexpr size_t MAX_SAVE_BYTES = 8192;
constexpr int HEADER_HEIGHT = 72;
constexpr int SIDE = 12;
constexpr int MAX_TILE = 64;
constexpr int CLEAN_EVERY_MOVES = 40;
constexpr unsigned long EXIT_HOLD_MS = 700;
constexpr unsigned long HINT_HOLD_MS = 700;
constexpr unsigned long STAR_STEP_MS = 500;  // about one panel refresh per star
constexpr int SELECT_COLS = 5;
constexpr int SELECT_ROWS = 7;
constexpr int SELECT_PER_PAGE = SELECT_COLS * SELECT_ROWS;
constexpr int SIDE_HINT_ZONE = 40;  // X3 side-button hints hug both edges
constexpr int SIDE_HINT_TOP = 150;
constexpr int SIDE_HINT_BOTTOM = 240;

int hintsTop(const GfxRenderer& renderer) {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight;
}
}  // namespace

// ---- lifecycle --------------------------------------------------------------

void SokobanActivity::onEnter() {
  Activity::onEnter();
  // ~3.1 KB board (cells + 4096-move undo ring) and 1 KB of drawn-tile state,
  // held only while the game is open.
  board = makeUniqueNoThrow<Board>();
  shown = makeUniqueNoThrow<uint8_t[]>(sokoban::MAX_CELLS);
  pack.open();
  best = makeUniqueNoThrow<uint16_t[]>(pack.count());
  helped = makeUniqueNoThrow<uint8_t[]>((pack.count() + 7) / 8);
  if (!board || !shown || !best || !helped) {
    LOG_ERR("SOKO", "OOM: board/state for %d levels", pack.count());
    finish();
    return;
  }
  std::fill_n(best.get(), pack.count(), 0);
  std::fill_n(helped.get(), (pack.count() + 7) / 8, 0);
  loadProgress();
  startLevel(level);
}

void SokobanActivity::onExit() {
  saveProgress();
  guide.clear();
  board.reset();
  shown.reset();
  best.reset();
  helped.reset();
  Activity::onExit();
}

// ---- game flow ----------------------------------------------------------------

void SokobanActivity::startLevel(const int index) {
  const int target = std::clamp(index, 0, pack.count() - 1);
  if (!pack.load(target, *board, parMoves, parPushes)) {
    LOG_ERR("SOKO", "Level %d failed to load", target);
    if (target != 0 && pack.load(0, *board, parMoves, parPushes)) {
      level = 0;
    } else {
      finish();
      return;
    }
  } else {
    level = target;
  }
  progressDirty |= level != savedLevel;
  layoutBoard();
  guide.clear();
  guideTried = false;
  hintShown = false;
  hintsUsed = 0;
  stuck = board->stuck();
  mode = Mode::PLAY;
  newBest = false;
  fullRedraw = true;
  cleanRequested = true;
  requestUpdate();
}

// Fits the largest square tile; tall screens show wide levels sideways.
void SokobanActivity::layoutBoard() {
  const int areaW = renderer.getScreenWidth() - 2 * SIDE;
  const int areaTop = HEADER_HEIGHT + 4;
  const int areaH = hintsTop(renderer) - 8 - areaTop;
  const int w = board->width(), h = board->height();
  const int upright = std::min({MAX_TILE, areaW / w, areaH / h});
  const int sideways = std::min({MAX_TILE, areaW / h, areaH / w});
  transposed = sideways > upright;
  if (transposed) board->transpose();
  tile = std::max(upright, sideways);
  boardX = (renderer.getScreenWidth() - board->width() * tile) / 2;
  boardY = areaTop + (areaH - board->height() * tile) / 2;
}

void SokobanActivity::step(const Dir dir) {
  hintShown = false;
  if (board->move(dir) != Board::Step::BLOCKED) movesSinceClean++;
  stuck = board->stuck();
  if (board->solved()) completeLevel();
  requestUpdate();
}

void SokobanActivity::undo() {
  hintShown = false;
  if (board->undo() == Board::Step::BLOCKED) {
    requestUpdate();  // still clears a shown hint
    return;
  }
  stuck = board->stuck();
  movesSinceClean++;
  if (mode == Mode::SOLVED) {
    mode = Mode::PLAY;
    fullRedraw = true;  // lift the card
  }
  requestUpdate();
}

// Hinted solves count as solved (marked with a bulb) but never set a record
// over an unaided one; stars are for unaided solves only.
void SokobanActivity::completeLevel() {
  const int moves = board->moveCount();
  const bool unaided = hintsUsed == 0;
  newBest = unaided && (best[level] == 0 || isHelped(level) || moves < best[level]);
  if (newBest || best[level] == 0) {
    best[level] = static_cast<uint16_t>(moves);
    setHelped(level, !unaided);
    progressDirty = true;
  }
  mode = Mode::SOLVED;
  starsShown = 0;
  revealMs = millis();
  swallowRelease = false;
  saveProgress();
}

int SokobanActivity::nextLevel() const {
  const int count = pack.count();
  for (int i = 1; i <= count; i++) {
    const int candidate = (level + i) % count;
    if (best[candidate] == 0) return candidate;
  }
  return (level + 1) % count;
}

int SokobanActivity::solvedCount() const {
  int solved = 0;
  for (int i = 0; i < pack.count(); i++) solved += best[i] != 0;
  return solved;
}

// Three stars at par, two within a quarter over it, else one. 0 = par unknown.
int SokobanActivity::stars(const int moves) const {
  if (parMoves == 0 || moves == 0 || hintsUsed > 0) return 0;
  if (moves <= parMoves) return 3;
  if (moves <= parMoves + std::max(2, parMoves / 4)) return 2;
  return 1;
}

// ---- persistence ------------------------------------------------------------------

// {"level":N,"count":C,"best":[moves,...]}: a hand-rolled reader keeps the
// JSON library out of the game.
void SokobanActivity::loadProgress() {
  level = 0;
  bool haveLevel = false;
  HalFile file;
  // The built-in fallback never touches the save: it would clobber the pack's progress.
  if (!pack.builtIn() && Storage.exists(SAVE_PATH) && Storage.openFileForRead("SOKO", SAVE_PATH, file)) {
    const size_t size = std::min(file.fileSize(), MAX_SAVE_BYTES);
    auto text = makeUniqueNoThrow<char[]>(size + 1);
    if (!text) {
      LOG_ERR("SOKO", "OOM: progress %u bytes", static_cast<unsigned>(size));
    } else if (file.read(text.get(), size) == static_cast<int>(size)) {
      text[size] = '\0';
      if (const char* p = strstr(text.get(), "\"level\":")) {
        level = atoi(p + 8);
        haveLevel = true;
      }
      if (const char* p = strstr(text.get(), "\"best\":[")) {
        p += 8;
        for (int i = 0; i < pack.count() && *p && *p != ']'; i++) {
          char* end = nullptr;
          const long moves = strtol(p, &end, 10);
          if (end == p) break;
          best[i] = static_cast<uint16_t>(std::clamp(moves, 0L, 65535L));
          p = end;
          while (*p == ',' || *p == ' ') p++;
        }
      }
      static constexpr char HELPED_TAG[] = "\"helped\":[";
      if (const char* p = strstr(text.get(), HELPED_TAG)) {
        p += sizeof(HELPED_TAG) - 1;
        while (*p && *p != ']') {
          char* end = nullptr;
          const long index = strtol(p, &end, 10);
          if (end == p) break;
          if (index >= 0 && index < pack.count()) setHelped(static_cast<int>(index), true);
          p = end;
          while (*p == ',' || *p == ' ') p++;
        }
      }
    }
  }
  if (!haveLevel) {
    // First run: the first unsolved level.
    while (level < pack.count() - 1 && best[level] != 0) level++;
  }
  level = std::clamp(level, 0, pack.count() - 1);
  savedLevel = level;
  selectIndex = level;
  progressDirty = false;
}

void SokobanActivity::saveProgress() {
  if (!progressDirty || !best || pack.builtIn()) return;
  Storage.ensureDirectoryExists(SAVE_DIR);
  HalFile file;
  if (!Storage.openFileForWrite("SOKO", SAVE_PATH, file)) {
    LOG_ERR("SOKO", "Failed to write %s", SAVE_PATH);
    return;
  }
  char chunk[48];
  int n = snprintf(chunk, sizeof(chunk), "{\"level\":%d,\"count\":%d,\"best\":[", level, pack.count());
  file.write(chunk, n);
  for (int i = 0; i < pack.count(); i++) {
    n = snprintf(chunk, sizeof(chunk), i == 0 ? "%u" : ",%u", static_cast<unsigned>(best[i]));
    file.write(chunk, n);
  }
  static constexpr char HELPED_KEY[] = "],\"helped\":[";
  file.write(HELPED_KEY, sizeof(HELPED_KEY) - 1);
  bool first = true;
  for (int i = 0; i < pack.count(); i++) {
    if (!isHelped(i)) continue;
    n = snprintf(chunk, sizeof(chunk), first ? "%d" : ",%d", i);
    file.write(chunk, n);
    first = false;
  }
  file.write("]}\n", 3);
  savedLevel = level;
  progressDirty = false;
}

void SokobanActivity::setHelped(const int index, const bool on) {
  const uint8_t bit = static_cast<uint8_t>(1 << (index % 8));
  helped[index / 8] = on ? (helped[index / 8] | bit) : (helped[index / 8] & static_cast<uint8_t>(~bit));
}

// ---- input ------------------------------------------------------------------------

void SokobanActivity::loop() {
  if (!board || !shown || !best) return;
  RenderLock lock(*this);
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, EXIT_HOLD_MS)) {
    lock.unlock();
    finish();
    return;
  }
  switch (mode) {
    case Mode::PLAY:
      handlePlay();
      break;
    case Mode::MENU:
      handleMenu();
      break;
    case Mode::SELECT:
      handleSelect();
      break;
    case Mode::SOLVED:
      handleSolved();
      break;
    case Mode::OFF_PATH:
      handleOffPath();
      break;
    case Mode::SOLUTION:
      handleSolution();
      break;
  }
}

// Every press is applied immediately; renders coalesce, so a burst of presses
// during one refresh lands as a single frame showing the latest position.
void SokobanActivity::handlePlay() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasLongPressed(B::Confirm, HINT_HOLD_MS)) {
    requestHint(false);
    return;
  }
  if (mappedInput.wasReleased(B::Back)) undo();
  if (mappedInput.wasReleased(B::Confirm)) {
    mode = Mode::MENU;
    menuIndex = MENU_RESUME;
    requestUpdate();
    return;
  }
  static constexpr B KEYS[4] = {B::Up, B::Right, B::Down, B::Left};
  for (int d = 0; d < 4 && mode == Mode::PLAY; d++) {
    if (mappedInput.wasPressed(KEYS[d])) step(static_cast<Dir>(d));
  }
}

void SokobanActivity::handleMenu() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    mode = Mode::PLAY;
    fullRedraw = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Left)) {
    menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    requestUpdate();
  }
  if (mappedInput.wasPressed(B::Down) || mappedInput.wasPressed(B::Right)) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    requestUpdate();
  }
  if (!mappedInput.wasReleased(B::Confirm)) return;
  switch (menuIndex) {
    case MENU_HINT:
    case MENU_SOLUTION:
      requestHint(menuIndex == MENU_SOLUTION);
      break;
    case MENU_RESTART:
      startLevel(level);
      break;
    case MENU_LEVELS:
      selectIndex = level;
      mode = Mode::SELECT;
      cleanRequested = true;
      requestUpdate();
      break;
    case MENU_EXIT:
      saveProgress();
      finish();
      break;
    default:
      mode = Mode::PLAY;
      fullRedraw = true;
      requestUpdate();
      break;
  }
}

void SokobanActivity::handleSelect() {
  using B = MappedInputManager::Button;
  const int count = pack.count();
  if (mappedInput.wasReleased(B::Back)) {
    mode = Mode::PLAY;
    fullRedraw = true;
    cleanRequested = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    startLevel(selectIndex);
    return;
  }
  int next = selectIndex;
  if (mappedInput.wasPressed(B::Left)) next = (next + count - 1) % count;
  if (mappedInput.wasPressed(B::Right)) next = (next + 1) % count;
  if (mappedInput.wasPressed(B::Up)) next = next >= SELECT_COLS ? next - SELECT_COLS : next;
  if (mappedInput.wasPressed(B::Down)) next = std::min(count - 1, next + SELECT_COLS);
  if (next != selectIndex) {
    selectIndex = next;
    requestUpdate();
  }
}

void SokobanActivity::handleSolved() {
  using B = MappedInputManager::Button;
  const int rating = stars(board->moveCount());
  if (starsShown < rating) {
    if (mappedInput.wasAnyPressed()) {
      starsShown = rating;
      swallowRelease = true;  // the skipping press must not also act on release
      requestUpdate();
    } else if (millis() - revealMs >= STAR_STEP_MS) {
      starsShown++;
      revealMs = millis();
      requestUpdate();
    }
    return;
  }
  if (swallowRelease) {
    if (mappedInput.wasAnyReleased()) swallowRelease = false;
    return;
  }
  if (mappedInput.wasReleased(B::Back)) {
    undo();
  } else if (mappedInput.wasReleased(B::Confirm)) {
    startLevel(nextLevel());
  }
}

// ---- drawing ------------------------------------------------------------------------

void SokobanActivity::drawHeader() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_HEIGHT, false);
  char text[64];
  snprintf(text, sizeof(text), "%s %d / %d", tr(STR_SOKO_LEVEL), level + 1, pack.count());
  renderer.drawText(UI_12_FONT_ID, SIDE + 4, 10, text, true, EpdFontFamily::BOLD);
  if (mode == Mode::SOLUTION) {
    snprintf(text, sizeof(text), "%s %d / %d", tr(STR_SOKO_SOLUTION), hintShown ? hint.step + 1 : guide.pushes(),
             guide.pushes());
  } else {
    snprintf(text, sizeof(text), "%s", pack.builtIn() ? tr(STR_SOKO_BUILT_IN) : pack.name());
  }
  const bool boldRight = mode == Mode::SOLUTION;
  const int nameW =
      renderer.getTextWidth(UI_10_FONT_ID, text, boldRight ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, width - SIDE - 4 - nameW, 13, text, true,
                    boldRight ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  snprintf(text, sizeof(text), "%s %d   %s %d", tr(STR_SOKO_MOVES), board->moveCount(), tr(STR_SOKO_PUSHES),
           board->pushCount());
  renderer.drawText(UI_10_FONT_ID, SIDE + 4, 42, text);
  if (stuck && !board->solved()) {
    // A gentle note instead of par/best: the position can no longer be won.
    const char* note = tr(STR_SOKO_STUCK);
    const int noteW = renderer.getTextWidth(UI_10_FONT_ID, note, EpdFontFamily::BOLD) + 24;
    const int noteX = width - SIDE - noteW;
    renderer.fillRoundedRect(noteX, 38, noteW, 26, 13, Color::Black);
    renderer.drawText(UI_10_FONT_ID, noteX + 12, 40, note, false, EpdFontFamily::BOLD);
    renderer.drawLine(SIDE, HEADER_HEIGHT - 2, width - SIDE - 1, HEADER_HEIGHT - 2, 2, true);
    return;
  }
  char par[16] = "-", record[16] = "-";
  if (parMoves > 0) snprintf(par, sizeof(par), "%u", static_cast<unsigned>(parMoves));
  if (best[level] > 0) snprintf(record, sizeof(record), "%u", static_cast<unsigned>(best[level]));
  snprintf(text, sizeof(text), "%s %s   %s %s", tr(STR_SOKO_PAR), par, tr(STR_PLAY_BEST), record);
  const int statW = renderer.getTextWidth(UI_10_FONT_ID, text);
  renderer.drawText(UI_10_FONT_ID, width - SIDE - 4 - statW, 42, text);
  renderer.drawLine(SIDE, HEADER_HEIGHT - 2, width - SIDE - 1, HEADER_HEIGHT - 2, 2, true);
}

// Redraws tiles whose look changed since the last frame (all of them when `all`).
void SokobanActivity::drawBoard(const bool all) {
  const int w = board->width();
  const int total = w * board->height();
  for (int i = 0; i < total; i++) {
    const uint8_t look = tileLook(i);
    if (!all && shown[i] == look) continue;
    shown[i] = look;
    const uint8_t sides = (look & sokoban::CELL_WALL) ? board->openSides(i) : 0;
    sokoban::drawTile(renderer, boardX + (i % w) * tile, boardY + (i / w) * tile, tile, look, boardX, boardY, sides);
  }
}

void SokobanActivity::drawPlayHints() const {
  const int width = renderer.getScreenWidth();
  const int top = hintsTop(renderer);
  renderer.fillRect(0, top, width, renderer.getScreenHeight() - top, false);
  const char* back = tr(STR_SOKO_UNDO);
  const char* confirm = tr(STR_SOKO_MENU);
  const char* left = tr(STR_DIR_LEFT);
  const char* right = tr(STR_DIR_RIGHT);
  if (mode == Mode::MENU) {
    back = tr(STR_BACK);
    confirm = tr(STR_SELECT);
    left = tr(STR_DIR_UP);
    right = tr(STR_DIR_DOWN);
  } else if (mode == Mode::SOLVED) {
    confirm = tr(STR_SOKO_NEXT);
    left = right = "";
  } else if (mode == Mode::OFF_PATH) {
    back = tr(STR_CANCEL);
    confirm = tr(STR_SOKO_UNDO);
    left = right = "";
  } else if (mode == Mode::SOLUTION) {
    back = confirm = tr(STR_SOKO_DONE);
    left = tr(STR_SOKO_PREV);
    right = tr(STR_SOKO_NEXT);
  }
  const auto labels = mappedInput.mapLabels(back, confirm, left, right);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Side buttons walk up/down; label them only where the board leaves room.
  const int boardRight = boardX + board->width() * tile;
  const int boardBottom = boardY + board->height() * tile;
  const bool clear = boardX >= SIDE_HINT_ZONE && boardRight <= width - SIDE_HINT_ZONE;
  if (mode == Mode::PLAY && (clear || boardBottom < SIDE_HINT_TOP || boardY > SIDE_HINT_BOTTOM)) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
}

void SokobanActivity::drawCardFrame(const int x, const int y, const int width, const int height) const {
  renderer.fillRoundedRect(x, y, width, height, 12, Color::White);
  renderer.drawRoundedRect(x, y, width, height, 3, 12, true);
  renderer.drawRoundedRect(x + 6, y + 6, width - 12, height - 12, 1, 8, true);
}

void SokobanActivity::drawMenu() const {
  static constexpr StrId ITEMS[MENU_COUNT] = {StrId::STR_SOKO_RESUME,        StrId::STR_SOKO_HINT,
                                              StrId::STR_SOKO_SHOW_SOLUTION, StrId::STR_SOKO_RESTART,
                                              StrId::STR_SOKO_LEVELS,        StrId::STR_SOKO_EXIT};
  constexpr int ITEM_H = 52;
  const int width = renderer.getScreenWidth();
  const int cardW = std::min(380, width - 48);
  const int cardH = 76 + ITEM_H * MENU_COUNT + 56;
  const int x = (width - cardW) / 2;
  const int y = std::max(HEADER_HEIGHT + 8, (renderer.getScreenHeight() - cardH) / 2 - 20);
  drawCardFrame(x, y, cardW, cardH);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 22, tr(STR_SOKO_MENU), true, EpdFontFamily::BOLD);
  for (int i = 0; i < MENU_COUNT; i++) {
    const int iy = y + 64 + i * ITEM_H;
    const bool focused = i == menuIndex;
    if (focused) renderer.fillRoundedRect(x + 20, iy, cardW - 40, ITEM_H - 8, 8, Color::Black);
    renderer.drawCenteredText(UI_12_FONT_ID, iy + 10, I18N.get(ITEMS[i]), !focused,
                              focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, y + cardH - 56, tr(STR_SOKO_HOLD_HINT));
  renderer.drawCenteredText(SMALL_FONT_ID, y + cardH - 34, tr(STR_SOKO_HOLD_EXIT));
}

void SokobanActivity::drawSolvedCard() const {
  const int width = renderer.getScreenWidth();
  const int moves = board->moveCount();
  const int rating = stars(moves);
  const int cardW = std::min(400, width - 48);
  const bool hinted = hintsUsed > 0;
  const int cardH = (rating > 0 || hinted) ? 236 : 176;
  const int x = (width - cardW) / 2;
  const int boardMid = boardY + board->height() * tile / 2;
  const int y = std::clamp(boardMid - cardH / 2, HEADER_HEIGHT + 8, hintsTop(renderer) - cardH - 8);
  drawCardFrame(x, y, cardW, cardH);
  renderer.drawCenteredText(UI_12_FONT_ID, y + 24, tr(STR_SOKO_SOLVED), true, EpdFontFamily::BOLD);
  int line = y + 64;
  if (rating > 0) {
    for (int i = 0; i < 3; i++) sokoban::drawStar(renderer, width / 2 + (i - 1) * 64, line + 26, 24, i < starsShown);
    line += 66;
  } else if (hinted) {
    const char* note = tr(STR_SOKO_WITH_HINTS);
    const int noteW = renderer.getTextWidth(UI_10_FONT_ID, note);
    const int noteX = (width - noteW + 36) / 2;
    sokoban::drawBulb(renderer, noteX - 22, line + 24, 30, true);
    renderer.drawText(UI_10_FONT_ID, noteX, line + 14, note);
    line += 66;
  }
  char text[64];
  snprintf(text, sizeof(text), "%s %d   %s %d", tr(STR_SOKO_MOVES), moves, tr(STR_SOKO_PUSHES), board->pushCount());
  renderer.drawCenteredText(UI_10_FONT_ID, line + 6, text);
  if (newBest) {
    snprintf(text, sizeof(text), "%s", tr(STR_SOKO_NEW_BEST));
  } else {
    snprintf(text, sizeof(text), "%s %u", tr(STR_PLAY_BEST), static_cast<unsigned>(best[level]));
  }
  if (parMoves > 0) {
    const size_t used = strlen(text);
    snprintf(text + used, sizeof(text) - used, "   %s %u", tr(STR_SOKO_PAR), static_cast<unsigned>(parMoves));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, line + 36, text, true, EpdFontFamily::BOLD);
}

void SokobanActivity::drawSelect() const {
  const int width = renderer.getScreenWidth();
  const int count = pack.count();
  char text[64];
  renderer.drawText(UI_12_FONT_ID, SIDE + 4, 10, tr(STR_SOKO_LEVELS), true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), "%d / %d %s", solvedCount(), count, tr(STR_SOKO_SOLVED_COUNT));
  renderer.drawText(UI_10_FONT_ID, width - SIDE - 4 - renderer.getTextWidth(UI_10_FONT_ID, text), 13, text);
  renderer.drawLine(SIDE, 46, width - SIDE - 1, 46, 2, true);

  constexpr int GAP = 10;
  const int top = 60;
  const int footer = hintsTop(renderer) - 40;
  const int cellW = (width - 2 * SIDE - (SELECT_COLS - 1) * GAP) / SELECT_COLS;
  const int cellH = (footer - top - (SELECT_ROWS - 1) * GAP) / SELECT_ROWS;
  const int first = (selectIndex / SELECT_PER_PAGE) * SELECT_PER_PAGE;
  for (int k = 0; k < SELECT_PER_PAGE && first + k < count; k++) {
    const int i = first + k;
    const int x = SIDE + (k % SELECT_COLS) * (cellW + GAP);
    const int y = top + (k / SELECT_COLS) * (cellH + GAP);
    const bool focused = i == selectIndex;
    if (focused) {
      renderer.fillRoundedRect(x, y, cellW, cellH, 8, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, cellW, cellH, i == level ? 3 : 1, 8, true);
    }
    snprintf(text, sizeof(text), "%d", i + 1);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (cellW - tw) / 2, y + cellH / 2 - 22, text, !focused, EpdFontFamily::BOLD);
    if (best[i] == 0) continue;
    // Solved: a check mark under the number, or a bulb when hints did it.
    const int cx = x + cellW / 2, cy = y + cellH / 2 + 18;
    if (isHelped(i)) {
      sokoban::drawBulb(renderer, cx, cy + 2, 24, !focused);
      continue;
    }
    renderer.drawLine(cx - 9, cy, cx - 3, cy + 6, 3, !focused);
    renderer.drawLine(cx - 3, cy + 6, cx + 9, cy - 7, 3, !focused);
  }

  // Footer: the focused level's record and the page.
  const int pages = (count + SELECT_PER_PAGE - 1) / SELECT_PER_PAGE;
  char record[16] = "-";
  if (best[selectIndex] > 0) snprintf(record, sizeof(record), "%u", static_cast<unsigned>(best[selectIndex]));
  snprintf(text, sizeof(text), "%s %d   %s %s   %d/%d", tr(STR_SOKO_LEVEL), selectIndex + 1, tr(STR_PLAY_BEST), record,
           selectIndex / SELECT_PER_PAGE + 1, pages);
  renderer.drawCenteredText(UI_10_FONT_ID, footer + 8, text);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SokobanActivity::render(RenderLock&& lock) {
  if (!board || !shown || !best) return;
  bool clean = cleanRequested || movesSinceClean >= CLEAN_EVERY_MOVES;
  if (mode == Mode::SELECT) {
    renderer.clearScreen();
    drawSelect();
    fullRedraw = true;  // the board comes back from scratch
  } else {
    if (fullRedraw) {
      renderer.clearScreen();
      drawBoard(true);
    } else {
      drawBoard(false);
    }
    drawHeader();
    drawPlayHints();
    if (mode == Mode::MENU) drawMenu();
    if (mode == Mode::SOLVED) drawSolvedCard();
    if (mode == Mode::OFF_PATH) drawOffPathCard();
    fullRedraw = false;
  }
  if (clean) {
    cleanRequested = false;
    movesSinceClean = 0;
  }
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
