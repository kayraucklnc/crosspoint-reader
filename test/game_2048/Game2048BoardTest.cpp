#include <gtest/gtest.h>

#include "activities/play/Game2048Board.h"

using g2048::Board;
using g2048::Dir;

namespace {
Board make(std::initializer_list<uint8_t> cells) {
  Board board;
  int i = 0;
  for (const uint8_t exp : cells) board.cells[i++] = exp;
  return board;
}

void expectRow(const Board& board, const int row, std::initializer_list<uint8_t> expected) {
  int col = 0;
  for (const uint8_t exp : expected) {
    EXPECT_EQ(board.cells[row * g2048::SIDE + col], exp) << "row " << row << " col " << col;
    col++;
  }
}
}  // namespace

TEST(Game2048Board, SlidesLeftAndMergesPairsOnce) {
  const Board board = make({1, 1, 1, 1,  //
                            2, 0, 2, 3,  //
                            0, 0, 0, 4,  //
                            1, 2, 3, 4});
  const auto result = g2048::slide(board, Dir::LEFT);
  EXPECT_TRUE(result.moved);
  expectRow(result.board, 0, {2, 2, 0, 0});
  expectRow(result.board, 1, {3, 3, 0, 0});
  expectRow(result.board, 2, {4, 0, 0, 0});
  expectRow(result.board, 3, {1, 2, 3, 4});
  EXPECT_EQ(result.gained, 4u + 4u + 8u);
  EXPECT_EQ(result.merged, (1u << 0) | (1u << 1) | (1u << 4));
}

TEST(Game2048Board, MergesTowardTheSlideEdge) {
  const Board board = make({1, 1, 1, 0,  //
                            0, 0, 0, 0,  //
                            0, 0, 0, 0,  //
                            0, 0, 0, 0});
  const auto result = g2048::slide(board, Dir::RIGHT);
  expectRow(result.board, 0, {0, 0, 1, 2});
  EXPECT_EQ(result.gained, 4u);
  EXPECT_EQ(result.merged, 1u << 3);
}

TEST(Game2048Board, SlidesColumnsUpAndDown) {
  const Board board = make({1, 0, 0, 0,  //
                            1, 0, 0, 0,  //
                            2, 0, 0, 0,  //
                            0, 0, 0, 5});
  const auto up = g2048::slide(board, Dir::UP);
  EXPECT_EQ(up.board.cells[0], 2);
  EXPECT_EQ(up.board.cells[4], 2);
  EXPECT_EQ(up.board.cells[8], 0);
  EXPECT_EQ(up.board.cells[3], 5);
  const auto down = g2048::slide(board, Dir::DOWN);
  EXPECT_EQ(down.board.cells[12], 2);
  EXPECT_EQ(down.board.cells[8], 2);
  EXPECT_EQ(down.board.cells[0], 0);
  EXPECT_EQ(down.board.cells[15], 5);
}

TEST(Game2048Board, ReportsNoMoveWhenNothingShifts) {
  const Board board = make({1, 2, 0, 0,  //
                            3, 0, 0, 0,  //
                            0, 0, 0, 0,  //
                            0, 0, 0, 0});
  EXPECT_FALSE(g2048::slide(board, Dir::LEFT).moved);
  EXPECT_FALSE(g2048::slide(board, Dir::UP).moved);
  EXPECT_TRUE(g2048::slide(board, Dir::RIGHT).moved);
}

TEST(Game2048Board, DetectsGameOver) {
  const Board stuck = make({1, 2, 1, 2,  //
                            2, 1, 2, 1,  //
                            1, 2, 1, 2,  //
                            2, 1, 2, 1});
  EXPECT_FALSE(g2048::canMove(stuck));
  Board pair = stuck;
  pair.cells[15] = 2;  // matches its left neighbour
  EXPECT_TRUE(g2048::canMove(pair));
  Board hole = stuck;
  hole.cells[5] = 0;
  EXPECT_TRUE(g2048::canMove(hole));
}

TEST(Game2048Board, MaxTilesNeverMerge) {
  Board board;
  board.cells[0] = g2048::MAX_EXP;
  board.cells[1] = g2048::MAX_EXP;
  const auto result = g2048::slide(board, Dir::LEFT);
  EXPECT_FALSE(result.moved);
  EXPECT_EQ(result.board.cells[0], g2048::MAX_EXP);
}

TEST(Game2048Board, SpawnFillsOnlyEmptyCells) {
  Board board = make({1, 1, 1, 1,  //
                      1, 1, 1, 1,  //
                      1, 1, 0, 1,  //
                      1, 1, 1, 1});
  EXPECT_EQ(g2048::spawn(board, 12345u), 10);
  EXPECT_NE(board.cells[10], 0);
  EXPECT_EQ(g2048::spawn(board, 99u), -1);
}

TEST(Game2048Board, SpawnsMostlyTwos) {
  uint32_t state = 0x2048u;
  int fours = 0;
  constexpr int ROUNDS = 5000;
  for (int i = 0; i < ROUNDS; i++) {
    Board board;
    const int cell = g2048::spawn(board, g2048::nextRandom(state));
    ASSERT_GE(cell, 0);
    fours += board.cells[cell] == 2 ? 1 : 0;
  }
  EXPECT_GT(fours, ROUNDS / 20);
  EXPECT_LT(fours, ROUNDS / 6);
}
