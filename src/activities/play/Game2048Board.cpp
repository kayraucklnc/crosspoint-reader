#include "Game2048Board.h"

namespace g2048 {
namespace {
// Cell index of the k-th tile in `line`, counting from the edge the tiles slide toward.
int lineCell(const Dir dir, const int line, const int k) {
  switch (dir) {
    case Dir::LEFT:
      return line * SIDE + k;
    case Dir::RIGHT:
      return line * SIDE + (SIDE - 1 - k);
    case Dir::UP:
      return k * SIDE + line;
    case Dir::DOWN:
    default:
      return (SIDE - 1 - k) * SIDE + line;
  }
}
}  // namespace

Slide slide(const Board& board, const Dir dir) {
  Slide result;
  for (int line = 0; line < SIDE; line++) {
    uint8_t packed[SIDE] = {};
    int count = 0;
    uint8_t pending = 0;
    for (int k = 0; k < SIDE; k++) {
      const uint8_t exp = board.cells[lineCell(dir, line, k)];
      if (exp == 0) continue;
      if (exp == pending && exp < MAX_EXP) {
        result.merged |= static_cast<uint16_t>(1u << lineCell(dir, line, count));
        packed[count++] = static_cast<uint8_t>(exp + 1);
        result.gained += tileValue(exp + 1);
        pending = 0;
      } else {
        if (pending != 0) packed[count++] = pending;
        pending = exp;
      }
    }
    if (pending != 0) packed[count++] = pending;
    for (int k = 0; k < SIDE; k++) {
      const int cell = lineCell(dir, line, k);
      result.board.cells[cell] = packed[k];
      if (packed[k] != board.cells[cell]) result.moved = true;
    }
  }
  return result;
}

bool canMove(const Board& board) {
  for (int row = 0; row < SIDE; row++) {
    for (int col = 0; col < SIDE; col++) {
      const uint8_t exp = board.cells[row * SIDE + col];
      if (exp == 0) return true;
      if (exp >= MAX_EXP) continue;
      if (col + 1 < SIDE && board.cells[row * SIDE + col + 1] == exp) return true;
      if (row + 1 < SIDE && board.cells[(row + 1) * SIDE + col] == exp) return true;
    }
  }
  return false;
}

int emptyCount(const Board& board) {
  int count = 0;
  for (const uint8_t exp : board.cells) count += exp == 0 ? 1 : 0;
  return count;
}

uint8_t maxExp(const Board& board) {
  uint8_t best = 0;
  for (const uint8_t exp : board.cells) best = exp > best ? exp : best;
  return best;
}

int spawn(Board& board, const uint32_t roll) {
  const int empty = emptyCount(board);
  if (empty == 0) return -1;
  // Low bits choose the cell, high bits the value, so the two stay independent.
  int pick = static_cast<int>((roll & 0xFFFFu) % static_cast<uint32_t>(empty));
  const bool four = (roll >> 16) % 10 == 0;
  for (int cell = 0; cell < CELLS; cell++) {
    if (board.cells[cell] != 0) continue;
    if (pick-- == 0) {
      board.cells[cell] = four ? 2 : 1;
      return cell;
    }
  }
  return -1;
}

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace g2048
