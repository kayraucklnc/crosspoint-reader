#pragma once

#include <cstdint>

// Pure 2048 rules, free of rendering and I/O so they can be unit tested on the host.
namespace g2048 {

constexpr int SIDE = 4;
constexpr int CELLS = SIDE * SIDE;
constexpr uint8_t WIN_EXP = 11;  // 2048
constexpr uint8_t MAX_EXP = 17;  // 131072, the largest tile a 4x4 board can build

enum class Dir : uint8_t { LEFT, RIGHT, UP, DOWN };

// Cells hold exponents: 0 = empty, 1 = 2, 2 = 4, ... 11 = 2048.
struct Board {
  uint8_t cells[CELLS] = {};
};

struct Slide {
  Board board;
  uint32_t gained = 0;  // sum of the tiles created by merges
  uint16_t merged = 0;  // bit per cell that received a merge
  bool moved = false;
};

inline uint32_t tileValue(const uint8_t exp) { return exp == 0 ? 0 : (1u << exp); }

// Slides every line toward `dir`; each tile merges at most once per move.
Slide slide(const Board& board, Dir dir);
bool canMove(const Board& board);
int emptyCount(const Board& board);
uint8_t maxExp(const Board& board);
// Picks the cell and value for a new tile from one random draw: a 2 nine times
// in ten, otherwise a 4. Returns the cell index, or -1 when the board is full.
int spawn(Board& board, uint32_t roll);
// xorshift32; `state` must be non-zero.
uint32_t nextRandom(uint32_t& state);

}  // namespace g2048
