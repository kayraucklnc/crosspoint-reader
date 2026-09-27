#include "TriviaActivity.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "network/NetworkTime.h"

namespace {
using B = MappedInputManager::Button;
// Categories weigh in by size up to this many questions, so the biggest (video
// games is a fifth of the pack) do not crowd out the rest in "All categories".
constexpr int CATEGORY_WEIGHT_CAP = 300;
constexpr int CLEAN_EVERY = 4;  // a HALF refresh with every fourth question
// Daily ten: three easy, four medium, three hard, never true/false.
constexpr uint8_t DAILY_PLAN[trivia::ROUND_LENGTH] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2};

int32_t daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
}  // namespace

// ---- lifecycle ----------------------------------------------------------------

void TriviaActivity::onEnter() {
  Activity::onEnter();
  trivia::loadStats(stats);
  if (pack.open()) {
    seen.load(pack.count());
    if (stats.category != trivia::ALL_CATEGORIES && stats.category >= pack.categoryCount()) {
      stats.category = trivia::ALL_CATEGORIES;
    }
  }
  rng.seed(static_cast<uint32_t>(millis()) * 2654435761u | 1u);
  refreshToday();
  showScreen(Screen::HOME);
}

void TriviaActivity::onExit() {
  persist();
  pack.close();
  Activity::onExit();
}

