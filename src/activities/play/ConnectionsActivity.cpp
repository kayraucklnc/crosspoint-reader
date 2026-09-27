#include "ConnectionsActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "ConnectionsImport.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "network/HttpDownloader.h"
#include "network/NetworkTime.h"
#include "network/WifiAutoConnect.h"

namespace {
using B = MappedInputManager::Button;

constexpr char DIR[] = "/.crosspoint/games/connections";
constexpr char INDEX_PATH[] = "/.crosspoint/games/connections/connections.idx";
constexpr char DATA_PATH[] = "/.crosspoint/games/connections/connections.dat";
constexpr char INDEX_TMP[] = "/.crosspoint/games/connections/connections.idx.tmp";
constexpr char DATA_TMP[] = "/.crosspoint/games/connections/connections.dat.tmp";
constexpr char RESULTS_PATH[] = "/.crosspoint/games/connections/results.bin";
constexpr char RESULTS_TMP[] = "/.crosspoint/games/connections/results.tmp";
constexpr char SAVE_PATH[] = "/.crosspoint/games/connections/save.txt";
constexpr int SAVE_VERSION = 3;
// The public archive mirror (every puzzle since 2023 in one ~1.4 MB file).
constexpr char ARCHIVE_URL[] =
    "https://raw.githubusercontent.com/Eyefyre/NYT-Connections-Answers/main/connections.json";

constexpr unsigned long HOLD_MS = 600;
constexpr unsigned long REPEAT_DELAY_MS = 400;
constexpr unsigned long REPEAT_MS = 220;
constexpr unsigned long FINALE_MS = 1600;

bool readAt(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
  auto* file = static_cast<HalFile*>(ctx);
  if (!file->seek(offset)) return false;
  return file->read(dst, len) == static_cast<int>(len);
}

// Import sink: the pack is streamed straight into two files.
struct Sink {
  HalFile file;
  bool failed = false;
};

bool writeTo(void* ctx, const void* src, const uint32_t len) {
  auto* sink = static_cast<Sink*>(ctx);
  if (sink->file.write(src, len) != len) sink->failed = true;
  return !sink->failed;
}

struct ImportContext {
  connections::PackWriter writer;
  Sink index;
  Sink data;
};

bool onImportedPuzzle(void* ctx, const connections::Puzzle& puzzle) {
  auto* import = static_cast<ImportContext*>(ctx);
  // A puzzle the writer rejects (out of order) is skipped; only a failed write stops the import.
  import->writer.add(puzzle);
  return !import->index.failed && !import->data.failed;
}
}  // namespace

// ---- lifecycle ----------------------------------------------------------------

void ConnectionsActivity::onEnter() {
  Activity::onEnter();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  results = makeUniqueNoThrow<uint8_t[]>(MAX_RESULTS);
  if (!results) LOG_ERR("CONN", "OOM: results %d bytes", MAX_RESULTS);
  openPack();
  loadResults();
  refreshToday();
  showScreen(Screen::HOME);
}

void ConnectionsActivity::onExit() {
  if (gameLoaded) saveGame();
  closePack();
  Activity::onExit();
  // Like the other WiFi screens, reboot home to reclaim heap the TLS stack fragmented.
  if (wifiStartedHere) {
    stopWifi();
    silentRestart();
  }
}

// ---- pack and results -----------------------------------------------------------

bool ConnectionsActivity::openPack() {
  closePack();
  if (!Storage.exists(INDEX_PATH) || !Storage.exists(DATA_PATH)) return false;
  if (!Storage.openFileForRead("CONN", INDEX_PATH, indexFile) ||
      !Storage.openFileForRead("CONN", DATA_PATH, dataFile)) {
    closePack();
    return false;
  }
  packOpen = pack.open(readAt, &indexFile, &dataFile, static_cast<uint32_t>(indexFile.fileSize()));
  if (!packOpen) {
    LOG_ERR("CONN", "Unreadable puzzle pack");
    closePack();
    return false;
  }
  pack.dateAt(0, firstDate);
  pack.dateAt(pack.count() - 1, lastDate);
  LOG_DBG("CONN", "Pack: %d puzzles %lu..%lu", pack.count(), static_cast<unsigned long>(firstDate),
          static_cast<unsigned long>(lastDate));
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
  if (Storage.openFileForRead("CONN", RESULTS_PATH, file)) {
    const int read = file.read(results.get(), MAX_RESULTS);
    resultsLength = read > 0 ? read : 0;
  }
  summary = connections::summarise(results.get(), resultsLength, connections::resultIndex(lastDate));
}

