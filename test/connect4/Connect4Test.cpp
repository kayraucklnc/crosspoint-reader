#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <vector>

#include "activities/play/Connect4Board.h"
#include "activities/play/Connect4Brain.h"

using c4::Board;
using c4::Search;

namespace {

// Plays a string of column digits ('0'-'6'); every move must be legal.
Board playMoves(const char* moves, const c4::Side first = c4::HUMAN) {
  Board board;
  board.reset(first);
  for (const char* p = moves; *p; p++) {
    EXPECT_GE(board.play(*p - '0'), 0) << "illegal move in " << moves;
  }
  return board;
}

int popcount(uint64_t bits) {
  int n = 0;
  for (; bits; bits &= bits - 1) n++;
  return n;
}

// Depth-first fill that never completes four: a drawn full board.
bool fillWithoutFour(Board& board) {
  if (board.full()) return true;
  for (int col = 0; col < c4::COLS; col++) {
    if (board.play(col) < 0) continue;
    if (board.winner() < 0 && fillWithoutFour(board)) return true;
    board.undo();
  }
  return false;
}

int searchBest(const Board& board, const int depth) {
  std::vector<c4::TTEntry> table(c4::TT_SIZE);
  Search search(table.data());
  search.clearTable();
  return search.bestMove(board.position(), depth, 0, nullptr, nullptr, nullptr);
}

}  // namespace

// ---- rules -----------------------------------------------------------------------

TEST(Connect4Board, StartsEmptyWithTheChosenSide) {
  Board board;
  board.reset(c4::COMPUTER);
  EXPECT_EQ(board.toMove(), c4::COMPUTER);
  EXPECT_EQ(board.moveCount(), 0);
  EXPECT_EQ(board.winner(), -1);
  EXPECT_FALSE(board.over());
  for (int col = 0; col < c4::COLS; col++) EXPECT_TRUE(board.canPlay(col));
}

TEST(Connect4Board, DiscsStackFromTheBottomAndAlternate) {
  Board board = playMoves("334");
  EXPECT_EQ(board.cell(3, 0), c4::HUMAN);
  EXPECT_EQ(board.cell(3, 1), c4::COMPUTER);
  EXPECT_EQ(board.cell(4, 0), c4::HUMAN);
  EXPECT_EQ(board.cell(4, 1), -1);
  EXPECT_EQ(board.lastColumn(), 4);
  EXPECT_EQ(board.lastRow(), 0);
  EXPECT_EQ(board.toMove(), c4::COMPUTER);
}

TEST(Connect4Board, VerticalFourWins) {
  Board board = playMoves("0101010");
  EXPECT_EQ(board.winner(), c4::HUMAN);
  EXPECT_TRUE(board.over());
  EXPECT_EQ(board.winLine(), c4::cellBit(0, 0) | c4::cellBit(0, 1) | c4::cellBit(0, 2) | c4::cellBit(0, 3));
  EXPECT_LT(board.play(5), 0) << "no moves after the game is decided";
}

TEST(Connect4Board, HorizontalFourWins) {
  Board board = playMoves("0011223");
  EXPECT_EQ(board.winner(), c4::HUMAN);
  EXPECT_EQ(popcount(board.winLine()), 4);
  EXPECT_TRUE(board.winLine() & c4::cellBit(3, 0));
}

TEST(Connect4Board, RisingDiagonalWins) {
  // Human builds (0,0) (1,1) (2,2) (3,3).
  Board board = playMoves("0112232336");
  ASSERT_EQ(board.winner(), -1);
  board.play(3);
  EXPECT_EQ(board.winner(), c4::HUMAN);
  EXPECT_EQ(board.winLine(), c4::cellBit(0, 0) | c4::cellBit(1, 1) | c4::cellBit(2, 2) | c4::cellBit(3, 3));
}

