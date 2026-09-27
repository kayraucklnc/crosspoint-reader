#include "ConnectionsActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "ConnectionsDaily.h"
#include "MappedInputManager.h"
#include "network/NetworkTime.h"

namespace {
using B = MappedInputManager::Button;

constexpr char DIR[] = "/.crosspoint/games/connections";
constexpr char PREFS_PATH[] = "/.crosspoint/games/connections/prefs.txt";
constexpr int SAVE_VERSION = 4;
constexpr int PREFS_VERSION = 1;

// Per language: pack, results, board, launch day (daily #1) and the salt that
// makes its daily order differ from the other language's.
struct LangFiles {
  const char* index;
  const char* data;
  const char* results;
  const char* resultsTmp;
  const char* save;
  uint32_t launch;
  uint32_t salt;
};
constexpr LangFiles FILES[] = {
    {"/.crosspoint/games/connections/connections.idx", "/.crosspoint/games/connections/connections.dat",
     "/.crosspoint/games/connections/results_en.bin", "/.crosspoint/games/connections/results_en.tmp",
     "/.crosspoint/games/connections/save_en.txt", 20260101, 0x454E3136u},
    {"/.crosspoint/games/connections/connections_tr.idx", "/.crosspoint/games/connections/connections_tr.dat",
     "/.crosspoint/games/connections/results_tr.bin", "/.crosspoint/games/connections/results_tr.tmp",
     "/.crosspoint/games/connections/save_tr.txt", 20260901, 0x54523236u},
};

constexpr unsigned long HOLD_MS = 600;
constexpr unsigned long REPEAT_DELAY_MS = 400;
constexpr unsigned long REPEAT_MS = 220;
constexpr unsigned long FINALE_MS = 1600;

bool readAt(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
  auto* file = static_cast<HalFile*>(ctx);
  if (!file->seek(offset)) return false;
  return file->read(dst, len) == static_cast<int>(len);
}
}  // namespace

// ---- lifecycle ----------------------------------------------------------------

void ConnectionsActivity::onEnter() {
  Activity::onEnter();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  results = makeUniqueNoThrow<uint8_t[]>(MAX_RESULTS);
  if (!results) LOG_ERR("CONN", "OOM: results %d bytes", MAX_RESULTS);
  loadPrefs();
  openLanguage();
  showScreen(Screen::HOME);
}

void ConnectionsActivity::onExit() {
  if (gameLoaded) saveGame();
  closePack();
  Activity::onExit();
}

// ---- language, pack and results --------------------------------------------------

// "version lang lastSeenDate". A first run follows the UI language.
void ConnectionsActivity::loadPrefs() {
  lang = I18N.getLanguage() == Language::TR ? LANG_TR : LANG_EN;
  char buffer[48] = {};
  if (!Storage.exists(PREFS_PATH) || Storage.readFileToBuffer(PREFS_PATH, buffer, sizeof(buffer) - 1) == 0) return;
  int version = 0;
  unsigned saved = 0;
  unsigned long seen = 0;
  if (sscanf(buffer, "%d %u %lu", &version, &saved, &seen) != 3 || version != PREFS_VERSION) return;
  if (saved < LANG_COUNT) lang = static_cast<Lang>(saved);
  lastSeenDate = connections::dayOfWeek(static_cast<uint32_t>(seen)) >= 0 ? static_cast<uint32_t>(seen) : 0;
}

void ConnectionsActivity::savePrefs() const {
  char line[48];
  const int length = snprintf(line, sizeof(line), "%d %u %lu", PREFS_VERSION, static_cast<unsigned>(lang),
                              static_cast<unsigned long>(lastSeenDate));
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  if (!Storage.openFileForWrite("CONN", PREFS_PATH, file) || file.write(line, length) != static_cast<size_t>(length)) {
    LOG_ERR("CONN", "Failed to save prefs");
  }
}

void ConnectionsActivity::switchLanguage(const Lang next) {
  if (next == lang) return;
  if (gameLoaded) saveGame();
  gameLoaded = false;
  lang = next;
  savePrefs();
  openLanguage();
  cleanNext = true;
  requestUpdate();
}

void ConnectionsActivity::openLanguage() {
  gameLoaded = false;
  openPack();
  refreshToday();
  loadResults();
  restoreSavedGame();
  archiveDate = today;
}

