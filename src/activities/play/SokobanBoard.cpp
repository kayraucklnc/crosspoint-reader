#include "SokobanBoard.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace sokoban {
namespace {
constexpr const char* PACK_PATH = "/.crosspoint/games/sokoban/levels.bin";
constexpr int PACK_HEADER_FIXED = 7;  // "SKB1"/"SKB2", u16 count, u8 name length
constexpr int LEVEL_HEADER = 6;       // u8 w, u8 h, u16 par moves, u16 par pushes
constexpr int PUSH_BIT = 4;
constexpr int DX[4] = {0, 1, 0, -1};
constexpr int DY[4] = {-1, 0, 1, 0};

// Microban 1-3 by David W. Skinner, used when the SD pack is missing.
struct BuiltIn {
  const char* xsb;
  const char* solution;  // LURD, pushes in capitals
  uint16_t parMoves;
  uint16_t parPushes;
};
constexpr BuiltIn BUILT_IN[] = {
    {"####|# .#|#  ###|#*@  #|#  $ #|#  ###|####", "dlUrrrdLullddrUluRuulDrddrruLdlUU", 33, 8},
    {"######|#    #|# #@ #|# $* #|# .* #|#    #|######", "rddLruulDuullddR", 16, 3},
    {"  ####|###  ####|#     $ #|# #  #$ #|# . .#@ #|#########", "ruuLLLulDrrrrddlUruLLLddllluurRDrdLuuurDD", 41, 13},
};
constexpr int BUILT_IN_COUNT = sizeof(BUILT_IN) / sizeof(BUILT_IN[0]);

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// Pack cell code (0 outside .. 7 player on goal) to cell bits.
constexpr uint8_t CODE_BITS[8] = {0,
                                  CELL_WALL,
                                  CELL_FLOOR,
                                  CELL_FLOOR | CELL_GOAL,
                                  CELL_FLOOR | CELL_BOX,
                                  CELL_FLOOR | CELL_GOAL | CELL_BOX,
                                  CELL_FLOOR,
                                  CELL_FLOOR | CELL_GOAL};
}  // namespace

void Board::reset(const int width, const int height) {
  w = static_cast<uint8_t>(width);
  h = static_cast<uint8_t>(height);
  memset(cells, 0, sizeof(cells));
  historyHead = historyCount = 0;
  moves = pushes = 0;
  boxesOffGoal = 0;
  facing = DOWN;
}

bool Board::setCode(const int cell, const uint8_t code) {
  if (code > 7) return false;
  cells[cell] = CODE_BITS[code];
  if (code >= 6) player = static_cast<uint16_t>(cell);
  return code >= 6;
}

bool Board::finishLoad(const bool sawPlayer) {
  int boxes = 0, goals = 0;
  for (int i = 0; i < w * h; i++) {
    boxes += (cells[i] & CELL_BOX) != 0;
    goals += (cells[i] & CELL_GOAL) != 0;
    if ((cells[i] & (CELL_BOX | CELL_GOAL)) == CELL_BOX) boxesOffGoal++;
  }
  if (!sawPlayer || boxes == 0 || boxes != goals) {
    LOG_ERR("SOKO", "Malformed level: player %d, boxes %d, goals %d", sawPlayer, boxes, goals);
    return false;
  }
  markDeadCells();
  return true;
}

// Pull analysis: a cell is live if a box there can be pushed onto some goal
// with no other box in the way. Relaxation passes need no queue memory.
void Board::markDeadCells() {
  const int total = w * h;
  for (int i = 0; i < total; i++) {
    if (cells[i] & CELL_FLOOR) cells[i] = static_cast<uint8_t>(cells[i] | (cells[i] & CELL_GOAL ? 0 : CELL_DEAD));
  }
  const auto inside = [this](const int c) { return c >= 0 && (cells[c] & CELL_FLOOR) && !(cells[c] & CELL_WALL); };
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < total; i++) {
      if (!inside(i) || (cells[i] & CELL_DEAD)) continue;
      for (int d = 0; d < 4; d++) {
        const int from = neighbour(i, static_cast<Dir>(d));
        const int keeper = from < 0 ? -1 : neighbour(from, static_cast<Dir>(d));
        if (inside(from) && inside(keeper) && (cells[from] & CELL_DEAD)) {
          cells[from] &= static_cast<uint8_t>(~CELL_DEAD);
          changed = true;
        }
      }
    }
  }
}

bool Board::walkable(const int cell) const {
  return cell >= 0 && (cells[cell] & (CELL_FLOOR | CELL_WALL | CELL_BOX)) == CELL_FLOOR;
}

