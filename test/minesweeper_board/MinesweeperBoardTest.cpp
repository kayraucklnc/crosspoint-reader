#include <gtest/gtest.h>

#include <cstdint>

#include "activities/play/MinesweeperBoard.h"

using mines::Board;
using mines::Outcome;
using mines::State;

namespace {
int countMines(const Board& board) {
  int total = 0;
  for (int i = 0; i < board.cellCount(); i++) total += board.isMine(i) ? 1 : 0;
  return total;
}

int neighbourMines(const Board& board, const int cell) {
  const int col = cell % board.cols(), row = cell / board.cols();
  int total = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      const int c = col + dx, r = row + dy;
      if ((dx || dy) && c >= 0 && r >= 0 && c < board.cols() && r < board.rows()) {
        total += board.isMine(r * board.cols() + c) ? 1 : 0;
      }
    }
  }
  return total;
}

// Opens every safe cell directly; the last one must win.
Outcome clearAllSafe(Board& board, uint32_t& rng) {
  Outcome last = Outcome::NONE;
  for (int i = 0; i < board.cellCount(); i++) {
    if (!board.isMine(i) && !board.isOpen(i)) last = board.reveal(i, rng);
  }
  return last;
}
}  // namespace

TEST(MinesweeperBoard, FirstRevealIsSafeAndOpensArea) {
  for (uint32_t seed = 1; seed < 200; seed++) {
    Board board;
    board.reset(16, 24, 60);
    uint32_t rng = seed;
    const int first = static_cast<int>(seed % 384);
    const Outcome outcome = board.reveal(first, rng);
    ASSERT_NE(outcome, Outcome::LOST);
    EXPECT_EQ(countMines(board), 60);
    EXPECT_TRUE(board.isOpen(first));
    EXPECT_EQ(board.number(first), 0);
    EXPECT_EQ(board.state(), State::PLAYING);
  }
}

TEST(MinesweeperBoard, NumbersMatchNeighbourMines) {
  Board board;
  board.reset(9, 9, 10);
  uint32_t rng = 12345;
  board.reveal(40, rng);
  for (int i = 0; i < board.cellCount(); i++) {
    if (!board.isMine(i)) EXPECT_EQ(board.number(i), neighbourMines(board, i)) << i;
  }
}

TEST(MinesweeperBoard, FloodFillStopsAtNumbers) {
  Board board;
  board.reset(12, 16, 30);
  uint32_t rng = 777;
  board.reveal(0, rng);
  for (int i = 0; i < board.cellCount(); i++) {
    if (!board.isBlank(i)) continue;
    // Every neighbour of an opened blank is opened too.
    const int col = i % 12, row = i / 12;
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        const int c = col + dx, r = row + dy;
        if (c >= 0 && r >= 0 && c < 12 && r < 16) EXPECT_TRUE(board.isOpen(r * 12 + c));
      }
    }
  }
  for (int i = 0; i < board.cellCount(); i++) EXPECT_FALSE(board.isOpen(i) && board.isMine(i));
}

TEST(MinesweeperBoard, RevealingMineLoses) {
  Board board;
  board.reset(9, 9, 10);
  uint32_t rng = 99;
  board.reveal(40, rng);
  int mine = 0;
  while (!board.isMine(mine)) mine++;
  EXPECT_EQ(board.reveal(mine, rng), Outcome::LOST);
  EXPECT_EQ(board.state(), State::LOST);
  EXPECT_EQ(board.trippedCell(), mine);
  EXPECT_EQ(board.reveal(mine, rng), Outcome::NONE);
  EXPECT_FALSE(board.toggleFlag(0));
}

TEST(MinesweeperBoard, ClearingAllSafeCellsWinsAndFlagsMines) {
  Board board;
  board.reset(9, 9, 10);
  uint32_t rng = 4242;
  board.reveal(0, rng);
  EXPECT_EQ(clearAllSafe(board, rng), Outcome::WON);
  EXPECT_EQ(board.state(), State::WON);
  EXPECT_EQ(board.minesLeft(), 0);
  for (int i = 0; i < board.cellCount(); i++) {
    if (board.isMine(i)) EXPECT_TRUE(board.isFlagged(i));
  }
}