TEST(Connect4Board, FallingDiagonalWinsForTheComputer) {
  // Computer builds (3,0) (2,1) (1,2) (0,3); it moves second.
  Board board = playMoves("23120611060");
  ASSERT_EQ(board.winner(), -1);
  board.play(0);
  EXPECT_EQ(board.winner(), c4::COMPUTER);
  EXPECT_EQ(board.winLine(), c4::cellBit(3, 0) | c4::cellBit(2, 1) | c4::cellBit(1, 2) | c4::cellBit(0, 3));
}

TEST(Connect4Board, LinesDoNotWrapBetweenColumns) {
  // Top three of column 0 and the bottom of column 1 are consecutive bit
  // indices only without the sentinel row.
  const uint64_t stones = c4::cellBit(0, 3) | c4::cellBit(0, 4) | c4::cellBit(0, 5) | c4::cellBit(1, 0);
  EXPECT_FALSE(c4::hasFour(stones));
  // Row 5 of columns 0-3 is a real line.
  EXPECT_TRUE(c4::hasFour(c4::cellBit(0, 5) | c4::cellBit(1, 5) | c4::cellBit(2, 5) | c4::cellBit(3, 5)));
}

TEST(Connect4Board, FullColumnRejectsDiscs) {
  Board board = playMoves("000000");
  EXPECT_FALSE(board.canPlay(0));
  EXPECT_LT(board.play(0), 0);
  EXPECT_EQ(board.moveCount(), 6);
}

TEST(Connect4Board, UndoRestoresTheWinAndTurn) {
  Board board = playMoves("0101010");
  ASSERT_TRUE(board.undo());
  EXPECT_EQ(board.winner(), -1);
  EXPECT_EQ(board.winLine(), 0u);
  EXPECT_EQ(board.toMove(), c4::HUMAN);
  EXPECT_EQ(board.cell(0, 3), -1);
  Board empty;
  empty.reset(c4::HUMAN);
  EXPECT_FALSE(empty.undo());
}

TEST(Connect4Board, FullBoardWithoutFourIsADraw) {
  Board board;
  board.reset(c4::HUMAN);
  ASSERT_TRUE(fillWithoutFour(board));
  EXPECT_TRUE(board.full());
  EXPECT_TRUE(board.over());
  EXPECT_EQ(board.winner(), -1);
  for (int col = 0; col < c4::COLS; col++) EXPECT_FALSE(board.canPlay(col));
}

TEST(Connect4Board, WinningCellsFindsTheGapInABrokenThree) {
  const uint64_t stones = c4::cellBit(1, 0) | c4::cellBit(2, 0) | c4::cellBit(4, 0);
  EXPECT_EQ(c4::winningCells(stones, stones), c4::cellBit(3, 0));
}

// ---- computer player ---------------------------------------------------------------

TEST(Connect4Brain, TakesAWinInOne) {
  // The computer has three stacked in column 5 and moves.
  const Board mine = playMoves("0515056", c4::HUMAN);
  ASSERT_EQ(mine.toMove(), c4::COMPUTER);
  for (const int depth : {1, 4, 12}) EXPECT_EQ(searchBest(mine, depth), 5) << "depth " << depth;
}

TEST(Connect4Brain, BlocksALossInOne) {
  // Human has 1,2,3 on the bottom row and 4 is the computer's: only 0 saves it.
  const Board board = playMoves("14243", c4::HUMAN);
  ASSERT_EQ(board.toMove(), c4::COMPUTER);
  for (const int depth : {1, 4, 12}) EXPECT_EQ(searchBest(board, depth), 0) << "depth " << depth;
}

TEST(Connect4Brain, DoesNotPlayUnderAThreat) {
  // Human holds (2,1) (3,1) (4,1) with (1,1) blocked, so (5,1) wins for them
  // as soon as column 5 gets a floor. The computer has nothing better to do
  // and must leave column 5 alone.
  const Board board = playMoves("2342611364", c4::COMPUTER);
  ASSERT_EQ(board.toMove(), c4::COMPUTER);
  ASSERT_EQ(board.winner(), -1);
  for (const int depth : {1, 4, 10}) EXPECT_NE(searchBest(board, depth), 5) << "depth " << depth;
}

