#pragma once

#include <cstdint>

// Connect Four rules on bitboards, without drawing or I/O so they can be
// unit-tested on the host. Bit `col * 7 + row` is a cell (row 0 at the bottom);
// the seventh bit of each column is a sentinel that keeps lines from wrapping
// into the next column.
namespace c4 {

constexpr int COLS = 7;
constexpr int ROWS = 6;
constexpr int CELLS = COLS * ROWS;
constexpr int STRIDE = ROWS + 1;

constexpr uint64_t BOTTOM = 0x0040810204081ULL;  // bit 0 of every column
constexpr uint64_t FULL = BOTTOM * ((1ULL << ROWS) - 1);

constexpr uint64_t bottomBit(const int col) { return 1ULL << (col * STRIDE); }
constexpr uint64_t topBit(const int col) { return 1ULL << (ROWS - 1 + col * STRIDE); }
constexpr uint64_t columnBits(const int col) { return ((1ULL << ROWS) - 1) << (col * STRIDE); }
constexpr uint64_t cellBit(const int col, const int row) { return 1ULL << (col * STRIDE + row); }

// Empty cells that would complete four in a line for the stones in `stones`.
uint64_t winningCells(uint64_t stones, uint64_t mask);
// True when `stones` hold four in a line.
bool hasFour(uint64_t stones);

// The search's view: stones of the side to move and all stones.
struct Position {
  uint64_t current = 0;
  uint64_t mask = 0;
  uint8_t moves = 0;
};

// Side 0 is the human (solid discs), side 1 the computer (ringed discs).
enum Side : uint8_t { HUMAN = 0, COMPUTER = 1 };

class Board {
 public:
  void reset(Side first);

  bool canPlay(int col) const;
  // Drops a disc for the side to move; returns its row, or -1 when the column
  // is full or the game is over.
  int play(int col);
  // Takes back the last disc. Returns false when the board is empty.
  bool undo();

  Side toMove() const { return static_cast<Side>((first + count) & 1); }
  Side firstSide() const { return first; }
  int moveCount() const { return count; }
  int moveAt(int index) const { return history[index]; }
  int lastColumn() const { return count > 0 ? history[count - 1] : -1; }
  int lastRow() const;
  // -1 for an empty cell, otherwise the Side that owns it.
  int cell(int col, int row) const;
  bool full() const { return count >= CELLS; }
  // The side with four in a line, or -1.
  int winner() const { return winnerSide; }
  bool over() const { return winnerSide >= 0 || full(); }
  // The four winning cells (0 when nobody has won).
  uint64_t winLine() const { return line; }

  Position position() const;

 private:
  uint64_t stones[2] = {};
  uint8_t history[CELLS] = {};
  uint8_t count = 0;
  Side first = HUMAN;
  int8_t winnerSide = -1;
  uint64_t line = 0;

  void settle();
};

}  // namespace c4
