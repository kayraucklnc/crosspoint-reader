#include "SokobanHint.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace sokoban {
namespace {
constexpr uint8_t UNSEEN = 0xFF;
constexpr uint8_t ORIGIN = 0xFE;

// Replays `moves` on `board`, counting pushes; with `out` it also records the
// layout before each one.
bool replay(Board& board, const uint8_t* packed, const int moves, const bool transposed, PushStep* out, int& pushes) {
  pushes = 0;
  for (int k = 0; k < moves; k++) {
    int code = (packed[k / 4] >> (6 - 2 * (k % 4))) & 3;
    if (transposed) code = 3 - code;  // rows and columns swap: up <-> left, right <-> down
    const Dir dir = static_cast<Dir>(code);
    const int next = board.neighbour(board.playerCell(), dir);
    if (next >= 0 && board.hasBox(next)) {
      if (out) out[pushes] = {board.boxHash(), static_cast<uint16_t>(board.playerCell()), static_cast<uint8_t>(dir)};
      pushes++;
    }
    if (board.move(dir) == Board::Step::BLOCKED) {
      LOG_ERR("SOKO", "Solution blocked at move %d", k);
      return false;
    }
  }
  return board.solved();
}
}  // namespace

void SolutionGuide::clear() {
  steps.reset();
  queue.reset();
  cameFrom.reset();
  count = 0;
  cells = 0;
}

bool SolutionGuide::build(const LevelPack& pack, const int index, const bool transposed) {
  clear();
  std::unique_ptr<uint8_t[]> packed;
  int moves = 0;
  if (!pack.loadSolution(index, packed, moves)) return false;
  // A scratch copy of the level (~3 KB) lives only while replaying.
  auto scratch = makeUniqueNoThrow<Board>();
  if (!scratch) {
    LOG_ERR("SOKO", "OOM: solution replay board");
    return false;
  }
  uint16_t par = 0, parPushes = 0;
  if (!pack.load(index, *scratch, par, parPushes)) return false;
  if (transposed) scratch->transpose();
  int pushes = 0;
  if (!replay(*scratch, packed.get(), moves, transposed, nullptr, pushes)) return false;

  cells = scratch->width() * scratch->height();
  steps = makeUniqueNoThrow<PushStep[]>(pushes);
  queue = makeUniqueNoThrow<uint16_t[]>(cells);
  cameFrom = makeUniqueNoThrow<uint8_t[]>(cells);
  if (!steps || !queue || !cameFrom) {
    LOG_ERR("SOKO", "OOM: guide for %d pushes, %d cells", pushes, cells);
    clear();
    return false;
  }
  pack.load(index, *scratch, par, parPushes);
  if (transposed) scratch->transpose();
  replay(*scratch, packed.get(), moves, transposed, steps.get(), count);
  LOG_DBG("SOKO", "Guide: %d moves, %d pushes", moves, count);
  return count > 0;
}

bool SolutionGuide::find(const Board& board, Hint& hint, uint8_t* route) const {
  if (!ready() || board.solved() || board.width() * board.height() != cells) return false;
  // Every cell the keeper can reach, remembering the step that entered it.
  memset(cameFrom.get(), UNSEEN, cells);
  const int start = board.playerCell();
  cameFrom[start] = ORIGIN;
  int head = 0, tail = 0;
  queue[tail++] = static_cast<uint16_t>(start);
  while (head < tail) {
    const int cell = queue[head++];
    for (int d = 0; d < 4; d++) {
      const int n = board.neighbour(cell, static_cast<Dir>(d));
      if (n < 0 || cameFrom[n] != UNSEEN || !board.walkable(n)) continue;
      cameFrom[n] = static_cast<uint8_t>(d);
      queue[tail++] = static_cast<uint16_t>(n);
    }
  }
  const uint32_t hash = board.boxHash();
  for (int k = count - 1; k >= 0; k--) {
    const PushStep& s = steps[k];
    if (s.hash != hash || cameFrom[s.from] == UNSEEN) continue;
    const int box = board.neighbour(s.from, static_cast<Dir>(s.dir));
    if (box < 0 || !board.hasBox(box)) continue;
    hint = {k, s.from, box, static_cast<Dir>(s.dir)};
    memset(route, 0, MAX_CELLS / 8);
    for (int c = s.from; cameFrom[c] != ORIGIN;) {
      route[c / 8] |= static_cast<uint8_t>(1 << (c % 8));
      c = board.neighbour(c, static_cast<Dir>((cameFrom[c] + 2) % 4));
    }
    return true;
  }
  return false;
}

}  // namespace sokoban