uint32_t Board::boxHash() const {
  uint32_t hash = 0;
  for (int i = 0; i < w * h; i++) {
    if (!(cells[i] & CELL_BOX)) continue;
    uint32_t x = static_cast<uint32_t>(i + 1) * 0x9E3779B1u;
    x ^= x >> 15;
    x *= 0x85EBCA77u;
    x ^= x >> 13;
    hash ^= x;
  }
  return hash;
}

// A box in a 2x2 block made only of walls and boxes can never move again.
bool Board::frozenAt(const int cell) const {
  const auto solid = [this](const int c) {
    return c < 0 || !(cells[c] & CELL_FLOOR) || (cells[c] & (CELL_WALL | CELL_BOX));
  };
  for (int d = 0; d < 4; d++) {
    const int a = neighbour(cell, static_cast<Dir>(d));
    const int b = neighbour(cell, static_cast<Dir>((d + 1) % 4));
    const int corner = a < 0 ? -1 : neighbour(a, static_cast<Dir>((d + 1) % 4));
    if (solid(a) && solid(b) && solid(corner)) return true;
  }
  return false;
}

bool Board::stuck() const {
  for (int i = 0; i < w * h; i++) {
    if ((cells[i] & (CELL_BOX | CELL_GOAL)) != CELL_BOX) continue;
    if ((cells[i] & CELL_DEAD) || frozenAt(i)) return true;
  }
  return false;
}

bool Board::loadCodes(const int width, const int height, const uint8_t* packed) {
  if (width < 3 || height < 3 || width > MAX_SIDE || height > MAX_SIDE || width * height > MAX_CELLS) return false;
  reset(width, height);
  bool sawPlayer = false;
  for (int i = 0; i < width * height; i++) {
    const uint8_t byte = packed[i / 2];
    sawPlayer |= setCode(i, (i % 2 == 0) ? byte >> 4 : byte & 0x0F);
  }
  return finishLoad(sawPlayer);
}

bool Board::loadText(const char* xsb) {
  int width = 0, height = 1, run = 0;
  for (const char* p = xsb; *p; p++) {
    if (*p == '|') {
      height++;
      run = 0;
    } else {
      width = std::max(width, ++run);
    }
  }
  if (width < 3 || height < 3 || width > MAX_SIDE || height > MAX_SIDE || width * height > MAX_CELLS) return false;
  reset(width, height);
  bool sawPlayer = false;
  int x = 0, y = 0;
  for (const char* p = xsb; *p; p++) {
    if (*p == '|') {
      x = 0;
      y++;
      continue;
    }
    static constexpr char CHARS[] = " #  $*@+";  // index = pack code; '.' handled below
    uint8_t code = 0;
    if (*p == '.') {
      code = 3;
    } else if (*p != ' ') {
      const char* hit = strchr(CHARS + 1, *p);
      code = hit ? static_cast<uint8_t>(hit - CHARS) : 0;
    }
    sawPlayer |= setCode(y * width + x, code);
    x++;
  }
  // Blanks inside the walls are floor: flood from the player. The built-in
  // levels are tiny, so a small fixed stack covers them.
  const int total = w * h;
  for (int i = 0; i < total; i++) cells[i] &= static_cast<uint8_t>(~CELL_FLOOR);
  uint16_t stack[64];
  int top = 0;
  cells[player] |= CELL_FLOOR;
  stack[top++] = player;
  while (top > 0) {
    const int cell = stack[--top];
    for (int d = 0; d < 4; d++) {
      const int n = neighbour(cell, static_cast<Dir>(d));
      if (n < 0 || n >= total || (cells[n] & (CELL_WALL | CELL_FLOOR))) continue;
      cells[n] |= CELL_FLOOR;
      if (top < 64) stack[top++] = static_cast<uint16_t>(n);
    }
  }
  return finishLoad(sawPlayer);
}

void Board::transpose() {
  // Cycle-following in-place transpose; `done` marks visited cells.
  uint8_t done[MAX_CELLS / 8] = {};
  const int total = w * h;
  for (int start = 0; start < total; start++) {
    if (done[start / 8] & (1 << (start % 8))) continue;
    int cell = start;
    uint8_t carry = cells[start];
    do {
      // Value at (x, y) moves to (y, x) in the h-wide grid.
      const int next = (cell % w) * h + cell / w;
      std::swap(carry, cells[next]);
      done[next / 8] |= static_cast<uint8_t>(1 << (next % 8));
      cell = next;
    } while (cell != start);
  }
  player = static_cast<uint16_t>((player % w) * h + player / w);
  std::swap(w, h);
}

