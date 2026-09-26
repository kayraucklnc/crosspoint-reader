#include "WordleStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wordle {
namespace {
constexpr const char* DIR = "/.crosspoint/games";
constexpr const char* PATH = "/.crosspoint/games/wordle.json";
constexpr size_t JSON_CAP = 1024;  // the file is ~400 bytes
constexpr int FORMAT_VERSION = 1;
constexpr const char* STAT_KEYS[LANG_COUNT] = {"en", "tr"};
constexpr const char* DAILY_KEYS[LANG_COUNT] = {"en_daily", "tr_daily"};
constexpr const char* RANDOM_KEYS[LANG_COUNT] = {"en_rand", "tr_rand"};
// Stats: xp, dailyPlayed, dailyWon, streak, bestStreak, lastWinDay, randomPlayed, randomWon, dist[6].
constexpr int STAT_FIELDS = 8 + MAX_GUESSES;
// Boards: day, state, count, answer, guesses[count].
constexpr int BOARD_FIELDS = 4 + MAX_GUESSES;

// XP: dailies are the main path, random games a small practice reward.
constexpr uint32_t DAILY_WIN_XP = 100;
constexpr uint32_t DAILY_SPARE_GUESS_XP = 20;  // per unused guess
constexpr uint32_t STREAK_XP = 10;             // per streak day, capped
constexpr uint16_t STREAK_XP_CAP = 10;
constexpr uint32_t DAILY_LOSS_XP = 10;
constexpr uint32_t RANDOM_WIN_XP = 10;
constexpr uint32_t RANDOM_SPARE_GUESS_XP = 4;
// Level L -> L+1 needs LEVEL_BASE_XP + LEVEL_STEP_XP * (L - 1).
constexpr uint32_t LEVEL_BASE_XP = 100;
constexpr uint32_t LEVEL_STEP_XP = 50;
constexpr int TITLE_LEVELS[TITLE_COUNT] = {1, 3, 6, 10, 15, 21};

// The file is flat JSON: every entry is "key":[integers]. Reads the integers
// of one key and returns how many were found.
int readArray(const char* json, const char* key, long* out, const int max) {
  char pattern[16];
  snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
  const char* p = strstr(json, pattern);
  if (!p) return 0;
  p += strlen(pattern);
  int count = 0;
  while (count < max && *p != ']') {
    char* end = nullptr;
    const long value = strtol(p, &end, 10);
    if (end == p) break;
    out[count++] = value;
    p = *end == ',' ? end + 1 : end;
  }
  return count;
}

void writeArray(char* out, size_t& length, const char* key, const long* values, const int count) {
  const auto append = [&](const char* format, auto value) {
    if (length < JSON_CAP) length += snprintf(out + length, JSON_CAP - length, format, value);
  };
  append(length > 1 ? ",\"%s\":[" : "\"%s\":[", key);
  for (int i = 0; i < count; i++) append(i ? ",%ld" : "%ld", values[i]);
  append("%s", "]");
}

void readBoard(const char* json, const char* key, Board& board) {
  long v[BOARD_FIELDS];
  const int n = readArray(json, key, v, BOARD_FIELDS);
  if (n < 4) return;
  board.started = true;
  board.day = v[0];
  board.state = static_cast<uint8_t>(std::clamp<long>(v[1], GAME_PLAYING, GAME_LOST));
  board.count = static_cast<uint8_t>(std::clamp<long>(v[2], 0, std::min(n - 4, MAX_GUESSES)));
  board.answer = static_cast<Word>(v[3]);
  for (int i = 0; i < board.count; i++) board.guesses[i] = static_cast<Word>(v[4 + i]);
}

void writeBoard(char* out, size_t& length, const char* key, const Board& board) {
  if (!board.started) return;
  long v[BOARD_FIELDS] = {board.day, board.state, board.count, static_cast<long>(board.answer)};
  for (int i = 0; i < board.count; i++) v[4 + i] = static_cast<long>(board.guesses[i]);
  writeArray(out, length, key, v, 4 + board.count);
}
}  // namespace