TEST(MinesweeperBoard, FlagsBlockRevealAndCount) {
  Board board;
  board.reset(9, 9, 10);
  uint32_t rng = 5;
  EXPECT_TRUE(board.toggleFlag(3));
  EXPECT_EQ(board.minesLeft(), 9);
  EXPECT_EQ(board.reveal(3, rng), Outcome::NONE);
  EXPECT_TRUE(board.toggleFlag(3));
  EXPECT_EQ(board.minesLeft(), 10);
  board.reveal(40, rng);
  EXPECT_FALSE(board.toggleFlag(40));
}

TEST(MinesweeperBoard, PressNumberChordsWhenFlagsSatisfied) {
  for (uint32_t seed = 1; seed < 100; seed++) {
    Board board;
    board.reset(9, 9, 10);
    uint32_t rng = seed;
    board.reveal(40, rng);
    // Find an opened number and flag exactly its mines.
    int target = -1;
    for (int i = 0; i < board.cellCount() && target < 0; i++) {
      if (board.isOpen(i) && board.number(i) > 0) target = i;
    }
    ASSERT_GE(target, 0);
    const int col = target % 9, row = target / 9;
    bool hasHiddenSafe = false;
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        const int c = col + dx, r = row + dy;
        if (c < 0 || r < 0 || c >= 9 || r >= 9) continue;
        const int n = r * 9 + c;
        if (board.isMine(n)) board.toggleFlag(n);
        if (!board.isMine(n) && !board.isOpen(n)) hasHiddenSafe = true;
      }
    }
    const Outcome outcome = board.pressNumber(target);
    if (!hasHiddenSafe) {
      EXPECT_EQ(outcome, Outcome::NONE);
      continue;
    }
    EXPECT_NE(outcome, Outcome::LOST);
    EXPECT_NE(outcome, Outcome::NONE);
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        const int c = col + dx, r = row + dy;
        if (c >= 0 && r >= 0 && c < 9 && r < 9 && !board.isMine(r * 9 + c)) EXPECT_TRUE(board.isOpen(r * 9 + c));
      }
    }
  }
}

TEST(MinesweeperBoard, PressNumberWithWrongFlagLoses) {
  for (uint32_t seed = 1; seed < 300; seed++) {
    Board board;
    board.reset(9, 9, 10);
    uint32_t rng = seed;
    board.reveal(40, rng);
    // A "1" with at least one hidden safe neighbour and one mine neighbour.
    for (int i = 0; i < board.cellCount(); i++) {
      if (!board.isOpen(i) || board.number(i) != 1) continue;
      int safe = -1;
      const int col = i % 9, row = i / 9;
      for (int dy = -1; dy <= 1 && safe < 0; dy++) {
        for (int dx = -1; dx <= 1 && safe < 0; dx++) {
          const int c = col + dx, r = row + dy;
          if (c < 0 || r < 0 || c >= 9 || r >= 9) continue;
          const int n = r * 9 + c;
          if (!board.isOpen(n) && !board.isMine(n)) safe = n;
        }
      }
      if (safe < 0) continue;
      board.toggleFlag(safe);
      EXPECT_EQ(board.pressNumber(i), Outcome::LOST);
      EXPECT_TRUE(board.isMine(board.trippedCell()));
      return;
    }
  }
  FAIL() << "no suitable board found";
}

TEST(MinesweeperBoard, PressNumberFlagsForcedMines) {
  int checked = 0;
  for (uint32_t seed = 1; seed < 300; seed++) {
    Board board;
    board.reset(9, 9, 10);
    uint32_t rng = seed;
    board.reveal(40, rng);
    for (int i = 0; i < board.cellCount(); i++) {
      if (!board.isOpen(i) || board.number(i) == 0) continue;
      int hidden = 0;
      const int col = i % 9, row = i / 9;
      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          const int c = col + dx, r = row + dy;
          if (c >= 0 && r >= 0 && c < 9 && r < 9 && !board.isOpen(r * 9 + c)) hidden++;
        }
      }
      if (hidden != board.number(i)) continue;
      const int before = board.flagCount();
      EXPECT_EQ(board.pressNumber(i), Outcome::CHANGED);
      EXPECT_EQ(board.flagCount(), before + hidden);
      checked++;
      break;
    }
  }
  EXPECT_GT(checked, 0);
}

TEST(MinesweeperBoard, DirtyBitsTrackChanges) {
  Board board;
  board.reset(9, 9, 10);
  for (int i = 0; i < board.cellCount(); i++) {
    EXPECT_TRUE(board.isDirty(i));
    board.clearDirty(i);
  }
  board.toggleFlag(5);
  EXPECT_TRUE(board.isDirty(5));
  EXPECT_FALSE(board.isDirty(6));
}