void TriviaActivity::refreshToday() {
  const time_t now = network_time::current();
  if (now == 0) {
    today = -1;
    return;
  }
  struct tm local{};
  localtime_r(&now, &local);
  today = daysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

void TriviaActivity::persist() {
  if (statsDirty && trivia::saveStats(stats)) statsDirty = false;
  seen.save();
}

void TriviaActivity::showScreen(const Screen next) {
  screen = next;
  cleanNext = true;
  requestUpdate();
}

void TriviaActivity::goHome() {
  persist();
  refreshToday();
  showScreen(Screen::HOME);
}

// ---- rounds -------------------------------------------------------------------

int TriviaActivity::dailyScore() const { return __builtin_popcount(stats.dailyMask); }

void TriviaActivity::openHomeItem() {
  refreshToday();
  switch (homeSelected) {
    case HOME_DAILY:
      if (today < 0 || !pack.isOpen()) {
        requestUpdate();  // the entry already explains why
        return;
      }
      startRound(Mode::DAILY);
      break;
    case HOME_QUICK:
      if (pack.isOpen()) startRound(Mode::QUICK);
      break;
    case HOME_SUDDEN:
      if (pack.isOpen()) startRound(Mode::SUDDEN);
      break;
    case HOME_FILTERS:
      if (!pack.isOpen()) return;
      filterRow = 0;
      showScreen(Screen::FILTERS);
      break;
    default:
      showScreen(Screen::STATS);
      break;
  }
}

// The same ten for the whole day: seeded by the day, spread across categories.
void TriviaActivity::planDaily() {
  trivia::Rng dayRng(static_cast<uint32_t>(today) * 2654435761u ^ 0x5EED1234u);
  const int count = pack.count();
  int previousCategory = -1;
  for (int k = 0; k < trivia::ROUND_LENGTH; k++) {
    dailyPicks[k] = static_cast<int>(dayRng.below(count));
    const int start = dailyPicks[k];
    for (int step = 0; step < count; step++) {
      const int i = (start + step) % count;
      const uint8_t meta = pack.meta(i);
      if (trivia::metaDifficulty(meta) != DAILY_PLAN[k] || trivia::metaBoolean(meta)) continue;
      if (trivia::metaCategory(meta) == previousCategory) continue;
      if (std::find(dailyPicks, dailyPicks + k, i) != dailyPicks + k) continue;
      dailyPicks[k] = i;
      break;
    }
    previousCategory = trivia::metaCategory(pack.meta(dailyPicks[k]));
  }
}

// A question matching the filters, unseen first. With all categories, a
// category is drawn first (weighted by size, capped) so small ones get turns.
int TriviaActivity::pickQuestion() {
  const int count = pack.count();
  uint8_t category = stats.category;
  if (category == trivia::ALL_CATEGORIES) {
    int weights[trivia::MAX_CATEGORIES] = {};
    for (int i = 0; i < count; i++) {
      const uint8_t meta = pack.meta(i);
      if (trivia::matches(meta, stats.difficulty, trivia::ALL_CATEGORIES)) weights[trivia::metaCategory(meta)]++;
    }
    int total = 0;
    for (int& w : weights) {
      w = std::min(w, CATEGORY_WEIGHT_CAP);
      total += w;
    }
    int roll = static_cast<int>(rng.below(static_cast<uint32_t>(std::max(total, 1))));
    for (int c = 0; c < pack.categoryCount(); c++) {
      if (roll < weights[c]) {
        category = static_cast<uint8_t>(c);
        break;
      }
      roll -= weights[c];
    }
  }
  const int start = static_cast<int>(rng.below(count));
  for (int pass = 0; pass < 2; pass++) {
    for (int step = 0; step < count; step++) {
      const int i = (start + step) % count;
      if (i == current || !trivia::matches(pack.meta(i), stats.difficulty, category)) continue;
      if (pass == 0 && seen.has(i)) continue;
      return i;
    }
  }
  return current >= 0 ? current : start;
}

bool TriviaActivity::loadQuestion(const int index) {
  if (!pack.read(index, question)) {
    LOG_ERR("TRIVIA", "Unreadable question %d", index);
    return false;
  }
  current = index;
  optionCount = question.wrongCount + 1;
  if (trivia::metaBoolean(question.meta)) {
    // True/false keeps its natural order.
    const bool answerIsTrue = strcmp(question.answer, "True") == 0;
    options[0] = answerIsTrue ? question.answer : question.wrong[0];
    options[1] = answerIsTrue ? question.wrong[0] : question.answer;
    correctOption = answerIsTrue ? 0 : 1;
  } else {
    options[0] = question.answer;
    for (int i = 0; i < question.wrongCount; i++) options[i + 1] = question.wrong[i];
    for (int i = optionCount - 1; i > 0; i--) {
      const int j = static_cast<int>(rng.below(i + 1));
      std::swap(options[i], options[j]);
    }
    for (int i = 0; i < optionCount; i++) {
      if (options[i] == question.answer) correctOption = i;
    }
  }
  seen.add(index);
  cursor = 0;
  chosen = -1;
  return true;
}

bool TriviaActivity::nextQuestion() {
  for (int attempt = 0; attempt < 4; attempt++) {
    const int index = mode == Mode::DAILY ? dailyPicks[roundPos] : pickQuestion();
    if (loadQuestion(index)) {
      if (roundPos % CLEAN_EVERY == 0) cleanNext = true;
      requestUpdate();
      return true;
    }
    if (mode == Mode::DAILY) break;
  }
  goHome();
  return false;
}

void TriviaActivity::startRound(const Mode next) {
  mode = next;
  newBest = false;
  runLength = 0;
  roundPos = 0;
  roundMask = 0;
  current = -1;
  if (mode == Mode::DAILY) {
    planDaily();
    if (stats.dailyDay != today) {
      stats.dailyDay = today;
      stats.dailyPos = 0;
      stats.dailyMask = 0;
      statsDirty = true;
    }
    roundPos = stats.dailyPos;
    roundMask = stats.dailyMask;
    if (roundPos >= trivia::ROUND_LENGTH) {
      showScreen(Screen::RESULT);
      return;
    }
  }
  screen = Screen::GAME;
  cleanNext = true;
  nextQuestion();
}

void TriviaActivity::answer(const int option) {
  chosen = option;
  const bool right = option == correctOption;
  const int difficulty = trivia::metaDifficulty(question.meta);
  stats.answered++;
  stats.byDifficulty[difficulty][0]++;
  if (right) {
    stats.correct++;
    stats.byDifficulty[difficulty][1]++;
    stats.answerStreak++;
    stats.bestAnswerStreak = std::max(stats.bestAnswerStreak, stats.answerStreak);
    roundMask |= static_cast<uint16_t>(1u << std::min(roundPos, 15));
    runLength++;
  } else {
    stats.answerStreak = 0;
  }
  roundPos++;
  statsDirty = true;
  if (mode == Mode::DAILY) {
    // Saved per answer so leaving and coming back cannot retry a question.
    stats.dailyPos = static_cast<uint8_t>(roundPos);
    stats.dailyMask = roundMask;
    persist();
  }
  requestUpdate();
}

bool TriviaActivity::roundOver() const {
  if (mode == Mode::SUDDEN) return chosen >= 0 && chosen != correctOption;
  return roundPos >= trivia::ROUND_LENGTH;
}

void TriviaActivity::finishRound() {
  const int score = __builtin_popcount(roundMask);
  switch (mode) {
    case Mode::QUICK:
      stats.rounds++;
      if (score == trivia::ROUND_LENGTH) stats.perfectRounds++;
      newBest = score > stats.bestRound;
      stats.bestRound = std::max<uint16_t>(stats.bestRound, score);
      break;
    case Mode::DAILY:
      stats.dailyPlayed++;
      stats.dailyStreak = stats.lastDailyDay == today - 1 ? stats.dailyStreak + 1 : 1;
      stats.lastDailyDay = today;
      stats.bestDailyStreak = std::max(stats.bestDailyStreak, stats.dailyStreak);
      newBest = score > stats.bestDaily;
      stats.bestDaily = std::max<uint16_t>(stats.bestDaily, score);
      break;
    case Mode::SUDDEN:
      newBest = runLength > stats.bestRun;
      stats.bestRun = std::max<uint16_t>(stats.bestRun, runLength);
      break;
  }
  statsDirty = true;
  persist();
  showScreen(Screen::RESULT);
}

// ---- input --------------------------------------------------------------------

void TriviaActivity::handleHome() {
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

void TriviaActivity::handleGame() {
  if (mappedInput.wasReleased(B::Back)) {
    goHome();
    return;
  }
  if (chosen >= 0) {
    if (mappedInput.wasReleased(B::Confirm)) {
      if (roundOver()) {
        finishRound();
      } else {
        nextQuestion();
      }
    }
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    answer(cursor);
    return;
  }
  if (mappedInput.wasPressed(B::NavPrevious)) {
    cursor = (cursor + optionCount - 1) % optionCount;
    requestUpdate();
  } else if (mappedInput.wasPressed(B::NavNext)) {
    cursor = (cursor + 1) % optionCount;
    requestUpdate();
  }
}

void TriviaActivity::handleResult() {
  if (mappedInput.wasReleased(B::Back)) {
    goHome();
  } else if (mappedInput.wasReleased(B::Confirm)) {
    if (mode == Mode::DAILY) {
      showScreen(Screen::STATS);
    } else {
      startRound(mode);
    }
  }
}

void TriviaActivity::handleFilters() {
  if (mappedInput.wasReleased(B::Back)) {
    goHome();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    startRound(Mode::QUICK);
    return;
  }
  if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down)) {
    filterRow = 1 - filterRow;
    requestUpdate();
    return;
  }
  const int step = mappedInput.wasPressed(B::Left) ? -1 : mappedInput.wasPressed(B::Right) ? 1 : 0;
  if (step == 0) return;
  if (filterRow == 0) {
    stats.difficulty = static_cast<uint8_t>((stats.difficulty + 4 + step) % 4);  // easy, medium, hard, any
  } else {
    // All categories sits before the first one.
    const int values = pack.categoryCount() + 1;
    const int position = stats.category == trivia::ALL_CATEGORIES ? 0 : stats.category + 1;
    const int next = (position + values + step) % values;
    stats.category = next == 0 ? trivia::ALL_CATEGORIES : static_cast<uint8_t>(next - 1);
  }
  statsDirty = true;
  requestUpdate();
}

void TriviaActivity::handleStats() {
  if (mappedInput.wasReleased(B::Back) || mappedInput.wasReleased(B::Confirm)) goHome();
}

void TriviaActivity::loop() {
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
    case Screen::FILTERS:
      handleFilters();
      break;
    case Screen::STATS:
      handleStats();
      break;
  }
}