int Board::neighbour(const int cell, const Dir dir) const {
  const int x = cell % w + DX[dir];
  const int y = cell / w + DY[dir];
  if (x < 0 || x >= w || y < 0 || y >= h) return -1;
  return y * w + x;
}

uint8_t Board::visual(const int cell) const {
  const uint8_t bits = cells[cell] & static_cast<uint8_t>(~CELL_DEAD);
  if (cell != player) return bits;
  return static_cast<uint8_t>(bits | CELL_PLAYER | (facing << FACING_SHIFT));
}

uint8_t Board::openSides(const int cell) const {
  uint8_t sides = 0;
  for (int d = 0; d < 4; d++) {
    const int n = neighbour(cell, static_cast<Dir>(d));
    if (n < 0 || !(cells[n] & CELL_WALL)) sides |= static_cast<uint8_t>(1 << d);
    const int diagonal = n < 0 ? -1 : neighbour(n, static_cast<Dir>((d + 1) % 4));
    if (diagonal < 0 || !(cells[diagonal] & CELL_WALL)) sides |= static_cast<uint8_t>(0x10 << d);
  }
  return sides;
}

void Board::record(const uint8_t entry) {
  uint8_t& byte = history[historyHead / 2];
  byte = (historyHead % 2 == 0) ? static_cast<uint8_t>((byte & 0x0F) | (entry << 4))
                                : static_cast<uint8_t>((byte & 0xF0) | entry);
  historyHead = (historyHead + 1) % HISTORY_MOVES;
  historyCount = std::min(historyCount + 1, HISTORY_MOVES);
}

Board::Step Board::move(const Dir dir) {
  facing = dir;
  const int next = neighbour(player, dir);
  if (next < 0 || (cells[next] & CELL_WALL) || !(cells[next] & CELL_FLOOR)) return Step::BLOCKED;
  bool pushed = false;
  if (cells[next] & CELL_BOX) {
    const int beyond = neighbour(next, dir);
    if (beyond < 0 || (cells[beyond] & (CELL_WALL | CELL_BOX)) || !(cells[beyond] & CELL_FLOOR)) {
      return Step::BLOCKED;
    }
    cells[next] &= static_cast<uint8_t>(~CELL_BOX);
    cells[beyond] |= CELL_BOX;
    if (cells[next] & CELL_GOAL) boxesOffGoal++;
    if (cells[beyond] & CELL_GOAL) boxesOffGoal--;
    pushed = true;
  }
  player = static_cast<uint16_t>(next);
  if (moves < UINT16_MAX) moves++;
  if (pushed && pushes < UINT16_MAX) pushes++;
  record(static_cast<uint8_t>(dir | (pushed ? PUSH_BIT : 0)));
  return pushed ? Step::PUSHED : Step::WALKED;
}

bool Board::lastWasPush() const {
  if (historyCount == 0) return false;
  const int last = (historyHead + HISTORY_MOVES - 1) % HISTORY_MOVES;
  const uint8_t byte = history[last / 2];
  return (((last % 2 == 0) ? byte >> 4 : byte & 0x0F) & PUSH_BIT) != 0;
}

Board::Step Board::undo() {
  if (historyCount == 0) return Step::BLOCKED;
  historyHead = (historyHead + HISTORY_MOVES - 1) % HISTORY_MOVES;
  historyCount--;
  const uint8_t byte = history[historyHead / 2];
  const uint8_t entry = (historyHead % 2 == 0) ? byte >> 4 : byte & 0x0F;
  const Dir dir = static_cast<Dir>(entry & 3);
  const int from = neighbour(player, static_cast<Dir>((dir + 2) % 4));
  if (entry & PUSH_BIT) {
    const int box = neighbour(player, dir);
    if (box >= 0) {
      cells[box] &= static_cast<uint8_t>(~CELL_BOX);
      cells[player] |= CELL_BOX;
      if (cells[box] & CELL_GOAL) boxesOffGoal++;
      if (cells[player] & CELL_GOAL) boxesOffGoal--;
    }
    if (pushes > 0) pushes--;
  }
  if (from >= 0) player = static_cast<uint16_t>(from);
  if (moves > 0) moves--;
  facing = dir;
  return (entry & PUSH_BIT) ? Step::PUSHED : Step::WALKED;
}

