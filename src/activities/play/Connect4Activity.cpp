#include "Connect4Activity.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"

namespace {
using Button = MappedInputManager::Button;

constexpr const char* SAVE_DIR = "/.crosspoint/games";
constexpr const char* SAVE_PATH = "/.crosspoint/games/connect4.json";
constexpr size_t SAVE_BYTES = 192;  // longest file is ~175 bytes
constexpr int CENTRE = c4::COLS / 2;
constexpr uint16_t MAX_STAT = 65535;
// The computer's disc lands this long after its hover frame reached the
// panel (plus the landing refresh itself), or after the failsafe.
constexpr unsigned long HOVER_HOLD_MS = 150;
constexpr unsigned long HOVER_FAILSAFE_MS = 2500;

// Settings, then [won, lost, drawn] per level, then the game's columns.
#define C4_SAVE_HEAD "{\"level\":%d,\"starter\":%d,\"aiNext\":%d,\"gameLevel\":%d,\"first\":%d,\"stats\":["
constexpr const char* SAVE_FORMAT = C4_SAVE_HEAD "%u,%u,%u,%u,%u,%u,%u,%u,%u],\"moves\":\"%s\"}";
constexpr const char* SAVE_SCAN = C4_SAVE_HEAD "%hu,%hu,%hu,%hu,%hu,%hu,%hu,%hu,%hu],\"moves\":\"%42[0-6]";
#undef C4_SAVE_HEAD
constexpr int SAVE_FIELDS = 14;  // without the moves, which may be empty
}  // namespace

// ---- lifecycle & persistence -------------------------------------------------

void Connect4Activity::onEnter() {
  Activity::onEnter();
  layout();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  worker.start();
  load();
  requestUpdate();
}

void Connect4Activity::onExit() {
  cancelJob();
  worker.stop();
  saveNeeded = true;  // keep the game in progress
  save();
  Activity::onExit();
}

void Connect4Activity::load() {
  char buf[SAVE_BYTES] = {};
  char moves[c4::CELLS + 1] = {};
  int values[5] = {};
  auto& t = stats;
  if (!Storage.exists(SAVE_PATH) || Storage.readFileToBuffer(SAVE_PATH, buf, sizeof(buf) - 1) == 0 ||
      sscanf(buf, SAVE_SCAN, &values[0], &values[1], &values[2], &values[3], &values[4], &t[0][0], &t[0][1], &t[0][2],
             &t[1][0], &t[1][1], &t[1][2], &t[2][0], &t[2][1], &t[2][2], moves) < SAVE_FIELDS) {
    startGame();
    return;
  }
  level = std::clamp(values[0], 0, c4::LEVEL_COUNT - 1);
  starter = static_cast<uint8_t>(std::clamp(values[1], 0, STARTER_COUNT - 1));
  computerOpensNext = values[2] != 0;
  gameLevel = std::clamp(values[3], 0, c4::LEVEL_COUNT - 1);
  restoreGame(moves, values[4] == c4::COMPUTER ? c4::COMPUTER : c4::HUMAN);
}

void Connect4Activity::save() {
  if (!saveNeeded) return;
  char moves[c4::CELLS + 1];
  const int count = board.moveCount();
  for (int i = 0; i < count; i++) moves[i] = static_cast<char>('0' + board.moveAt(i));
  moves[count] = '\0';
  const auto& t = stats;
  char buf[SAVE_BYTES];
  snprintf(buf, sizeof(buf), SAVE_FORMAT, level, starter, computerOpensNext ? 1 : 0, gameLevel, board.firstSide(),
           t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2], t[2][0], t[2][1], t[2][2], moves);
  Storage.mkdir(SAVE_DIR);  // may already exist; a real failure surfaces in the write below
  if (!Storage.writeFile(SAVE_PATH, String(buf))) {
    LOG_ERR("C4", "Failed to write %s", SAVE_PATH);
    return;
  }
  saveNeeded = false;
}

// ---- game flow ---------------------------------------------------------------