bool ConnectionsActivity::openPack() {
  closePack();
  const LangFiles& files = FILES[lang];
  if (!Storage.exists(files.index) || !Storage.exists(files.data)) return false;
  if (!Storage.openFileForRead("CONN", files.index, indexFile) ||
      !Storage.openFileForRead("CONN", files.data, dataFile)) {
    closePack();
    return false;
  }
  packOpen = pack.open(readAt, &indexFile, &dataFile, static_cast<uint32_t>(indexFile.fileSize()));
  if (!packOpen) {
    LOG_ERR("CONN", "Unreadable puzzle pack %s", files.index);
    closePack();
    return false;
  }
  LOG_DBG("CONN", "Pack %s: %d puzzles", files.index, pack.count());
  return true;
}

void ConnectionsActivity::closePack() {
  packOpen = false;
  indexFile.close();
  dataFile.close();
}

void ConnectionsActivity::loadResults() {
  resultsLength = 0;
  summary = connections::Summary{};
  if (!results) return;
  std::memset(results.get(), 0, MAX_RESULTS);
  HalFile file;
  if (Storage.openFileForRead("CONN", FILES[lang].results, file)) {
    const int read = file.read(results.get(), MAX_RESULTS);
    resultsLength = read > 0 ? read : 0;
  }
  summary = connections::summarise(results.get(), resultsLength, dayIndex(today));
}

connections::Record ConnectionsActivity::resultFor(const uint32_t date) const {
  const int index = dayIndex(date);
  if (!results || index < 0 || index >= resultsLength) return {};
  return connections::decodeRecord(results[index]);
}

// Rewritten whole through a temp file: it is every score kept, and a torn write must not lose it.
void ConnectionsActivity::writeResult(const uint32_t date, const connections::Record& record) {
  const int index = dayIndex(date);
  if (!results || index < 0 || index >= MAX_RESULTS) return;
  results[index] = connections::encodeRecord(record);
  if (index >= resultsLength) resultsLength = index + 1;
  summary = connections::summarise(results.get(), resultsLength, dayIndex(today));

  const LangFiles& files = FILES[lang];
  Storage.ensureDirectoryExists(DIR);
  HalFile out;
  if (!Storage.openFileForWrite("CONN", files.resultsTmp, out)) return;
  const bool ok = out.write(results.get(), resultsLength) == static_cast<size_t>(resultsLength);
  out.close();
  if (!ok) {
    LOG_ERR("CONN", "Failed to write results");
    Storage.remove(files.resultsTmp);
    return;
  }
  Storage.remove(files.results);
  Storage.rename(files.resultsTmp, files.results);
}

// The local date when the clock is known; otherwise the last date seen, so an
// unset clock keeps showing the latest daily instead of jumping back.
void ConnectionsActivity::refreshToday() {
  const time_t now = network_time::current();
  clockKnown = now >= network_time::MIN_VALID_EPOCH;
  if (clockKnown) {
    struct tm local{};
    localtime_r(&now, &local);
    const auto date = static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
    today = date;
    if (date != lastSeenDate) {
      lastSeenDate = date;
      savePrefs();
    }
  } else {
    today = lastSeenDate;
  }
  if (dayIndex(today) < 0) today = launchDate();
  if (results) summary = connections::summarise(results.get(), resultsLength, dayIndex(today));
}

uint32_t ConnectionsActivity::launchDate() const { return FILES[lang].launch; }

int ConnectionsActivity::dayIndex(const uint32_t date) const {
  return date == 0 ? -1 : connections::daysBetween(launchDate(), date);
}

// Past dailies (before today) not played yet: what Random draws from.
int ConnectionsActivity::unplayedCount() const {
  const int past = dayIndex(today);
  int played = 0;
  for (int i = 0; i < past && i < resultsLength; i++) {
    if (connections::decodeRecord(results[i]).played) played++;
  }
  return past > played ? past - played : 0;
}

// ---- game -----------------------------------------------------------------------

// The daily of `date`, stamped with that date: results, the save and the
// header all key on it.
bool ConnectionsActivity::readDaily(const uint32_t date, connections::Puzzle& out) const {
  const int day = dayIndex(date);
  if (!packOpen || day < 0) return false;
  const int index = connections::dailyPuzzle(static_cast<uint32_t>(day), pack.count(), FILES[lang].salt);
  if (!pack.readPuzzle(index, out) || !connections::isPlayable(out)) {
    LOG_ERR("CONN", "Could not read puzzle %d", index);
    return false;
  }
  out.date = date;
  return true;
}

