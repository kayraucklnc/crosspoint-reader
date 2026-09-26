#include "WordleActivity.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "network/NetworkTime.h"
#include "network/TrenordClient.h"

namespace {
using B = MappedInputManager::Button;
constexpr unsigned long HOLD_MS = 600;
constexpr unsigned long REPEAT_DELAY_MS = 400;
constexpr unsigned long REPEAT_MS = 220;       // ~2 keys per 480 ms panel frame
constexpr unsigned long REVEAL_STEP_MS = 350;  // the first reveal frame takes ~480 ms anyway
constexpr unsigned long RESULT_DELAY_MS = 1400;
constexpr uint32_t DAILY_STRIDE = 7919;  // prime: consecutive days never repeat within a list
constexpr uint32_t DAILY_SALT[wordle::LANG_COUNT] = {0, 104729};
constexpr int LEGEND_GAMES = 3;

int32_t daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
}  // namespace

void WordleActivity::onEnter() {
  Activity::onEnter();
  wordle::loadProgress(progress);
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  refreshToday();
  showScreen(Screen::HOME);
}

void WordleActivity::onExit() {
  persist();
  Activity::onExit();
}

void WordleActivity::refreshToday() {
  const time_t now = network_time::current();
  if (now == 0) {
    today = -1;
    return;
  }
  struct tm rome{};
  trenord::romeTime(now, rome);
  today = daysFromCivil(rome.tm_year + 1900, rome.tm_mon + 1, rome.tm_mday);
}