void Connect4Activity::startGame() {
  cancelJob();
  gameLevel = level;
  c4::Side first = c4::HUMAN;
  if (starter == START_COMPUTER) {
    first = c4::COMPUTER;
  } else if (starter == START_ALTERNATE) {
    first = computerOpensNext ? c4::COMPUTER : c4::HUMAN;
    computerOpensNext = !computerOpensNext;
  }
  board.reset(first);
  cursor = CENTRE;
  hintColumn = aiColumn = -1;
  resultRecorded = false;
  settingsChanged = false;
  saveNeeded = true;
  save();  // settings and the alternating opener
  cleanRefresh = true;
  afterMove();
}

// Replays saved column digits (up to the first non-digit); an unreadable game
// is replaced by a new one.
void Connect4Activity::restoreGame(const char* moves, const c4::Side first) {
  board.reset(first);
  for (const char* p = moves; *p >= '0' && *p <= '9'; p++) {
    if (board.play(*p - '0') < 0) {
      LOG_ERR("C4", "Saved game is invalid, starting over");
      startGame();
      return;
    }
  }
  cursor = CENTRE;
  resultRecorded = board.over();  // counted when it ended
  cleanRefresh = true;
  afterMove();
}

// Decides whose turn it is after any change to the board.
void Connect4Activity::afterMove() {
  if (board.over()) {
    phase = Phase::OVER;
    recordResult();
  } else if (board.toMove() == c4::COMPUTER) {
    phase = Phase::THINKING;
    queueJob(Job::MOVE);
  } else {
    phase = Phase::HUMAN;
    if (!board.canPlay(cursor)) cursor = firstPlayableFrom(cursor, 1);
  }
  requestUpdate();
}

void Connect4Activity::recordResult() {
  if (resultRecorded) return;
  resultRecorded = true;
  const int winner = board.winner();
  const Result result = winner == c4::HUMAN ? WON : winner == c4::COMPUTER ? LOST : DRAWN;
  uint16_t& tally = stats[gameLevel][result];
  if (tally < MAX_STAT) tally++;
  saveNeeded = true;
  save();
}

// ---- background search ---------------------------------------------------------

void Connect4Activity::queueJob(const Job job) { pendingJob = job; }

void Connect4Activity::cancelJob() {
  if (ticket != 0) worker.cancel();
  ticket = 0;
  runningJob = Job::NONE;
  pendingJob = Job::NONE;
}

// Collects a finished search and starts the queued one once the task is idle
// (a cancelled search may still be winding down).
void Connect4Activity::pumpWorker() {
  int column = -1;
  if (ticket != 0 && worker.take(ticket, column)) {
    ticket = 0;
    const Job done = runningJob;
    runningJob = Job::NONE;
    if (done == Job::MOVE && phase == Phase::THINKING) {
      onSearchDone(column);
    } else if (done == Job::HINT && phase == Phase::HUMAN) {
      hintColumn = column;
      if (column >= 0) cursor = column;
      requestUpdate();
    }
  }
  if (pendingJob == Job::NONE || ticket != 0 || worker.busy()) return;
  const c4::Level& spec = pendingJob == Job::HINT ? c4::HINT_LEVEL : c4::LEVELS[gameLevel];
  ticket = worker.post(board.position(), spec, c4::nextRandom(rng));
  if (ticket != 0) {
    runningJob = pendingJob;
    pendingJob = Job::NONE;
  }
}

void Connect4Activity::onSearchDone(int column) {
  if (!board.canPlay(column)) {
    LOG_ERR("C4", "Search returned column %d", column);
    column = firstPlayableFrom(CENTRE, 1);
    if (column < 0) return;
  }
  aiColumn = column;
  phase = Phase::AI_HOVER;
  hoverId++;
  hoverStartMs = millis();
  requestUpdate();
}

void Connect4Activity::landComputerDisc() {
  board.play(aiColumn);
  aiColumn = -1;
  afterMove();
}

// ---- input -------------------------------------------------------------------

void Connect4Activity::loop() {
  RenderLock lock(*this);
  pumpWorker();
  if (menuOpen) {
    handleMenu(lock);
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    openMenu();
    return;
  }
  handleBoard();
}