bool ConnectionsActivity::openDaily(const uint32_t date) {
  connections::Puzzle puzzle;
  if (!readDaily(date, puzzle)) return false;
  if (!loadGame(puzzle)) {
    // Seeded from the puzzle, so a puzzle always deals the same board.
    game.start(puzzle, 0x9E3779B9u ^ (static_cast<uint32_t>(puzzle.id) * 2654435761u));
  }
  gameLoaded = true;
  recorded = resultFor(date).played;
  replay = recorded && game.result() == connections::Result::Playing;
  cursorRow = 0;
  cursorCol = 0;
  toast = Toast::NONE;
  finaleStage = 0;
  saveGame();
  showScreen(game.result() == connections::Result::Playing ? Screen::GAME : Screen::RESULT);
  return true;
}

// Brings back the language's last board so the home can show its progress.
void ConnectionsActivity::restoreSavedGame() {
  char buffer[24] = {};
  const char* path = FILES[lang].save;
  if (!Storage.exists(path) || Storage.readFileToBuffer(path, buffer, sizeof(buffer) - 1) == 0) return;
  int version = 0;
  unsigned long date = 0;
  if (sscanf(buffer, "%d %lu", &version, &date) != 2 || version != SAVE_VERSION) return;
  const int day = dayIndex(static_cast<uint32_t>(date));
  if (day < 0 || day > dayIndex(today)) return;
  connections::Puzzle puzzle;
  if (!readDaily(static_cast<uint32_t>(date), puzzle) || !loadGame(puzzle)) return;
  gameLoaded = true;
  recorded = resultFor(puzzle.date).played;
  replay = recorded && game.result() == connections::Result::Playing;
}

void ConnectionsActivity::saveGame() const {
  if (!gameLoaded) return;
  const connections::Game::Save state = game.save();
  char line[160];
  int at = snprintf(line, sizeof(line), "%d %lu %u %lu %u %u %u %u %u %u %u", SAVE_VERSION,
                    static_cast<unsigned long>(game.puzzle().date), game.puzzle().id,
                    static_cast<unsigned long>(state.seed), state.solvedCount, state.mistakes, state.solvedOrder[0],
                    state.solvedOrder[1], state.solvedOrder[2], state.solvedOrder[3], state.guessCount);
  for (int i = 0; i < state.guessCount && at > 0 && at < static_cast<int>(sizeof(line)) - 8; i++) {
    at += snprintf(line + at, sizeof(line) - at, " %u", state.guessed[i]);
  }
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  if (!Storage.openFileForWrite("CONN", FILES[lang].save, file) || file.write(line, at) != static_cast<size_t>(at)) {
    LOG_ERR("CONN", "Failed to save the board");
  }
}

// Restores the saved board when it belongs to `puzzle` (same day, same pack puzzle).
bool ConnectionsActivity::loadGame(const connections::Puzzle& puzzle) {
  char buffer[160] = {};
  const char* path = FILES[lang].save;
  if (!Storage.exists(path) || Storage.readFileToBuffer(path, buffer, sizeof(buffer) - 1) == 0) return false;
  int version = 0, consumed = 0;
  unsigned long date = 0, seed = 0;
  unsigned id = 0, solved = 0, mistakes = 0, order[4] = {}, guesses = 0;
  if (sscanf(buffer, "%d %lu %u %lu %u %u %u %u %u %u %u%n", &version, &date, &id, &seed, &solved, &mistakes, &order[0],
             &order[1], &order[2], &order[3], &guesses, &consumed) != 11) {
    return false;
  }
  if (version != SAVE_VERSION || date != puzzle.date || id != puzzle.id) return false;
  connections::Game::Save state;
  state.seed = static_cast<uint32_t>(seed);
  state.solvedCount = static_cast<uint8_t>(solved);
  state.mistakes = static_cast<uint8_t>(mistakes);
  for (int i = 0; i < connections::kGroups; i++) state.solvedOrder[i] = static_cast<uint8_t>(order[i]);
  state.guessCount = static_cast<uint8_t>(guesses > 8 ? 8 : guesses);
  const char* at = buffer + consumed;
  for (int i = 0; i < state.guessCount; i++) {
    unsigned mask = 0;
    int used = 0;
    if (sscanf(at, " %u%n", &mask, &used) != 1) return false;
    state.guessed[i] = static_cast<uint16_t>(mask);
    at += used;
  }
  game.start(puzzle, state.seed);
  return game.restore(state);
}