void LevelPack::open() {
  fallback = true;
  levelCount = BUILT_IN_COUNT;
  packName[0] = '\0';
  HalFile file;
  if (!Storage.exists(PACK_PATH) || !Storage.openFileForRead("SOKO", PACK_PATH, file)) {
    LOG_INF("SOKO", "No level pack, using %d built-in levels", BUILT_IN_COUNT);
    return;
  }
  uint8_t head[PACK_HEADER_FIXED];
  if (file.read(head, sizeof(head)) != static_cast<int>(sizeof(head)) ||
      (memcmp(head, "SKB1", 4) != 0 && memcmp(head, "SKB2", 4) != 0)) {
    LOG_ERR("SOKO", "Bad level pack header");
    return;
  }
  hasSolutions = head[3] == '2';
  const int count = readU16(head + 4);
  const int nameLength = std::min<int>(head[6], sizeof(packName) - 1);
  if (count == 0 || file.read(packName, nameLength) != nameLength) {
    LOG_ERR("SOKO", "Bad level pack index");
    packName[0] = '\0';
    return;
  }
  packName[nameLength] = '\0';
  levelCount = count;
  fallback = false;
  LOG_INF("SOKO", "Level pack %s: %d levels", packName, levelCount);
}

bool LevelPack::load(const int index, Board& board, uint16_t& parMoves, uint16_t& parPushes) const {
  parMoves = parPushes = 0;
  if (index < 0 || index >= levelCount) return false;
  if (fallback) {
    parMoves = BUILT_IN[index].parMoves;
    parPushes = BUILT_IN[index].parPushes;
    return board.loadText(BUILT_IN[index].xsb);
  }
  HalFile file;
  if (!Storage.openFileForRead("SOKO", PACK_PATH, file)) return false;
  uint8_t head[PACK_HEADER_FIXED];
  uint8_t word[4];
  if (file.read(head, sizeof(head)) != static_cast<int>(sizeof(head)) ||
      !file.seek(PACK_HEADER_FIXED + head[6] + 4 * static_cast<size_t>(index)) || file.read(word, 4) != 4 ||
      !file.seek(readU32(word))) {
    LOG_ERR("SOKO", "Level %d: bad offset", index);
    return false;
  }
  uint8_t level[LEVEL_HEADER];
  if (file.read(level, sizeof(level)) != static_cast<int>(sizeof(level))) return false;
  const int width = level[0], height = level[1];
  if (width * height > MAX_CELLS) {
    LOG_ERR("SOKO", "Level %d: bad size %dx%d", index, width, height);
    return false;
  }
  // At most 512 bytes, released as soon as the board has decoded it.
  const int bytes = (width * height + 1) / 2;
  auto packed = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!packed) {
    LOG_ERR("SOKO", "OOM: level %d bytes", bytes);
    return false;
  }
  if (file.read(packed.get(), bytes) != bytes) return false;
  parMoves = readU16(level + 2);
  parPushes = readU16(level + 4);
  return board.loadCodes(width, height, packed.get());
}

bool LevelPack::loadSolution(const int index, std::unique_ptr<uint8_t[]>& packed, int& moveCount) const {
  moveCount = 0;
  if (index < 0 || index >= levelCount) return false;
  if (fallback) {
    const char* lurd = BUILT_IN[index].solution;
    const int n = static_cast<int>(strlen(lurd));
    packed = makeUniqueNoThrow<uint8_t[]>((n + 3) / 4);
    if (!packed) return false;
    memset(packed.get(), 0, (n + 3) / 4);
    for (int k = 0; k < n; k++) {
      static constexpr char KEYS[] = "urdl";
      const char* hit = strchr(KEYS, lurd[k] | 0x20);
      const int dir = hit ? static_cast<int>(hit - KEYS) : 0;
      packed[k / 4] |= static_cast<uint8_t>(dir << (6 - 2 * (k % 4)));
    }
    moveCount = n;
    return true;
  }
  if (!hasSolutions) return false;
  HalFile file;
  if (!Storage.openFileForRead("SOKO", PACK_PATH, file)) return false;
  uint8_t head[PACK_HEADER_FIXED];
  uint8_t word[4];
  uint8_t level[LEVEL_HEADER];
  if (file.read(head, sizeof(head)) != static_cast<int>(sizeof(head)) ||
      !file.seek(PACK_HEADER_FIXED + head[6] + 4 * static_cast<size_t>(index)) || file.read(word, 4) != 4 ||
      !file.seek(readU32(word)) || file.read(level, sizeof(level)) != static_cast<int>(sizeof(level)) ||
      !file.seekCur((level[0] * level[1] + 1) / 2) || file.read(word, 2) != 2) {
    LOG_ERR("SOKO", "Level %d: no solution record", index);
    return false;
  }
  const int n = readU16(word);
  const int bytes = (n + 3) / 4;
  packed = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!packed) {
    LOG_ERR("SOKO", "OOM: solution %d bytes", bytes);
    return false;
  }
  if (n == 0 || file.read(packed.get(), bytes) != bytes) {
    packed.reset();
    return false;
  }
  moveCount = n;
  return true;
}

}  // namespace sokoban