TEST(Connect4Brain, FindsTheOpenThreeDoubleThreat) {
  // Human has 2 and 3 on an empty bottom row: 1 or 4 makes an unstoppable
  // open three.
  const Board board = playMoves("2636", c4::HUMAN);
  ASSERT_EQ(board.toMove(), c4::HUMAN);
  std::vector<c4::TTEntry> table(c4::TT_SIZE);
  Search search(table.data());
  const int move = search.bestMove(board.position(), 8, 0, nullptr, nullptr, nullptr);
  EXPECT_TRUE(move == 1 || move == 4) << move;
  EXPECT_GE(search.lastScore, c4::WIN - c4::CELLS);
}

TEST(Connect4Brain, WorksWithoutATable) {
  const Board board = playMoves("14243", c4::HUMAN);
  Search search(nullptr);
  EXPECT_EQ(search.bestMove(board.position(), 6, 0, nullptr, nullptr, nullptr), 0);
}

TEST(Connect4Brain, OpensInTheCentre) {
  Board board;
  board.reset(c4::COMPUTER);
  EXPECT_EQ(searchBest(board, 8), 3);
}

TEST(Connect4Brain, AbortStillReturnsALegalMove) {
  Board board;
  board.reset(c4::HUMAN);
  std::vector<c4::TTEntry> table(c4::TT_SIZE);
  Search search(table.data());
  std::atomic<bool> abort{true};
  const int move = search.bestMove(board.position(), c4::CELLS, 0, nullptr, &abort, nullptr);
  EXPECT_GE(move, 0);
  EXPECT_LT(move, c4::COLS);
}

TEST(Connect4Brain, NoMoveOnAFinishedBoard) {
  Board board;
  board.reset(c4::HUMAN);
  ASSERT_TRUE(fillWithoutFour(board));
  Search search(nullptr);
  EXPECT_EQ(search.bestMove(board.position(), 4, 0, nullptr, nullptr, nullptr), -1);
}

TEST(Connect4Brain, EasyIsSometimesRandomButAlwaysLegal) {
  const Board board = playMoves("000000");  // column 0 full
  std::vector<c4::TTEntry> table(c4::TT_SIZE);
  Search search(table.data());
  uint32_t rng = 12345;
  bool sawNonCentre = false;
  for (int i = 0; i < 200; i++) {
    const int move = search.choose(board.position(), c4::LEVELS[0], nullptr, nullptr, rng);
    ASSERT_GE(move, 1);
    ASSERT_LT(move, c4::COLS);
    sawNonCentre = sawNonCentre || move != 3;
  }
  EXPECT_TRUE(sawNonCentre);
}

TEST(Connect4Brain, BeatsARandomPlayer) {
  std::vector<c4::TTEntry> table(c4::TT_SIZE);
  Search search(table.data());
  uint32_t rng = 99;
  int wins = 0;
  constexpr int GAMES = 20;
  for (int game = 0; game < GAMES; game++) {
    Board board;
    board.reset(game % 2 ? c4::HUMAN : c4::COMPUTER);
    while (!board.over()) {
      int col;
      if (board.toMove() == c4::COMPUTER) {
        col = search.bestMove(board.position(), 6, 0, nullptr, nullptr, nullptr);
      } else {
        do {
          col = static_cast<int>(c4::nextRandom(rng) % c4::COLS);
        } while (!board.canPlay(col));
      }
      ASSERT_GE(board.play(col), 0);
    }
    wins += board.winner() == c4::COMPUTER ? 1 : 0;
  }
  EXPECT_EQ(wins, GAMES);
}

TEST(Connect4Brain, EvaluationIsZeroSum) {
  const Board board = playMoves("33425");
  const c4::Position pos = board.position();
  const uint64_t opponent = pos.current ^ pos.mask;
  EXPECT_EQ(c4::evaluate(pos.current, pos.mask), -c4::evaluate(opponent, pos.mask));
}