void ConnectionsActivity::finishGame() {
  if (!replay && !recorded) {
    connections::Record record;
    record.played = true;
    record.lost = game.result() == connections::Result::Lost;
    record.mistakes = static_cast<uint8_t>(game.mistakes());
    record.hardestFirst = !record.lost && game.solvedGroup(0).level == 3;
    writeResult(game.puzzle().date, record);
    recorded = true;
  }
  finaleStage = 1;
  finaleDueMs = millis() + FINALE_MS;
}

void ConnectionsActivity::focusAction(const Action action) {
  cursorRow = boardRows();
  cursorCol = action;
}

void ConnectionsActivity::submitSelection() {
  if (game.selectedCount() != connections::kMembers) {
    toast = Toast::NEED_FOUR;
    requestUpdate();
    return;
  }
  switch (game.submit()) {
    case connections::Guess::Solved:
      toast = Toast::NONE;
      cursorRow = 0;
      cursorCol = 0;
      break;
    case connections::Guess::OneAway:
      toast = Toast::ONE_AWAY;
      break;
    case connections::Guess::Wrong:
      toast = Toast::WRONG;
      break;
    case connections::Guess::AlreadyGuessed:
      toast = Toast::REPEAT;
      break;
    case connections::Guess::Invalid:
      toast = Toast::NEED_FOUR;
      break;
  }
  if (game.result() != connections::Result::Playing) finishGame();
  saveGame();
  cleanNext = cleanNext || game.result() != connections::Result::Playing;
  requestUpdate();
}

// Shuffle moves the words, not the choice: the selected words stay selected.
void ConnectionsActivity::shuffleKeepingSelection() {
  const char* chosen[connections::kMembers] = {};
  int count = 0;
  for (int i = 0; i < game.tileCount() && count < connections::kMembers; i++) {
    if (game.isSelected(i)) chosen[count++] = game.tileWord(i);
  }
  game.shuffle(game.seed() * 1103515245u + 12345u);
  for (int i = 0; i < game.tileCount(); i++) {
    for (int k = 0; k < count; k++) {
      if (game.tileWord(i) == chosen[k]) game.toggleTile(i);
    }
  }
  saveGame();
}

void ConnectionsActivity::pressConfirm() {
  toast = Toast::NONE;
  if (cursorRow >= boardRows()) {
    switch (cursorCol) {
      case ACT_SHUFFLE:
        shuffleKeepingSelection();
        break;
      case ACT_DESELECT:
        game.deselectAll();
        break;
      default:
        submitSelection();
        return;
    }
    requestUpdate();
    return;
  }
  const int tile = cursorTile();
  if (!game.isSelected(tile) && game.selectedCount() >= connections::kMembers) {
    toast = Toast::FULL;
  } else {
    game.toggleTile(tile);
    if (game.selectedCount() == connections::kMembers && game.isSelected(tile)) focusAction(ACT_SUBMIT);
  }
  requestUpdate();
}

// Rows are the unsolved tile rows plus the action row; vertical moves keep the
// horizontal position (four tile columns, three actions).
void ConnectionsActivity::moveCursor(const int dCol, const int dRow) {
  const int rows = boardRows();
  const int totalRows = rows + 1;
  toast = Toast::NONE;
  if (dRow != 0) {
    const bool onActions = cursorRow >= rows;
    const int center = onActions ? (2 * cursorCol + 1) * 4 : (2 * cursorCol + 1) * 3;  // in 24ths of the width
    cursorRow = (cursorRow + dRow + totalRows) % totalRows;
    cursorCol = cursorRow >= rows ? center / 8 : center / 6;
  }
  if (dCol != 0) {
    const int count = cursorRow >= rows ? ACTION_COUNT : connections::kMembers;
    cursorCol = (cursorCol + dCol + count) % count;
  }
  requestUpdate();
}

// ---- navigation -------------------------------------------------------------------

void ConnectionsActivity::showScreen(const Screen next) {
  screen = next;
  cleanNext = true;
  requestUpdate();
}

// A random past daily not played yet; a random past one to replay when all
// are done; today's when there is no past yet.
void ConnectionsActivity::openRandom() {
  const int past = dayIndex(today);
  if (past <= 0) {
    openDaily(today);
    return;
  }
  const int start = static_cast<int>(rng % static_cast<uint32_t>(past));
  rng = rng * 1664525u + 1013904223u;
  for (int step = 0; step < past; step++) {
    const int day = (start + step) % past;
    if (day >= resultsLength || !connections::decodeRecord(results[day]).played) {
      openDaily(connections::addDays(launchDate(), day));
      return;
    }
  }
  openDaily(connections::addDays(launchDate(), start));
}