connections::Record ConnectionsActivity::resultFor(const uint32_t date) const {
  const int index = connections::resultIndex(date);
  if (!results || index < 0 || index >= resultsLength) return {};
  return connections::decodeRecord(results[index]);
}

// Rewritten whole through a temp file: it is every score kept, and a torn write must not lose it.
void ConnectionsActivity::writeResult(const uint32_t date, const connections::Record& record) {
  const int index = connections::resultIndex(date);
  if (!results || index < 0 || index >= MAX_RESULTS) return;
  results[index] = connections::encodeRecord(record);
  if (index >= resultsLength) resultsLength = index + 1;
  summary = connections::summarise(results.get(), resultsLength, connections::resultIndex(lastDate));

  Storage.ensureDirectoryExists(DIR);
  HalFile out;
  if (!Storage.openFileForWrite("CONN", RESULTS_TMP, out)) return;
  const bool ok = out.write(results.get(), resultsLength) == static_cast<size_t>(resultsLength);
  out.close();
  if (!ok) {
    LOG_ERR("CONN", "Failed to write results");
    Storage.remove(RESULTS_TMP);
    return;
  }
  Storage.remove(RESULTS_PATH);
  Storage.rename(RESULTS_TMP, RESULTS_PATH);
}

void ConnectionsActivity::refreshToday() {
  const time_t now = network_time::current();
  if (now == 0) {
    today = 0;
    return;
  }
  struct tm local{};
  localtime_r(&now, &local);
  today = static_cast<uint32_t>((local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday);
}

// Today's puzzle, or the newest one held when the pack lags or the clock is unknown.
int ConnectionsActivity::todayIndex() const {
  if (!packOpen) return -1;
  if (today == 0) return pack.count() - 1;
  const int index = pack.indexOnOrBefore(today);
  return index >= 0 ? index : pack.count() - 1;
}

int ConnectionsActivity::unplayedCount() const {
  if (!packOpen) return 0;
  const int span = connections::daysBetween(firstDate, lastDate) + 1;
  int played = 0;
  const int first = connections::resultIndex(firstDate);
  for (int i = 0; i < span && first + i < resultsLength; i++) {
    if (connections::decodeRecord(results[first + i]).played) played++;
  }
  return pack.count() > played ? pack.count() - played : 0;
}

// ---- game -----------------------------------------------------------------------

bool ConnectionsActivity::openPuzzle(const int index) {
  connections::Puzzle puzzle;
  if (!packOpen || !pack.readPuzzle(index, puzzle)) {
    LOG_ERR("CONN", "Could not read puzzle %d", index);
    return false;
  }
  const bool onRecord = resultFor(puzzle.date).played;
  if (!loadGame(puzzle)) {
    // Seeded from the id, so a puzzle always deals the same board.
    game.start(puzzle, 0x9E3779B9u ^ (static_cast<uint32_t>(puzzle.id) * 2654435761u));
  }
  gameLoaded = true;
  recorded = onRecord;
  replay = onRecord && game.result() == connections::Result::Playing && game.mistakes() == 0 && game.solvedCount() == 0;
  cursorRow = 0;
  cursorCol = 0;
  toast = Toast::NONE;
  finaleStage = 0;
  saveGame();
  showScreen(game.result() == connections::Result::Playing ? Screen::GAME : Screen::RESULT);
  return true;
}

void ConnectionsActivity::saveGame() const {
  if (!gameLoaded) return;
  const connections::Game::Save state = game.save();
  char line[160];
  int at = snprintf(line, sizeof(line), "%d %lu %lu %u %u %u %u %u %u %u", SAVE_VERSION,
                    static_cast<unsigned long>(game.puzzle().date), static_cast<unsigned long>(state.seed),
                    state.solvedCount, state.mistakes, state.solvedOrder[0], state.solvedOrder[1], state.solvedOrder[2],
                    state.solvedOrder[3], state.guessCount);
  for (int i = 0; i < state.guessCount && at > 0 && at < static_cast<int>(sizeof(line)) - 8; i++) {
    at += snprintf(line + at, sizeof(line) - at, " %u", state.guessed[i]);
  }
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  if (!Storage.openFileForWrite("CONN", SAVE_PATH, file) || file.write(line, at) != static_cast<size_t>(at)) {
    LOG_ERR("CONN", "Failed to save the board");
  }
}

// Restores the saved board when it belongs to `puzzle`.
bool ConnectionsActivity::loadGame(const connections::Puzzle& puzzle) {
  char buffer[160] = {};
  if (!Storage.exists(SAVE_PATH) || Storage.readFileToBuffer(SAVE_PATH, buffer, sizeof(buffer) - 1) == 0) return false;
  int version = 0, consumed = 0;
  unsigned long date = 0, seed = 0;
  unsigned solved = 0, mistakes = 0, order[4] = {}, guesses = 0;
  if (sscanf(buffer, "%d %lu %lu %u %u %u %u %u %u %u%n", &version, &date, &seed, &solved, &mistakes, &order[0],
             &order[1], &order[2], &order[3], &guesses, &consumed) != 10) {
    return false;
  }
  if (version != SAVE_VERSION || date != puzzle.date) return false;
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

// ---- update from the archive ------------------------------------------------------

void ConnectionsActivity::startUpdate() {
  updateError = "";
  updateAccepted = 0;
  screen = Screen::UPDATE;
  if (WiFi.status() == WL_CONNECTED) {
    updateStep = UpdateStep::DOWNLOAD;
  } else {
    updateStep = UpdateStep::CONNECT;
    wifiStartedHere = true;
  }
  cleanNext = true;
  requestUpdate();
}

void ConnectionsActivity::stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

void ConnectionsActivity::runConnect() {
  {
    RenderLock lock(*this);  // SD shares the SPI bus with the display
    WIFI_STORE.loadFromFile();
  }
  if (wifi_auto_connect::connect() == wifi_auto_connect::Result::CONNECTED) {
    updateStep = UpdateStep::DOWNLOAD;
    requestUpdate();
    return;
  }
  updateStep = UpdateStep::PICKING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                         [this](const ActivityResult& result) {
                           cleanNext = true;
                           if (result.isCancelled) {
                             updateStep = UpdateStep::FAILED;
                             updateError = tr(STR_CONN_ERR_WIFI);
                           } else {
                             updateStep = UpdateStep::DOWNLOAD;
                           }
                         });
}

// Streams the archive straight into a new pack (peak memory is one puzzle),
// written beside the old one and swapped in only once complete.
void ConnectionsActivity::runDownload() {
  time_t now = 0;
  network_time::ensure(now);  // the update is also a chance to set the clock for "today"
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    updateStep = UpdateStep::FAILED;
    updateError = tr(STR_CONN_ERR_MEMORY);
    requestUpdate();
    return;
  }
  auto import = makeUniqueNoThrow<ImportContext>();
  auto importer = makeUniqueNoThrow<connections::Importer>();
  if (!import || !importer) {
    LOG_ERR("CONN", "OOM: importer");
    updateStep = UpdateStep::FAILED;
    updateError = tr(STR_CONN_ERR_MEMORY);
    requestUpdate();
    return;
  }

  bool fetched = false, parsed = false;
  {
    RenderLock lock(*this);
    closePack();
    Storage.ensureDirectoryExists(DIR);
    Storage.remove(INDEX_TMP);
    Storage.remove(DATA_TMP);
    const bool opened = Storage.openFileForWrite("CONN", INDEX_TMP, import->index.file) &&
                        Storage.openFileForWrite("CONN", DATA_TMP, import->data.file) &&
                        import->writer.begin(writeTo, &import->index, &import->data);
    lock.unlock();
    if (opened) {
      importer->begin(onImportedPuzzle, import.get());
      fetched = HttpDownloader::fetchUrl(
          ARCHIVE_URL, [&importer](const uint8_t* data, const size_t len) { return importer->feed(data, len); });
      parsed = importer->finish();
    } else {
      import->index.failed = true;
    }
  }
  import->index.file.close();
  import->data.file.close();
  const bool writeFailed = import->index.failed || import->data.failed;
  const int written = import->writer.written();
  LOG_INF("CONN", "Import: fetched=%d parsed=%d written=%d skipped=%d/%d", fetched, parsed, written,
          importer->stats().skippedUnrenderable, importer->stats().skippedMalformed);
  if (wifiStartedHere) stopWifi();

  {
    RenderLock lock(*this);
    if (!fetched || !parsed || writeFailed || written == 0) {
      Storage.remove(INDEX_TMP);
      Storage.remove(DATA_TMP);
      updateStep = UpdateStep::FAILED;
      updateError = writeFailed ? tr(STR_CONN_ERR_CARD) : tr(STR_CONN_ERR_DOWNLOAD);
    } else {
      Storage.remove(INDEX_PATH);
      Storage.remove(DATA_PATH);
      const bool swapped = Storage.rename(INDEX_TMP, INDEX_PATH) && Storage.rename(DATA_TMP, DATA_PATH);
      updateStep = swapped ? UpdateStep::DONE : UpdateStep::FAILED;
      updateError = swapped ? "" : tr(STR_CONN_ERR_CARD);
      updateAccepted = written;
    }
    openPack();
    summary = connections::summarise(results.get(), resultsLength, connections::resultIndex(lastDate));
    refreshToday();
  }
  requestUpdate();
}

