#include "MinesweeperBoard.h"

#include <algorithm>
#include <cstdlib>

namespace mines {
namespace {
uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
}  // namespace

template <typename Fn>
void Board::forEachNeighbour(const int cell, Fn&& fn) const {
  const int col = cell % width;
  const int row = cell / width;
  for (int dy = -1; dy <= 1; dy++) {
    const int r = row + dy;
    if (r < 0 || r >= height) continue;
    for (int dx = -1; dx <= 1; dx++) {
      const int c = col + dx;
      if ((dx == 0 && dy == 0) || c < 0 || c >= width) continue;
      fn(r * width + c);
    }
  }
}

void Board::reset(const int cols, const int rows, const int mineTotal) {
  width = std::clamp(cols, 2, MAX_COLS);
  height = std::clamp(rows, 2, MAX_ROWS);
  mines = std::clamp(mineTotal, 1, width * height - 1);
  flags = 0;
  hiddenSafe = width * height - mines;
  tripped = -1;
  gameState = State::READY;
  std::fill(cells, cells + MAX_CELLS, DIRTY);
}

void Board::markAllDirty() {
  for (int i = 0; i < cellCount(); i++) cells[i] |= DIRTY;
}

void Board::placeMines(const int safeCell, uint32_t& rng) {
  const int total = cellCount();
  const int safeCol = safeCell % width;
  const int safeRow = safeCell / width;
  // Keep the 3x3 around the first reveal clear so it opens an area; tiny or
  // crowded boards fall back to sparing only the cell itself.
  const bool spareNeighbours = total - 9 >= mines;
  int candidates = 0;
  for (int i = 0; i < total; i++) {
    const bool near = std::abs(i % width - safeCol) <= 1 && std::abs(i / width - safeRow) <= 1;
    if (i == safeCell || (spareNeighbours && near)) continue;
    stack[candidates++] = static_cast<uint16_t>(i);
  }
  // Partial Fisher-Yates: the first `mines` slots become the mine cells.
  for (int i = 0; i < mines; i++) {
    const int j = i + static_cast<int>(nextRandom(rng) % static_cast<uint32_t>(candidates - i));
    std::swap(stack[i], stack[j]);
    cells[stack[i]] |= MINE;
  }
  for (int i = 0; i < total; i++) {
    if (!isMine(i)) continue;
    forEachNeighbour(i, [this](const int n) { cells[n]++; });
  }
}

bool Board::open(const int cell) {
  if (isOpen(cell) || isFlagged(cell)) return true;
  if (isMine(cell)) return false;
  int top = 0;
  cells[cell] |= OPEN | DIRTY;
  hiddenSafe--;
  stack[top++] = static_cast<uint16_t>(cell);
  while (top > 0) {
    const int current = stack[--top];
    if (number(current) != 0) continue;
    forEachNeighbour(current, [this, &top](const int n) {
      if (isOpen(n) || isFlagged(n)) return;
      cells[n] |= OPEN | DIRTY;
      hiddenSafe--;
      stack[top++] = static_cast<uint16_t>(n);
    });
  }
  return true;
}

void Board::lose(const int cell) {
  tripped = cell;
  gameState = State::LOST;
  // Mines and wrong flags are drawn differently once the game is lost.
  for (int i = 0; i < cellCount(); i++) {
    if (isMine(i) || isFlagged(i)) cells[i] |= DIRTY;
  }
}

Outcome Board::finishIfWon() {
  if (hiddenSafe > 0) return Outcome::CHANGED;
  gameState = State::WON;
  for (int i = 0; i < cellCount(); i++) {
    if (isMine(i) && !isFlagged(i)) cells[i] |= FLAG | DIRTY;
  }
  flags = mines;
  return Outcome::WON;
}

Outcome Board::reveal(const int cell, uint32_t& rng) {
  if (over() || cell < 0 || cell >= cellCount() || isOpen(cell) || isFlagged(cell)) return Outcome::NONE;
  if (gameState == State::READY) {
    placeMines(cell, rng);
    gameState = State::PLAYING;
  }
  if (!open(cell)) {
    lose(cell);
    return Outcome::LOST;
  }
  return finishIfWon();
}

Outcome Board::pressNumber(const int cell) {
  if (gameState != State::PLAYING || cell < 0 || cell >= cellCount() || !isOpen(cell)) return Outcome::NONE;
  const int need = number(cell);
  int flagged = 0, hidden = 0;
  forEachNeighbour(cell, [&](const int n) {
    if (isFlagged(n)) {
      flagged++;
    } else if (!isOpen(n)) {
      hidden++;
    }
  });
  if (need == 0 || hidden == 0) return Outcome::NONE;
  if (flagged == need) {
    int mineHit = -1;
    forEachNeighbour(cell, [&](const int n) {
      if (!open(n) && mineHit < 0) mineHit = n;
    });
    if (mineHit >= 0) {
      lose(mineHit);
      return Outcome::LOST;
    }
    return finishIfWon();
  }
  if (flagged + hidden == need) {
    forEachNeighbour(cell, [this](const int n) {
      if (!isOpen(n) && !isFlagged(n)) {
        cells[n] |= FLAG | DIRTY;
        flags++;
      }
    });
    return Outcome::CHANGED;
  }
  return Outcome::NONE;
}

bool Board::toggleFlag(const int cell) {
  if (over() || cell < 0 || cell >= cellCount() || isOpen(cell)) return false;
  cells[cell] ^= FLAG;
  cells[cell] |= DIRTY;
  flags += isFlagged(cell) ? 1 : -1;
  return true;
}

}  // namespace mines
