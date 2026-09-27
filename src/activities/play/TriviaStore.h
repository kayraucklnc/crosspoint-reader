#pragma once

#include <cstdint>
#include <memory>

#include "TriviaPack.h"

// Trivia progress: stats and filters in /.crosspoint/games/trivia.json, and
// which questions have been served in /.crosspoint/games/trivia/seen.bin (one
// bit per pack question, so repeats wait until the pack is used up).
namespace trivia {

constexpr int ROUND_LENGTH = 10;

struct Stats {
  uint32_t answered = 0;
  uint32_t correct = 0;
  uint16_t rounds = 0;         // finished quick rounds
  uint16_t perfectRounds = 0;  // 10 of 10
  uint16_t bestRound = 0;      // best quick-round score
  uint16_t bestRun = 0;        // best sudden-death run
  uint16_t answerStreak = 0;   // consecutive right answers, any mode
  uint16_t bestAnswerStreak = 0;
  uint32_t byDifficulty[DIFFICULTIES][2] = {};  // answered, correct
  // Daily ten.
  uint16_t dailyPlayed = 0;
  uint16_t dailyStreak = 0;  // consecutive days with the daily finished
  uint16_t bestDailyStreak = 0;
  int32_t lastDailyDay = -1;
  uint16_t bestDaily = 0;
  int32_t dailyDay = -1;   // the day of the daily in progress (or finished)
  uint8_t dailyPos = 0;    // questions answered in it
  uint16_t dailyMask = 0;  // bit i: question i answered right
  // Quick-round filters.
  uint8_t difficulty = ANY_DIFFICULTY;
  uint8_t category = ALL_CATEGORIES;
};

bool loadStats(Stats& out);
bool saveStats(const Stats& stats);
// Daily streak as it stands today: a missed day breaks it before the next daily.
uint16_t currentDailyStreak(const Stats& stats, int32_t today);

// One bit per pack question. The file is discarded when its size no longer
// matches the pack (a new pack was copied onto the card).
class SeenSet {
 public:
  bool load(int count);
  bool save();
  bool has(int index) const;
  void add(int index);
  int size() const { return seenCount; }
  void clear();

 private:
  std::unique_ptr<uint8_t[]> bits;
  int count = 0;
  int seenCount = 0;
  bool dirty = false;
};

}  // namespace trivia
