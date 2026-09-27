#include "Connect4Brain.h"

#include <cstring>

namespace c4 {
namespace {

constexpr uint8_t EXACT = 1;
constexpr uint8_t LOWER = 2;
constexpr uint8_t UPPER = 3;
constexpr uint32_t CLOCK_EVERY = 1024;  // nodes between clock/abort checks
constexpr uint8_t CENTRE_ORDER[COLS] = {3, 2, 4, 1, 5, 0, 6};
constexpr uint64_t COL3 = columnBits(3);
constexpr uint64_t COL24 = columnBits(2) | columnBits(4);
constexpr uint64_t COL15 = columnBits(1) | columnBits(5);
constexpr int THREAT_WEIGHT = 16;

int popcount(const uint64_t bits) { return __builtin_popcountll(bits); }

int columnOf(const uint64_t bit) {
  for (int col = 0; col < COLS; col++) {
    if (bit & columnBits(col)) return col;
  }
  return -1;
}

uint64_t playable(const uint64_t mask) { return (mask + BOTTOM) & FULL; }

uint64_t keyOf(const uint64_t current, const uint64_t mask) { return current + mask; }

}  // namespace

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

int evaluate(const uint64_t current, const uint64_t mask) {
  const uint64_t opponent = current ^ mask;
  int score = THREAT_WEIGHT * (popcount(winningCells(current, mask)) - popcount(winningCells(opponent, mask)));
  score += 4 * (popcount(current & COL3) - popcount(opponent & COL3));
  score += 2 * (popcount(current & COL24) - popcount(opponent & COL24));
  score += popcount(current & COL15) - popcount(opponent & COL15);
  return score;
}

void Search::clearTable() {
  if (table) memset(table, 0, sizeof(TTEntry) * TT_SIZE);
}

bool Search::timeUp() {
  if (abortFlag && abortFlag->load(std::memory_order_relaxed)) return true;
  return timed && static_cast<int32_t>(clock() - deadline) >= 0;
}

// Fills `out` with the candidate columns, best first: the table's move, then
// by how many winning cells the move leaves, ties toward the centre.
int Search::orderMoves(const uint64_t current, const uint64_t mask, const uint64_t candidates, const int preferred,
                       uint8_t* out) const {
  int8_t weight[COLS];
  int n = 0;
  for (const uint8_t col : CENTRE_ORDER) {
    const uint64_t move = candidates & columnBits(col);
    if (!move) continue;
    int8_t w = static_cast<int8_t>(popcount(winningCells(current | move, mask | move)));
    if (col == preferred) w = 100;
    int i = n++;
    for (; i > 0 && weight[i - 1] < w; i--) {
      weight[i] = weight[i - 1];
      out[i] = out[i - 1];
    }
    weight[i] = w;
    out[i] = col;
  }
  return n;
}

int Search::negamax(const uint64_t current, const uint64_t mask, const int moves, const int depth, int alpha,
                    int beta) {
  if (++nodes % CLOCK_EVERY == 0 && timeUp()) stopped = true;
  if (stopped) return 0;
  if (moves >= CELLS) return 0;

  uint64_t candidates = playable(mask);
  if (winningCells(current, mask) & candidates) return WIN - (moves + 1);
  const uint64_t theirWins = winningCells(current ^ mask, mask);
  const uint64_t forced = candidates & theirWins;
  if (forced) {
    if (forced & (forced - 1)) return -(WIN - (moves + 2));
    candidates = forced;
  }
  candidates &= ~(theirWins >> 1);  // never fill the cell under their win
  if (!candidates) return -(WIN - (moves + 2));
  if (moves >= CELLS - 2) return 0;
  if (depth <= 0) return evaluate(current, mask);

  // Neither side can win on the next two stones: tighten the window.
  const int best = WIN - (moves + 3);
  const int worst = -(WIN - (moves + 4));
  if (beta > best) {
    beta = best;
    if (alpha >= beta) return beta;
  }
  if (alpha < worst) {
    alpha = worst;
    if (alpha >= beta) return alpha;
  }

  const uint64_t key = keyOf(current, mask);
  const auto check = static_cast<uint32_t>(key ^ (key >> 32));
  TTEntry* entry = table ? &table[(key * 0x9E3779B97F4A7C15ULL) >> (64 - TT_BITS)] : nullptr;
  int preferred = -1;
  if (entry && entry->check == check && entry->flagMove) {
    preferred = (entry->flagMove >> 2) - 1;
    if (entry->depth >= depth) {
      const int stored = entry->score;
      const uint8_t flag = entry->flagMove & 3;
      if (flag == EXACT) return stored;
      if (flag == LOWER && stored > alpha) alpha = stored;
      if (flag == UPPER && stored < beta) beta = stored;
      if (alpha >= beta) return stored;
    }
  }

  uint8_t order[COLS];
  const int count = orderMoves(current, mask, candidates, preferred, order);
  const int alphaIn = alpha;
  int bestScore = -INF;
  int bestCol = -1;
  for (int i = 0; i < count; i++) {
    const uint64_t move = candidates & columnBits(order[i]);
    const int score = -negamax(current ^ mask, mask | move, moves + 1, depth - 1, -beta, -alpha);
    if (stopped) return 0;
    if (score > bestScore) {
      bestScore = score;
      bestCol = order[i];
    }
    if (score > alpha) alpha = score;
    if (alpha >= beta) break;
  }

  if (entry) {
    const uint8_t flag = bestScore <= alphaIn ? UPPER : bestScore >= beta ? LOWER : EXACT;
    entry->check = check;
    entry->score = static_cast<int16_t>(bestScore);
    entry->depth = static_cast<uint8_t>(depth);
    entry->flagMove = static_cast<uint8_t>(flag | ((bestCol + 1) << 2));
  }
  return bestScore;
}

int Search::bestMove(const Position& pos, const int maxDepth, const uint32_t budgetMs, const Clock clockFn,
                     const std::atomic<bool>* abort, uint32_t* rng) {
  clock = clockFn;
  abortFlag = abort;
  timed = clockFn != nullptr && budgetMs > 0;
  deadline = timed ? clockFn() + budgetMs : 0;
  stopped = false;
  nodes = 0;
  lastDepth = 0;
  lastScore = 0;

  const uint64_t current = pos.current, mask = pos.mask;
  const uint64_t legal = playable(mask);
  if (!legal || hasFour(current) || hasFour(current ^ mask)) return -1;
  if (const uint64_t wins = winningCells(current, mask) & legal) {
    lastScore = WIN - (pos.moves + 1);
    return columnOf(wins & (~wins + 1));
  }

  uint8_t order[COLS];
  int count = orderMoves(current, mask, legal, -1, order);
  int bestCol = order[0];
  const int remaining = CELLS - pos.moves;
  const int depthLimit = maxDepth < remaining ? maxDepth : remaining;
  for (int depth = 1; depth <= depthLimit; depth++) {
    int iterScore = -INF;
    uint8_t ties[COLS];
    int tieCount = 0;
    for (int i = 0; i < count; i++) {
      const uint64_t move = legal & columnBits(order[i]);
      // With rng, search one point wider so equal moves report exact scores.
      const int alpha = iterScore == -INF ? -INF : iterScore - (rng ? 1 : 0);
      const int score = -negamax(current ^ mask, mask | move, pos.moves + 1, depth - 1, -INF, -alpha);
      if (stopped) break;
      if (score > iterScore) {
        iterScore = score;
        tieCount = 0;
      }
      if (score == iterScore) ties[tieCount++] = order[i];
    }
    if (stopped && depth > 1) break;
    if (tieCount == 0) break;  // stopped before the first move finished
    const int pick = rng ? static_cast<int>(nextRandom(*rng) % tieCount) : 0;
    bestCol = ties[pick];
    lastScore = iterScore;
    lastDepth = depth;
    if (stopped) break;
    // Search the best move first next time.
    for (int i = 0; i < count; i++) {
      if (order[i] != bestCol) continue;
      for (; i > 0; i--) order[i] = order[i - 1];
      order[0] = static_cast<uint8_t>(bestCol);
      break;
    }
    if (iterScore >= WIN - CELLS || iterScore <= -(WIN - CELLS)) break;  // decided
  }
  return bestCol;
}

int Search::choose(const Position& pos, const Level& level, const Clock clockFn, const std::atomic<bool>* abort,
                   uint32_t& rng) {
  if (level.randomPercent > 0 && nextRandom(rng) % 100 < level.randomPercent) {
    const uint64_t legal = playable(pos.mask);
    if (legal) {
      for (int col = static_cast<int>(nextRandom(rng) % COLS);; col = (col + 1) % COLS) {
        if (legal & columnBits(col)) return col;
      }
    }
  }
  return bestMove(pos, level.maxDepth, level.budgetMs, clockFn, abort, &rng);
}

}  // namespace c4
