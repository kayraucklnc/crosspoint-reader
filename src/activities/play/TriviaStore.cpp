#include "TriviaStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace trivia {
namespace {
constexpr const char* DIR = "/.crosspoint/games/trivia";
constexpr const char* PATH = "/.crosspoint/games/trivia.json";
constexpr const char* SEEN_PATH = "/.crosspoint/games/trivia/seen.bin";
constexpr size_t JSON_CAP = 512;  // the file is ~250 bytes
constexpr int FORMAT_VERSION = 1;

// Flat JSON of "key":[integers], like the Wordle store.
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
}  // namespace

bool loadStats(Stats& out) {
  out = Stats{};
  if (!Storage.exists(PATH)) return false;
  auto buffer = makeUniqueNoThrow<char[]>(JSON_CAP);
  if (!buffer) {
    LOG_ERR("TRIVIA", "OOM: %u bytes", static_cast<unsigned>(JSON_CAP));
    return false;
  }
  char* json = buffer.get();
  memset(json, 0, JSON_CAP);
  if (Storage.readFileToBuffer(PATH, json, JSON_CAP - 1) == 0) {
    LOG_ERR("TRIVIA", "Unreadable %s", PATH);
    return false;
  }
  long t[8] = {};
  readArray(json, "totals", t, 8);
  out.answered = static_cast<uint32_t>(t[0]);
  out.correct = static_cast<uint32_t>(t[1]);
  out.rounds = static_cast<uint16_t>(t[2]);
  out.perfectRounds = static_cast<uint16_t>(t[3]);
  out.bestRound = static_cast<uint16_t>(t[4]);
  out.bestRun = static_cast<uint16_t>(t[5]);
  out.answerStreak = static_cast<uint16_t>(t[6]);
  out.bestAnswerStreak = static_cast<uint16_t>(t[7]);
  long d[DIFFICULTIES * 2] = {};
  readArray(json, "diff", d, DIFFICULTIES * 2);
  for (int i = 0; i < DIFFICULTIES; i++) {
    out.byDifficulty[i][0] = static_cast<uint32_t>(d[2 * i]);
    out.byDifficulty[i][1] = static_cast<uint32_t>(d[2 * i + 1]);
  }
  long daily[9] = {0, 0, 0, -1, 0, -1, 0, 0, 0};
  readArray(json, "daily", daily, 9);
  out.dailyPlayed = static_cast<uint16_t>(daily[0]);
  out.dailyStreak = static_cast<uint16_t>(daily[1]);
  out.bestDailyStreak = static_cast<uint16_t>(daily[2]);
  out.lastDailyDay = static_cast<int32_t>(daily[3]);
  out.bestDaily = static_cast<uint16_t>(daily[4]);
  out.dailyDay = static_cast<int32_t>(daily[5]);
  out.dailyPos = static_cast<uint8_t>(daily[6] > ROUND_LENGTH ? ROUND_LENGTH : daily[6]);
  out.dailyMask = static_cast<uint16_t>(daily[7]);
  long filters[2] = {ANY_DIFFICULTY, ALL_CATEGORIES};
  readArray(json, "filters", filters, 2);
  out.difficulty = static_cast<uint8_t>(filters[0] > ANY_DIFFICULTY ? ANY_DIFFICULTY : filters[0]);
  out.category = static_cast<uint8_t>(filters[1]);
  return true;
}

bool saveStats(const Stats& s) {
  auto buffer = makeUniqueNoThrow<char[]>(JSON_CAP);
  if (!buffer) {
    LOG_ERR("TRIVIA", "OOM: %u bytes", static_cast<unsigned>(JSON_CAP));
    return false;
  }
  char* json = buffer.get();
  size_t length = 1;
  json[0] = '{';
  const long meta[1] = {FORMAT_VERSION};
  writeArray(json, length, "meta", meta, 1);
  const long totals[8] = {static_cast<long>(s.answered),
                          static_cast<long>(s.correct),
                          s.rounds,
                          s.perfectRounds,
                          s.bestRound,
                          s.bestRun,
                          s.answerStreak,
                          s.bestAnswerStreak};
  writeArray(json, length, "totals", totals, 8);
  long diff[DIFFICULTIES * 2];
  for (int i = 0; i < DIFFICULTIES; i++) {
    diff[2 * i] = static_cast<long>(s.byDifficulty[i][0]);
    diff[2 * i + 1] = static_cast<long>(s.byDifficulty[i][1]);
  }
  writeArray(json, length, "diff", diff, DIFFICULTIES * 2);
  const long daily[8] = {s.dailyPlayed, s.dailyStreak, s.bestDailyStreak, s.lastDailyDay,
                         s.bestDaily,   s.dailyDay,    s.dailyPos,        s.dailyMask};
  writeArray(json, length, "daily", daily, 8);
  const long filters[2] = {s.difficulty, s.category};
  writeArray(json, length, "filters", filters, 2);
  if (length + 2 >= JSON_CAP) {
    LOG_ERR("TRIVIA", "Stats too large to save");
    return false;
  }
  json[length++] = '}';
  Storage.ensureDirectoryExists("/.crosspoint/games");
  HalFile file;
  if (!Storage.openFileForWrite("TRIVIA", PATH, file) || file.write(json, length) != length) {
    LOG_ERR("TRIVIA", "Failed to write %s", PATH);
    return false;
  }
  return true;
}

uint16_t currentDailyStreak(const Stats& stats, const int32_t today) {
  return stats.lastDailyDay >= today - 1 ? stats.dailyStreak : 0;
}

bool SeenSet::load(const int questions) {
  count = questions;
  seenCount = 0;
  dirty = false;
  const int bytes = (count + 7) / 8;
  bits = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!bits) {
    LOG_ERR("TRIVIA", "OOM: seen %d bytes", bytes);
    count = 0;
    return false;
  }
  memset(bits.get(), 0, bytes);
  HalFile file;
  uint8_t header[4];
  if (!Storage.openFileForRead("TRIVIA", SEEN_PATH, file) || file.read(header, 4) != 4) return false;
  const int stored = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
  if (stored != count || file.read(bits.get(), bytes) != bytes) {
    memset(bits.get(), 0, bytes);
    return false;
  }
  for (int i = 0; i < bytes; i++) seenCount += __builtin_popcount(bits[i]);
  return true;
}

bool SeenSet::save() {
  if (!dirty || !bits) return true;
  Storage.ensureDirectoryExists(DIR);
  HalFile file;
  const uint8_t header[4] = {static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8),
                             static_cast<uint8_t>(count >> 16), static_cast<uint8_t>(count >> 24)};
  const size_t bytes = (count + 7) / 8;
  if (!Storage.openFileForWrite("TRIVIA", SEEN_PATH, file) || file.write(header, 4) != 4 ||
      file.write(bits.get(), bytes) != bytes) {
    LOG_ERR("TRIVIA", "Failed to write %s", SEEN_PATH);
    return false;
  }
  dirty = false;
  return true;
}

bool SeenSet::has(const int index) const {
  return bits && index >= 0 && index < count && (bits[index >> 3] & (1u << (index & 7))) != 0;
}

void SeenSet::add(const int index) {
  if (!bits || index < 0 || index >= count || has(index)) return;
  bits[index >> 3] |= static_cast<uint8_t>(1u << (index & 7));
  seenCount++;
  dirty = true;
}

void SeenSet::clear() {
  if (!bits) return;
  memset(bits.get(), 0, (count + 7) / 8);
  seenCount = 0;
  dirty = true;
}

}  // namespace trivia