// ---- navigation -------------------------------------------------------------------

void ConnectionsActivity::showScreen(const Screen next) {
  screen = next;
  cleanNext = true;
  requestUpdate();
}

void ConnectionsActivity::openRandom() {
  if (!packOpen) return;
  const int count = pack.count();
  const int start = static_cast<int>(rng % static_cast<uint32_t>(count));
  rng = rng * 1664525u + 1013904223u;
  for (int step = 0; step < count; step++) {
    const int index = (start + step) % count;
    uint32_t date = 0;
    if (pack.dateAt(index, date) && !resultFor(date).played) {
      openPuzzle(index);
      return;
    }
  }
  openPuzzle(start);
}

void ConnectionsActivity::openHomeItem() {
  refreshToday();
  if (homeSelected == HOME_UPDATE) {
    startUpdate();
    return;
  }
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
      openPuzzle(todayIndex());
      break;
    case HOME_RANDOM:
      openRandom();
      break;
    default: {
      uint32_t date = lastDate;
      pack.dateAt(todayIndex(), date);
      archiveDate = gameLoaded && game.puzzle().date >= firstDate ? game.puzzle().date : date;
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
    const int index = pack.indexOfDate(archiveDate);
    if (index >= 0) openPuzzle(index);
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
  const uint32_t clamped = next < firstDate ? firstDate : next > lastDate ? lastDate : next;
  if (clamped == archiveDate) return;
  // A month change redraws most of the screen: clean it.
  if (clamped / 100 != archiveDate / 100) cleanNext = true;
  archiveDate = clamped;
  requestUpdate();
}

void ConnectionsActivity::handleStats() {
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) showScreen(Screen::HOME);
}

void ConnectionsActivity::handleUpdate() {
  // Each step shows its status before blocking on the network.
  if (updateStep == UpdateStep::CONNECT) {
    requestUpdateAndWait();
    runConnect();
    return;
  }
  if (updateStep == UpdateStep::DOWNLOAD) {
    requestUpdateAndWait();
    runDownload();
    return;
  }
}

void ConnectionsActivity::loop() {
  // Network steps block without the render lock; their status frame is drawn first.
  if (screen == Screen::UPDATE && updateStep <= UpdateStep::PICKING) {
    handleUpdate();
    return;
  }
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
    case Screen::UPDATE:
      if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) showScreen(Screen::HOME);
      break;
  }
}