void ConnectionsActivity::openHomeItem() {
  if (homeSelected == HOME_LANG) {
    switchLanguage(lang == LANG_EN ? LANG_TR : LANG_EN);
    return;
  }
  refreshToday();
  if (homeSelected == HOME_STATS) {
    showScreen(Screen::STATS);
    return;
  }
  if (!packOpen) {
    requestUpdate();  // the entries already say the pack is missing
    return;
  }
  switch (homeSelected) {
    case HOME_TODAY:
      openDaily(today);
      break;
    case HOME_RANDOM:
      openRandom();
      break;
    default: {
      const uint32_t current = gameLoaded ? game.puzzle().date : 0;
      archiveDate = dayIndex(current) >= 0 && current <= today ? current : today;
      showScreen(Screen::ARCHIVE);
      break;
    }
  }
}

bool ConnectionsActivity::repeatPress(const B button) {
  if (mappedInput.wasPressed(button)) {
    heldButton = button;
    nextRepeatMs = millis() + REPEAT_DELAY_MS;
    return true;
  }
  if (heldButton == button && mappedInput.isPressed(button) && millis() >= nextRepeatMs) {
    nextRepeatMs = millis() + REPEAT_MS;
    return true;
  }
  return false;
}

void ConnectionsActivity::handleHome() {
  if (mappedInput.wasReleased(B::Confirm)) {
    openHomeItem();
    return;
  }
  if (mappedInput.wasPressed(B::NavPrevious)) {
    homeSelected = (homeSelected + HOME_ITEMS - 1) % HOME_ITEMS;
    requestUpdate();
  } else if (mappedInput.wasPressed(B::NavNext)) {
    homeSelected = (homeSelected + 1) % HOME_ITEMS;
    requestUpdate();
  }
}

void ConnectionsActivity::handleGame() {
  if (finaleStage != 0) {
    if (mappedInput.wasAnyPressed() || millis() >= finaleDueMs) {
      finaleStage = 0;
      showScreen(Screen::RESULT);
    }
    return;
  }
  if (mappedInput.wasReleased(B::Back)) {
    saveGame();
    showScreen(Screen::HOME);
    return;
  }
  if (mappedInput.wasLongPressed(B::Confirm, HOLD_MS)) {
    submitSelection();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    pressConfirm();
    return;
  }
  if (repeatPress(B::Left)) moveCursor(-1, 0);
  if (repeatPress(B::Right)) moveCursor(+1, 0);
  if (repeatPress(B::Up)) moveCursor(0, -1);
  if (repeatPress(B::Down)) moveCursor(0, +1);
}

void ConnectionsActivity::handleResult() {
  if (mappedInput.wasReleased(B::Back)) {
    showScreen(Screen::HOME);
  } else if (mappedInput.wasReleased(B::Confirm)) {
    showScreen(Screen::STATS);
  }
}

void ConnectionsActivity::handleArchive() {
  if (mappedInput.wasReleased(B::Back)) {
    showScreen(Screen::HOME);
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    openDaily(archiveDate);
    return;
  }
  int delta = 0;
  if (repeatPress(B::Left)) delta = -1;
  if (repeatPress(B::Right)) delta = +1;
  if (repeatPress(B::Up)) delta = -7;
  if (repeatPress(B::Down)) delta = +7;
  if (delta == 0) return;
  const uint32_t next = connections::addDays(archiveDate, delta);
  if (next == 0) return;
  const uint32_t first = launchDate();
  const uint32_t clamped = next < first ? first : next > today ? today : next;
  if (clamped == archiveDate) return;
  // A month change redraws most of the screen: clean it.
  if (clamped / 100 != archiveDate / 100) cleanNext = true;
  archiveDate = clamped;
  requestUpdate();
}

void ConnectionsActivity::handleStats() {
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) showScreen(Screen::HOME);
}

void ConnectionsActivity::loop() {
  RenderLock lock(*this);
  if (screen == Screen::HOME && mappedInput.wasReleased(B::Back)) {
    lock.unlock();
    finish();
    return;
  }
  switch (screen) {
    case Screen::HOME:
      handleHome();
      break;
    case Screen::GAME:
      handleGame();
      break;
    case Screen::RESULT:
      handleResult();
      break;
    case Screen::ARCHIVE:
      handleArchive();
      break;
    case Screen::STATS:
      handleStats();
      break;
  }
}
