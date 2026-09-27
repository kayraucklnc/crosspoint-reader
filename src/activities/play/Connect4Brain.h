#pragma once

#include <atomic>
#include <cstdint>

#include "Connect4Board.h"

// The computer player: negamax with alpha-beta over bitboards, centre-first
// move ordering (refined by threat count and the table's best move), a small
// transposition table and iterative deepening against a time budget. Scores
// are from the side to move: WIN - n for a win completed by the n-th stone,
// its negation for a loss, otherwise a threat/centre heuristic.
namespace c4 {

constexpr int WIN = 10000;
constexpr int INF = 30000;
constexpr int LEVEL_COUNT = 3;

struct Level {
  uint8_t maxDepth;
  uint16_t budgetMs;
  uint8_t randomPercent;  // chance of an arbitrary legal move instead of a search
};
// Easy, Medium, Hard.
constexpr Level LEVELS[LEVEL_COUNT] = {{1, 400, 30}, {5, 800, 0}, {CELLS, 1600, 0}};
// Hints search like Hard, a little shorter.
constexpr Level HINT_LEVEL = {CELLS, 1000, 0};

struct TTEntry {
  uint32_t check;
  int16_t score;
  uint8_t depth;
  uint8_t flagMove;  // bits 0-1 bound kind, bits 2-4 best column + 1 (0 = none)
};
constexpr int TT_BITS = 9;  // 512 entries, 4 KB
constexpr int TT_SIZE = 1 << TT_BITS;

// Static evaluation for the side to move.
int evaluate(uint64_t current, uint64_t mask);

class Search {
 public:
  using Clock = uint32_t (*)();

  // `table` may be null (no transposition table); it must hold TT_SIZE entries.
  explicit Search(TTEntry* table) : table(table) {}

  // Best column for the side to move, or -1 when there is no legal move.
  // `clock` may be null (depth limit only). `abort` may be null. With `rng`,
  // equally scored best moves are picked at random.
  int bestMove(const Position& pos, int maxDepth, uint32_t budgetMs, Clock clock, const std::atomic<bool>* abort,
               uint32_t* rng);
  // Applies a level: sometimes a random legal move, otherwise a search.
  int choose(const Position& pos, const Level& level, Clock clock, const std::atomic<bool>* abort, uint32_t& rng);

  void clearTable();

  int lastScore = 0;
  int lastDepth = 0;
  uint32_t nodes = 0;

 private:
  TTEntry* table;
  Clock clock = nullptr;
  const std::atomic<bool>* abortFlag = nullptr;
  uint32_t deadline = 0;
  bool timed = false;
  bool stopped = false;

  int negamax(uint64_t current, uint64_t mask, int moves, int depth, int alpha, int beta);
  bool timeUp();
  int orderMoves(uint64_t current, uint64_t mask, uint64_t candidates, int preferred, uint8_t* out) const;
};

uint32_t nextRandom(uint32_t& state);

}  // namespace c4
