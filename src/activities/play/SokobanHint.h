#pragma once

#include <cstdint>
#include <memory>

#include "SokobanBoard.h"

namespace sokoban {

// The next push along a level's stored solution.
struct Hint {
  int step = -1;  // index of the push in the solution (0-based)
  int from = -1;  // cell the keeper pushes from
  int box = -1;   // cell of the crate to push
  Dir dir = UP;   // push direction
};

// One push of a stored solution and the box layout just before it.
struct PushStep {
  uint32_t hash;  // Board::boxHash()
  uint16_t from;  // keeper cell
  uint8_t dir;
};

// Replays a level's stored solution once and remembers, for every push, the
// box layout before it. A live position is "on the path" when its boxes match
// one of those layouts and the keeper can walk to that push; walking detours
// between pushes never matter.
class SolutionGuide {
 public:
  // `transposed` mirrors the live board, which layoutBoard() may have turned.
  bool build(const LevelPack& pack, int index, bool transposed);
  void clear();
  bool ready() const { return count > 0; }
  int pushes() const { return count; }

  // Finds the furthest solution push the position on `board` can make next.
  // Fills `route` (MAX_CELLS / 8 bytes, one bit per cell) with the shortest
  // walk to hint.from, excluding the keeper's own cell.
  bool find(const Board& board, Hint& hint, uint8_t* route) const;

 private:
  std::unique_ptr<PushStep[]> steps;
  int count = 0;
  // BFS scratch sized to the level, reused by every find().
  std::unique_ptr<uint16_t[]> queue;
  std::unique_ptr<uint8_t[]> cameFrom;
  int cells = 0;
};

}  // namespace sokoban