void Connect4Activity::handleBoard() {
  if (phase == Phase::OVER) {
    if (mappedInput.wasReleased(Button::Confirm)) startGame();
    return;
  }
  if (mappedInput.wasPressed(Button::Up)) {
    undo();
    return;
  }
  bool pressed = false;
  if (mappedInput.wasPressed(Button::Left)) {
    moveCursor(-1);
    pressed = true;
  }
  if (mappedInput.wasPressed(Button::Right)) {
    moveCursor(1);
    pressed = true;
  }
  if (phase == Phase::AI_HOVER) {
    pressed = pressed || mappedInput.wasReleased(Button::Confirm) || mappedInput.wasPressed(Button::Down);
    const bool shown = presentedHover.load() == hoverId && millis() - presentedAtMs.load() >= HOVER_HOLD_MS;
    if (pressed || shown || millis() - hoverStartMs >= HOVER_FAILSAFE_MS) landComputerDisc();
    return;
  }
  if (phase != Phase::HUMAN) return;
  if (mappedInput.wasPressed(Button::Down)) requestHint();
  if (mappedInput.wasReleased(Button::Confirm)) dropHuman();
}

int Connect4Activity::firstPlayableFrom(const int col, const int direction) const {
  for (int i = 0; i < c4::COLS; i++) {
    const int c = ((col + direction * i) % c4::COLS + c4::COLS) % c4::COLS;
    if (board.canPlay(c)) return c;
  }
  return -1;
}

int Connect4Activity::humanMoves() const {
  const int count = board.moveCount();
  return board.firstSide() == c4::HUMAN ? (count + 1) / 2 : count / 2;
}

void Connect4Activity::moveCursor(const int direction) {
  const int next = firstPlayableFrom(cursor + direction, direction);
  if (next < 0 || next == cursor) return;
  cursor = next;
  requestUpdate();
}

void Connect4Activity::dropHuman() {
  if (!board.canPlay(cursor)) return;
  cancelJob();  // a hint still being searched
  board.play(cursor);
  hintColumn = -1;
  afterMove();
}

// Takes back the human's last disc and, when it has already answered, the
// computer's reply, so it is the human's turn again.
void Connect4Activity::undo() {
  if (phase == Phase::HUMAN) {
    if (board.moveCount() < 2) return;
    cancelJob();
    board.undo();
  } else {
    if (board.moveCount() == 0) return;  // the computer is still opening
    cancelJob();
    aiColumn = -1;
  }
  cursor = board.lastColumn();
  board.undo();
  hintColumn = -1;
  afterMove();
}

void Connect4Activity::requestHint() {
  if (runningJob == Job::HINT || pendingJob == Job::HINT) return;
  hintColumn = -1;
  queueJob(Job::HINT);
  requestUpdate();
}

// ---- menu --------------------------------------------------------------------

void Connect4Activity::openMenu() {
  menuOpen = true;
  menuIndex = ITEM_NEW;
  requestUpdate();
}

// Settings changed before the human's first disc restart the game with them;
// otherwise they wait for the next game.
void Connect4Activity::closeMenu() {
  menuOpen = false;
  cleanRefresh = true;
  if (settingsChanged) {
    settingsChanged = false;
    saveNeeded = true;
    if (humanMoves() == 0 && phase != Phase::OVER) {
      computerOpensNext = board.firstSide() == c4::COMPUTER;  // an alternating opener keeps its turn
      startGame();                                            // saves
      return;
    }
    save();
  }
  requestUpdate();
}

void Connect4Activity::handleMenu(RenderLock& lock) {
  if (mappedInput.wasReleased(Button::Back)) {
    closeMenu();
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
  switch (menuIndex) {
    case ITEM_NEW:
      menuOpen = false;
      startGame();
      break;
    case ITEM_LEVEL:
      level = (level + 1) % c4::LEVEL_COUNT;
      settingsChanged = true;
      requestUpdate();
      break;
    case ITEM_STARTER:
      starter = static_cast<uint8_t>((starter + 1) % STARTER_COUNT);
      settingsChanged = true;
      requestUpdate();
      break;
    default:
      lock.unlock();
      finish();
      break;
  }
}