bool loadProgress(Progress& out) {
  out = Progress{};
  if (!Storage.exists(PATH)) return false;
  auto json = makeUniqueNoThrow<char[]>(JSON_CAP);
  if (!json) {
    LOG_ERR("WORDLE", "OOM: %u bytes", static_cast<unsigned>(JSON_CAP));
    return false;
  }
  if (Storage.readFileToBuffer(PATH, json.get(), JSON_CAP - 1) == 0) {
    LOG_ERR("WORDLE", "Unreadable %s", PATH);
    return false;
  }
  long meta[2] = {};
  readArray(json.get(), "meta", meta, 2);
  out.finished = static_cast<uint16_t>(meta[1]);
  for (int lang = 0; lang < LANG_COUNT; lang++) {
    long v[STAT_FIELDS] = {};
    readArray(json.get(), STAT_KEYS[lang], v, STAT_FIELDS);
    LangStats& s = out.stats[lang];
    s.xp = static_cast<uint32_t>(v[0]);
    s.dailyPlayed = static_cast<uint16_t>(v[1]);
    s.dailyWon = static_cast<uint16_t>(v[2]);
    s.streak = static_cast<uint16_t>(v[3]);
    s.bestStreak = static_cast<uint16_t>(v[4]);
    s.lastWinDay = v[5] != 0 ? static_cast<int32_t>(v[5]) : -1;
    s.randomPlayed = static_cast<uint16_t>(v[6]);
    s.randomWon = static_cast<uint16_t>(v[7]);
    for (int i = 0; i < MAX_GUESSES; i++) s.dist[i] = static_cast<uint16_t>(v[8 + i]);
    readBoard(json.get(), DAILY_KEYS[lang], out.daily[lang]);
    readBoard(json.get(), RANDOM_KEYS[lang], out.random[lang]);
  }
  return true;
}

bool saveProgress(const Progress& progress) {
  auto json = makeUniqueNoThrow<char[]>(JSON_CAP);
  if (!json) {
    LOG_ERR("WORDLE", "OOM: %u bytes", static_cast<unsigned>(JSON_CAP));
    return false;
  }
  size_t length = 1;
  json[0] = '{';
  const long meta[2] = {FORMAT_VERSION, progress.finished};
  writeArray(json.get(), length, "meta", meta, 2);
  for (int lang = 0; lang < LANG_COUNT; lang++) {
    const LangStats& s = progress.stats[lang];
    long v[STAT_FIELDS] = {static_cast<long>(s.xp), s.dailyPlayed, s.dailyWon, s.streak, s.bestStreak, s.lastWinDay,
                           s.randomPlayed,          s.randomWon};
    for (int i = 0; i < MAX_GUESSES; i++) v[8 + i] = s.dist[i];
    writeArray(json.get(), length, STAT_KEYS[lang], v, STAT_FIELDS);
    writeBoard(json.get(), length, DAILY_KEYS[lang], progress.daily[lang]);
    writeBoard(json.get(), length, RANDOM_KEYS[lang], progress.random[lang]);
  }
  if (length + 2 >= JSON_CAP) {
    LOG_ERR("WORDLE", "Progress too large to save");
    return false;
  }
  json[length++] = '}';
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  if (!Storage.openFileForWrite("WORDLE", PATH, file) || file.write(json.get(), length) != length) {
    LOG_ERR("WORDLE", "Failed to write %s", PATH);
    return false;
  }
  return true;
}

Level levelFor(uint32_t xp) {
  Level result{1, 0, LEVEL_BASE_XP};
  while (xp >= result.span) {
    xp -= result.span;
    result.level++;
    result.span += LEVEL_STEP_XP;
  }
  result.into = xp;
  return result;
}

int titleIndex(const int level) {
  int index = 0;
  while (index + 1 < TITLE_COUNT && level >= TITLE_LEVELS[index + 1]) index++;
  return index;
}

uint16_t currentStreak(const LangStats& stats, const int32_t today) {
  return stats.lastWinDay >= today - 1 ? stats.streak : 0;
}

uint32_t recordResult(LangStats& stats, const Board& board, const bool daily) {
  const bool won = board.state == GAME_WON;
  const uint32_t spare = won ? static_cast<uint32_t>(MAX_GUESSES - board.count) : 0;
  uint32_t xp = 0;
  if (daily) {
    stats.dailyPlayed++;
    if (won) {
      stats.dailyWon++;
      stats.dist[board.count - 1]++;
      stats.streak = stats.lastWinDay == board.day - 1 ? stats.streak + 1 : 1;
      stats.lastWinDay = board.day;
      stats.bestStreak = std::max(stats.bestStreak, stats.streak);
      xp = DAILY_WIN_XP + DAILY_SPARE_GUESS_XP * spare + STREAK_XP * std::min(stats.streak, STREAK_XP_CAP);
    } else {
      stats.streak = 0;
      xp = DAILY_LOSS_XP;
    }
  } else {
    stats.randomPlayed++;
    if (won) {
      stats.randomWon++;
      xp = RANDOM_WIN_XP + RANDOM_SPARE_GUESS_XP * spare;
    }
  }
  stats.xp += xp;
  return xp;
}

}  // namespace wordle