uint32_t WordleActivity::nextRandom() {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

// Simulator-only test hook: CROSSPOINT_SIM_WORDLE_EN / _TR force the answer.
bool WordleActivity::answerOverride(wordle::Word& out) const {
#ifdef SIMULATOR
  const char* forced = getenv(lang == wordle::LANG_TR ? "CROSSPOINT_SIM_WORDLE_TR" : "CROSSPOINT_SIM_WORDLE_EN");
  uint8_t letters[wordle::WORD_LEN];
  if (forced && wordle::parseWord(lang, forced, letters)) {
    out = wordle::pack(letters);
    return true;
  }
#endif
  (void)out;
  return false;
}

WordleActivity::Note WordleActivity::idleNote() const {
  if (!dictionary.hasLists()) return Note::NO_LISTS;
  return progress.finished < LEGEND_GAMES ? Note::LEGEND : Note::NONE;
}

void WordleActivity::persist() {
  if (!progressDirty) return;
  wordle::saveProgress(progress);
  progressDirty = false;
}

void WordleActivity::showScreen(const Screen next) {
  screen = next;
  fullRedraw = true;
  cleanNext = true;
  requestUpdate();
}

void WordleActivity::goHome() {
  persist();
  refreshToday();
  showScreen(Screen::HOME);
}

void WordleActivity::newRandomBoard() {
  wordle::Board& b = progress.random[lang];
  const wordle::Word previous = b.answer;
  b = wordle::Board{};
  b.started = true;
  if (!answerOverride(b.answer)) {
    const uint32_t count = dictionary.answerCount();
    for (int tries = 0; tries < 4 && (tries == 0 || b.answer == previous); tries++) {
      b.answer = dictionary.answerAt(nextRandom() % count);
    }
  }
  progressDirty = true;
  persist();
}

void WordleActivity::startGame(const wordle::Lang language, const bool isDaily) {
  lang = language;
  daily = isDaily;
  dictionary.open(lang);
  wordle::Board& b = board();
  if (daily && (!b.started || b.day != today)) {
    b = wordle::Board{};
    b.started = true;
    b.day = today;
    if (!answerOverride(b.answer)) {
      const uint32_t index = static_cast<uint32_t>(today) * DAILY_STRIDE + DAILY_SALT[lang];
      b.answer = dictionary.answerAt(index % dictionary.answerCount());
    }
    progressDirty = true;
    persist();
  } else if (!daily && (!b.started || b.state != wordle::GAME_PLAYING)) {
    newRandomBoard();
  }
  inputLength = 0;
  keyRow = keyCol = 0;
  revealStage = 0;
  resultDueMs = 0;
  xpGained = 0;
  leveledUp = false;
  note = idleNote();
  showScreen(b.state == wordle::GAME_PLAYING ? Screen::GAME : Screen::RESULT);
}

void WordleActivity::openHomeItem() {
  refreshToday();
  switch (homeSelected) {
    case 0:
    case 1:
      if (today < 0) {
        requestUpdate();  // the entry already explains the missing clock
        return;
      }
      startGame(homeSelected == 0 ? wordle::LANG_EN : wordle::LANG_TR, true);
      break;
    case 2:
    case 3:
      startGame(homeSelected == 2 ? wordle::LANG_EN : wordle::LANG_TR, false);
      break;
    default:
      statsLang = lang;
      showScreen(Screen::STATS);
      break;
  }
}

bool WordleActivity::repeatPress(const B button) {
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

void WordleActivity::moveColumn(const int delta) {
  const wordle::KeyRow& row = wordle::keyboard(lang)[keyRow];
  keyCol = static_cast<uint8_t>((keyCol + delta + row.count) % row.count);
  requestUpdate();
}

// Vertical moves land on the key whose centre is nearest the current one.
void WordleActivity::moveRow(const int delta) {
  int width = 0;
  const int center = keyX(keyRow, keyCol, &width) + width / 2;
  keyRow = static_cast<uint8_t>((keyRow + delta + wordle::KEY_ROWS) % wordle::KEY_ROWS);
  const wordle::KeyRow& row = wordle::keyboard(lang)[keyRow];
  int best = 0, bestDistance = 1 << 30;
  for (int col = 0; col < row.count; col++) {
    const int distance = std::abs(keyX(keyRow, col, &width) + width / 2 - center);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = col;
    }
  }
  keyCol = static_cast<uint8_t>(best);
  requestUpdate();
}

void WordleActivity::pressKey() {
  const uint8_t key = wordle::keyboard(lang)[keyRow].keys[keyCol];
  if (key == wordle::KEY_ENTER || inputLength == wordle::WORD_LEN) {
    submit();
    return;
  }
  if (inputLength >= wordle::WORD_LEN) return;
  input[inputLength++] = key;
  if (note == Note::TOO_SHORT || note == Note::NOT_WORD) note = idleNote();
  requestUpdate();
}

void WordleActivity::deleteLetter() {
  if (inputLength > 0) inputLength--;
  if (note == Note::TOO_SHORT || note == Note::NOT_WORD) note = idleNote();
  requestUpdate();
}

void WordleActivity::submit() {
  wordle::Board& b = board();
  if (b.state != wordle::GAME_PLAYING) return;
  if (inputLength < wordle::WORD_LEN) {
    note = Note::TOO_SHORT;
    requestUpdate();
    return;
  }
  const wordle::Word guess = wordle::pack(input);
  if (!dictionary.isValid(guess)) {
    note = Note::NOT_WORD;
    requestUpdate();
    return;
  }
  b.guesses[b.count++] = guess;
  inputLength = 0;
  note = idleNote();
  if (guess == b.answer) {
    b.state = wordle::GAME_WON;
  } else if (b.count >= wordle::MAX_GUESSES) {
    b.state = wordle::GAME_LOST;
  }
  if (b.state != wordle::GAME_PLAYING) {
    const int before = wordle::levelFor(stats().xp).level;
    xpGained = wordle::recordResult(stats(), b, daily);
    leveledUp = wordle::levelFor(stats().xp).level > before;
    progress.finished++;
  }
  progressDirty = true;
  persist();
  revealStage = 1;
  revealDueMs = millis() + REVEAL_STEP_MS;
  requestUpdate();
}

void WordleActivity::finishReveal() {
  revealStage = 0;
  cleanNext = true;  // HALF refresh with the revealed row scrubs the typing ghosts
  if (board().state != wordle::GAME_PLAYING) resultDueMs = millis() + RESULT_DELAY_MS;
  requestUpdate();
}

void WordleActivity::handleHome() {
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

void WordleActivity::handleGame() {
  const bool pressed = mappedInput.wasAnyPressed();
  if (revealStage != 0 && (pressed || millis() >= revealDueMs)) finishReveal();
  if (board().state != wordle::GAME_PLAYING) {
    if (revealStage == 0 && resultDueMs != 0 && (pressed || millis() >= resultDueMs)) {
      resultDueMs = 0;
      showScreen(Screen::RESULT);
    }
    return;
  }
  if (mappedInput.wasLongPressed(B::Back, HOLD_MS)) {
    goHome();
    return;
  }
  if (mappedInput.wasReleased(B::Back)) {
    if (inputLength == 0) {
      goHome();
    } else {
      deleteLetter();
    }
    return;
  }
  if (mappedInput.wasLongPressed(B::Confirm, HOLD_MS)) {
    submit();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    pressKey();
    return;
  }
  if (repeatPress(B::Left)) moveColumn(-1);
  if (repeatPress(B::Right)) moveColumn(+1);
  if (repeatPress(B::Up)) moveRow(-1);
  if (repeatPress(B::Down)) moveRow(+1);
}

void WordleActivity::handleResult() {
  if (mappedInput.wasReleased(B::Back)) {
    goHome();
  } else if (mappedInput.wasReleased(B::Confirm)) {
    if (daily) {
      statsLang = lang;
      showScreen(Screen::STATS);
    } else {
      newRandomBoard();
      startGame(lang, false);
    }
  }
}

void WordleActivity::handleStats() {
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) {
    goHome();
    return;
  }
  // Left/Right pick the tab under their hint; the side buttons toggle.
  wordle::Lang next = statsLang;
  if (mappedInput.wasPressed(B::Left)) next = wordle::LANG_EN;
  if (mappedInput.wasPressed(B::Right)) next = wordle::LANG_TR;
  if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down)) {
    next = statsLang == wordle::LANG_EN ? wordle::LANG_TR : wordle::LANG_EN;
  }
  if (next != statsLang) {
    statsLang = next;
    requestUpdate();
  }
}

void WordleActivity::loop() {
  RenderLock lock(*this);
  if (screen == Screen::HOME && mappedInput.wasReleased(B::Back)) {
    persist();
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
    case Screen::STATS:
      handleStats();
      break;
  }
}
