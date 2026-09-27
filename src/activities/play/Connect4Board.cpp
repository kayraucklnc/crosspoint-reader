#include "Connect4Board.h"

namespace c4 {

static_assert(BOTTOM == 0x40810204081ULL, "one bit per column bottom");

namespace {
// Bit distances between neighbours on a line: horizontal, falling, rising.
constexpr int LINE_SHIFTS[3] = {STRIDE, STRIDE - 1, STRIDE + 1};
constexpr int ALL_SHIFTS[4] = {1, STRIDE, STRIDE - 1, STRIDE + 1};

// The lowest cell of every four-in-a-line along `shift`.
uint64_t lineStarts(const uint64_t stones, const int shift) {
  const uint64_t pair = stones & (stones >> shift);
  return pair & (pair >> 2 * shift);
}
}  // namespace

uint64_t winningCells(const uint64_t stones, const uint64_t mask) {
  // Vertical: three stacked below the cell.
  uint64_t cells = (stones << 1) & (stones << 2) & (stones << 3);
  // Horizontal and both diagonals: every way a gap completes a line.
  for (const int shift : LINE_SHIFTS) {
    uint64_t pair = (stones << shift) & (stones << 2 * shift);
    cells |= pair & (stones << 3 * shift);
    cells |= pair & (stones >> shift);
    pair = (stones >> shift) & (stones >> 2 * shift);
    cells |= pair & (stones << shift);
    cells |= pair & (stones >> 3 * shift);
  }
  return cells & (FULL ^ mask);
}

bool hasFour(const uint64_t stones) {
  for (const int shift : ALL_SHIFTS) {
    if (lineStarts(stones, shift)) return true;
  }
  return false;
}

void Board::reset(const Side firstSide) {
  stones[0] = stones[1] = 0;
  count = 0;
  first = firstSide;
  winnerSide = -1;
  line = 0;
}

bool Board::canPlay(const int col) const {
  if (col < 0 || col >= COLS || winnerSide >= 0) return false;
  return ((stones[0] | stones[1]) & topBit(col)) == 0;
}

int Board::play(const int col) {
  if (!canPlay(col)) return -1;
  const uint64_t mask = stones[0] | stones[1];
  const uint64_t bit = (mask + bottomBit(col)) & columnBits(col);
  stones[toMove()] |= bit;
  history[count++] = static_cast<uint8_t>(col);
  settle();
  return lastRow();
}

bool Board::undo() {
  if (count == 0) return false;
  const int col = history[count - 1];
  const int row = lastRow();
  count--;
  stones[toMove()] &= ~cellBit(col, row);
  winnerSide = -1;
  line = 0;
  return true;
}

int Board::lastRow() const {
  if (count == 0) return -1;
  return __builtin_popcountll((stones[0] | stones[1]) & columnBits(history[count - 1])) - 1;
}

int Board::cell(const int col, const int row) const {
  const uint64_t bit = cellBit(col, row);
  if (stones[0] & bit) return 0;
  if (stones[1] & bit) return 1;
  return -1;
}

Position Board::position() const {
  Position pos;
  pos.current = stones[toMove()];
  pos.mask = stones[0] | stones[1];
  pos.moves = count;
  return pos;
}

// Only the last disc can have completed a line, so any line found goes
// through it.
void Board::settle() {
  const int side = (first + count - 1) & 1;
  for (const int shift : ALL_SHIFTS) {
    const uint64_t starts = lineStarts(stones[side], shift);
    if (!starts) continue;
    const uint64_t low = starts & (~starts + 1);
    line = low | (low << shift) | (low << 2 * shift) | (low << 3 * shift);
    winnerSide = static_cast<int8_t>(side);
    return;
  }
}

}  // namespace c4
