#pragma once

#include <cstdint>

// Sokoban rules and level loading, independent of drawing.
namespace sokoban {

constexpr int MAX_SIDE = 40;
constexpr int MAX_CELLS = 1024;
// Undo depth; entries are 4-bit so the ring costs HISTORY_MOVES / 2 bytes.
constexpr int HISTORY_MOVES = 4096;

// Static and dynamic cell bits. PLAYER and the facing bits only appear in
// Board::visual(), never in the stored cells.
enum : uint8_t { CELL_WALL = 1, CELL_GOAL = 2, CELL_BOX = 4, CELL_FLOOR = 8, CELL_PLAYER = 16 };
constexpr int FACING_SHIFT = 5;

enum Dir : uint8_t { UP = 0, RIGHT = 1, DOWN = 2, LEFT = 3 };

class Board {
 public:
  enum class Step : uint8_t { BLOCKED, WALKED, PUSHED };

  // Parses a level from the pack's 4-bit cell codes (see scripts/gen_sokoban.py).
  bool loadCodes(int width, int height, const uint8_t* packed);
  // Parses a level from XSB rows separated by '|'.
  bool loadText(const char* xsb);
  // Swaps rows and columns; the puzzle is unchanged. Only valid before a move.
  void transpose();

  Step move(Dir dir);
  bool undo();
  bool solved() const { return boxesOffGoal == 0; }
  bool canUndo() const { return historyCount > 0; }

  int width() const { return w; }
  int height() const { return h; }
  int playerCell() const { return player; }
  int moveCount() const { return moves; }
  int pushCount() const { return pushes; }
  // Cell bits plus, on the player's cell, CELL_PLAYER and the facing.
  uint8_t visual(int cell) const;
  // Bit (1 << Dir) set where the neighbour is not a wall (or off the board);
  // bit (0x10 << Dir) likewise for the diagonal clockwise of Dir.
  uint8_t openSides(int cell) const;

 private:
  uint8_t cells[MAX_CELLS] = {};
  uint8_t history[HISTORY_MOVES / 2] = {};
  int historyHead = 0;  // next write slot
  int historyCount = 0;
  uint8_t w = 0;
  uint8_t h = 0;
  uint16_t player = 0;
  uint8_t facing = DOWN;
  uint16_t boxesOffGoal = 0;
  uint16_t moves = 0;
  uint16_t pushes = 0;

  void reset(int width, int height);
  bool setCode(int cell, uint8_t code);
  bool finishLoad(bool sawPlayer);
  int neighbour(int cell, Dir dir) const;
  void record(uint8_t entry);
};

// Level pack on SD (/.crosspoint/games/sokoban/levels.bin) with a tiny
// built-in fallback. Levels are read one at a time; only the header is kept.
class LevelPack {
 public:
  // Reads the pack header; falls back to the built-in levels when the file is
  // missing or malformed.
  void open();
  bool builtIn() const { return fallback; }
  int count() const { return levelCount; }
  const char* name() const { return packName; }
  // Loads level `index` (0-based). Par values are 0 when unknown.
  bool load(int index, Board& board, uint16_t& parMoves, uint16_t& parPushes) const;

 private:
  bool fallback = true;
  int levelCount = 0;
  char packName[24] = {};
};

}  // namespace sokoban
