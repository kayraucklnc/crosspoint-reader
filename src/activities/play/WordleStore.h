#pragma once

#include <cstdint>

#include "WordleWords.h"

// Wordle progress persisted to /.crosspoint/games/wordle.json: per-language
// stats and XP, today's daily board and the unfinished random board.
namespace wordle {

enum GameState : uint8_t { GAME_PLAYING = 0, GAME_WON = 1, GAME_LOST = 2 };

struct Board {
  bool started = false;
  uint8_t count = 0;
  uint8_t state = GAME_PLAYING;
  int32_t day = -1;  // Rome day number of a daily board
  Word answer = 0;
  Word guesses[MAX_GUESSES] = {};
};

struct LangStats {
  uint32_t xp = 0;
  uint16_t dailyPlayed = 0;
  uint16_t dailyWon = 0;
  uint16_t streak = 0;
  uint16_t bestStreak = 0;
  int32_t lastWinDay = -1;
  uint16_t dist[MAX_GUESSES] = {};  // daily wins by guess count
  uint16_t randomPlayed = 0;
  uint16_t randomWon = 0;
};

struct Progress {
  LangStats stats[LANG_COUNT];
  Board daily[LANG_COUNT];
  Board random[LANG_COUNT];
  uint16_t finished = 0;  // games completed in any mode; the legend shows for the first few
};

struct Level {
  int level;      // from 1
  uint32_t into;  // XP earned inside this level
  uint32_t span;  // XP this level needs
};

constexpr int TITLE_COUNT = 6;

bool loadProgress(Progress& out);
bool saveProgress(const Progress& progress);

Level levelFor(uint32_t xp);
int titleIndex(int level);  // 0..TITLE_COUNT-1
// Streak as it stands today: a missed day breaks it even before the next win.
uint16_t currentStreak(const LangStats& stats, int32_t today);
// Books a finished board into the stats and returns the XP it earned.
uint32_t recordResult(LangStats& stats, const Board& board, bool daily);

}  // namespace wordle
