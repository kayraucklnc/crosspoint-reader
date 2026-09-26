#pragma once

#include <cstdint>

// Minesweeper rules without any drawing or I/O, so they can be unit-tested on
// the host. Mines are placed on the first reveal, away from the opened cell's
// 3x3 neighbourhood, so the first move always opens an area.
//
// Every cell that changes appearance gets a dirty bit; the renderer repaints
// exactly those cells and clears the bits.
namespace mines {

constexpr int MAX_COLS = 16;
constexpr int MAX_ROWS = 24;
constexpr int MAX_CELLS = MAX_COLS * MAX_ROWS;

enum class State : uint8_t { READY, PLAYING, WON, LOST };
// NONE: the press changed nothing. CHANGED: cells changed, game continues.
enum class Outcome : uint8_t { NONE, CHANGED, WON, LOST };

class Board {
 public:
  // Clears the board to an unplayed state. Sizes are clamped to MAX_*.
  void reset(int cols, int rows, int mines);

  // Opens a hidden, unflagged cell, flood-filling blank areas. The first call
  // places the mines using `rng` (xorshift32 state, must be non-zero).
  Outcome reveal(int cell, uint32_t& rng);
  // Pressing an opened number: when its flags are satisfied, opens the other
  // neighbours ("chord"); when every hidden neighbour must be a mine, flags them.
  Outcome pressNumber(int cell);
  // Flags or unflags a hidden cell. Returns false when the cell can't be flagged.
  bool toggleFlag(int cell);

  int cols() const { return width; }
  int rows() const { return height; }
  int cellCount() const { return width * height; }
  int mineCount() const { return mines; }
  int flagCount() const { return flags; }
  int minesLeft() const { return mines - flags; }
  State state() const { return gameState; }
  bool over() const { return gameState == State::WON || gameState == State::LOST; }
  int trippedCell() const { return tripped; }

  bool isOpen(const int cell) const { return (cells[cell] & OPEN) != 0; }
  bool isFlagged(const int cell) const { return (cells[cell] & FLAG) != 0; }
  bool isMine(const int cell) const { return (cells[cell] & MINE) != 0; }
  // Adjacent mine count; only meaningful once mines are placed.
  int number(const int cell) const { return cells[cell] & COUNT_MASK; }
  // An opened cell with no neighbouring mines.
  bool isBlank(const int cell) const { return isOpen(cell) && number(cell) == 0 && !isMine(cell); }

  bool isDirty(const int cell) const { return (cells[cell] & DIRTY) != 0; }
  void markDirty(const int cell) { cells[cell] |= DIRTY; }
  void markAllDirty();
  void clearDirty(const int cell) { cells[cell] &= static_cast<uint8_t>(~DIRTY); }

 private:
  static constexpr uint8_t COUNT_MASK = 0x0F;
  static constexpr uint8_t MINE = 0x10;
  static constexpr uint8_t OPEN = 0x20;
  static constexpr uint8_t FLAG = 0x40;
  static constexpr uint8_t DIRTY = 0x80;

  uint8_t cells[MAX_CELLS] = {};
  uint16_t stack[MAX_CELLS] = {};  // flood-fill work list, also mine-placement scratch
  int width = 0;
  int height = 0;
  int mines = 0;
  int flags = 0;
  int hiddenSafe = 0;  // safe cells still closed; zero means won
  int tripped = -1;
  State gameState = State::READY;

  template <typename Fn>
  void forEachNeighbour(int cell, Fn&& fn) const;
  void placeMines(int safeCell, uint32_t& rng);
  // Opens `cell` and any blank region connected to it. Returns false on a mine.
  bool open(int cell);
  Outcome finishIfWon();
  void lose(int cell);
};

}  // namespace mines
