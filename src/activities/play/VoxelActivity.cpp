#include "VoxelActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/WallpaperStore.h"

namespace {
constexpr int N = VoxelActivity::N;
constexpr int H = VoxelActivity::H;
constexpr int HALF = 14;    // half a tile's width
constexpr int QUARTER = 7;  // half a tile's height
constexpr int CUBE = 16;    // cube side height
constexpr int SPAN = 2 * HALF;  // top-face local coordinate range
constexpr int SLAB = 5;         // bridge plank thickness
constexpr int HEADER = 62;
constexpr int GROUND = 9;  // below this, side faces darken toward the island's underside
constexpr unsigned long HOLD_MS = 650;
constexpr const char* SAVE_PATH = "/.crosspoint/skyisland.bin";
constexpr int SAVE_HEADER = 12;

enum Material : uint8_t { AIR, GRASS, DIRT, STONE, SAND, WOOD, LEAVES, WATER, PLANK, GLASS, LANTERN, FLOWER, COUNT };
// Dither level (of 16) per face: top, left (lit), right (shade).
constexpr uint8_t LEVELS[COUNT][3] = {{0, 0, 0},  {0, 4, 9},  {2, 5, 9},  {0, 4, 9}, {1, 2, 6},   {1, 6, 11},
                                      {1, 5, 10}, {0, 7, 12}, {0, 5, 10}, {0, 0, 2}, {12, 12, 14}, {0, 0, 0}};
constexpr StrId NAMES[COUNT] = {StrId::STR_VX_GRASS, StrId::STR_VX_GRASS,  StrId::STR_VX_DIRT,  StrId::STR_VX_STONE,
                                StrId::STR_VX_SAND,  StrId::STR_VX_WOOD,   StrId::STR_VX_LEAVES, StrId::STR_VX_WATER,
                                StrId::STR_VX_PLANK, StrId::STR_VX_GLASS,  StrId::STR_VX_LANTERN, StrId::STR_VX_FLOWER};
constexpr StrId MENU[] = {StrId::STR_VX_RESUME, StrId::STR_VX_ROTATE, StrId::STR_VX_NEW, StrId::STR_VX_WALLPAPER,
                          StrId::STR_VX_EXIT};
constexpr uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

bool solid(const uint8_t m) { return m != AIR && m != PLANK && m != LANTERN && m != FLOWER; }

uint32_t hash2(const int x, const int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

bool bayer(const int level, const int x, const int y) { return BAYER[y & 3][x & 3] < level; }
bool grain(const int level, const int x, const int y) { return static_cast<int>(hash2(x, y) & 15) < level; }

// Smooth value noise in [0, 1).
float noise(const float x, const float y, const uint32_t seed) {
  const int ix = static_cast<int>(floorf(x)), iy = static_cast<int>(floorf(y));
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  const auto v = [seed](const int a, const int b) { return (hash2(a + seed * 31, b - seed * 17) & 0xFFFF) / 65536.0f; };
  const float top = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * fx;
  const float low = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * fx;
  return top + (low - top) * fy;
}

// Ink for one pixel of a face. (u, v) are face-local: top faces use the two
// iso axes (0..24), side faces use column (0..11) and row from the top edge.
bool ink(const uint8_t m, const int face, const int x, const int y, const int u, const int v, const int extra) {
  const int level = std::min(16, LEVELS[m][face] + extra);
  const bool tone = bayer(level, x, y);
  switch (m) {
    case GRASS:
      // Tops: light turf with short blade marks; sides: a dark turf lip over dirt.
      if (face == 0) {
        // Sparse regular dots with little "v" tufts of grass.
        const int tx = x % 6, ty = y % 4;
        const bool tuft = (hash2(x / 6, y / 4) & 7) == 0 && ((ty == 1 && (tx == 1 || tx == 3)) || (ty == 2 && tx == 2));
        return tuft || bayer(level + 1, x, y);
      }
      if (v < 3 + static_cast<int>(hash2(x, 7) % 3)) return bayer(level + 7, x, y);
      return bayer(LEVELS[DIRT][face] + extra, x, y) || (hash2(x, y) & 63) == 0;
    case STONE:
      if (face == 0) {
        const bool inner = u > 1 && v > 1 && u < SPAN - 1 && v < SPAN - 1;
        return (inner && (u % (SPAN / 2) == 0 || v % (SPAN / 2) == 0)) || tone;
      }
      return v % 6 == 0 || (u + (v / 6 % 2) * 7) % 14 == 0 || tone;
    case WOOD:
      if (face == 0) return std::max(std::abs(u - SPAN / 2), std::abs(v - SPAN / 2)) % 5 == 0 || tone;
      return u % 4 == 1 || tone;
    case LEAVES:
      // Leafy clumps: 2x2 blobs over a soft tone, a few bright glints on top.
      if (face == 0 && (hash2(x, y) & 15) == 0) return false;
      return tone || (hash2(x >> 1, y >> 1) & 7) == 0;
    case WATER:
      if (face == 0) return (y % 4 == 0 && (x + (y / 4) * 5) % 12 < 6) || bayer(2, x, y);
      return v % 5 != 0 && tone;
    case PLANK:
      if (face == 0) return v % 7 == 0 || tone;
      return tone || u % 7 == 0;
    case GLASS:
      if (face == 0) return (u == v + 8 || u == v + 11) && u > 10 && u < SPAN - 4;
      return u == v / 2 + 3 || tone;
    case SAND:
      return tone || (hash2(x, y) & 31) == 0;
    default:  // dirt, lantern
      return tone || (hash2(x, y) & 31) == 0;
  }
}

class Pen {
 public:
  Pen(const GfxRenderer& r, const int x0, const int y0, const int x1, const int y1)
      : r(r), x0(x0), y0(y0), x1(x1), y1(y1) {}
  void put(const int x, const int y, const bool black) const {
    if (x >= x0 && x <= x1 && y >= y0 && y <= y1) r.drawPixel(x, y, black);
  }
  // dash 0: solid black; dash < 0: solid white; dash > 0: black/white dashes.
  void line(int ax, int ay, const int bx, const int by, const int dash = 0) const {
    const int dx = std::abs(bx - ax), dy = -std::abs(by - ay);
    const int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    int err = dx + dy, k = 0;
    while (true) {
      put(ax, ay, dash == 0 || (dash > 0 && (k++ / dash) % 2 == 0));
      if (ax == bx && ay == by) return;
      const int e2 = 2 * err;
      if (e2 >= dy) err += dy, ax += sx;
      if (e2 <= dx) err += dx, ay += sy;
    }
  }

 private:
  const GfxRenderer& r;
  int x0, y0, x1, y1;
};
}  // namespace

// ---- world ---------------------------------------------------------------------

uint8_t VoxelActivity::get(const int x, const int y, const int z) const {
  if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= H) return AIR;
  return world[(z * N + y) * N + x];
}

void VoxelActivity::set(const int x, const int y, const int z, const uint8_t m) {
  if (x < 0 || y < 0 || z < 0 || x >= N || y >= N || z >= H) return;
  world[(z * N + y) * N + x] = m;
}

int VoxelActivity::columnTop(const int x, const int y) const {
  for (int z = H - 1; z >= 0; z--) {
    if (get(x, y, z) != AIR) return z;
  }
  return -1;
}

void VoxelActivity::generate(const uint32_t newSeed) {
  seed = newSeed;
  memset(world.get(), AIR, N * N * H);
  uint32_t rng = seed * 2654435761u | 1u;
  const auto rand = [&rng](const int n) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<int>(rng % static_cast<uint32_t>(n));
  };
  // Island body: noisy disc, domed top, tapering rocky underside.
  for (int y = 0; y < N; y++) {
    for (int x = 0; x < N; x++) {
      const float dx = x - 9.5f, dy = y - 9.5f;
      const float rim = 7.4f * (0.8f + 0.4f * noise(x * 0.35f, y * 0.35f, seed));
      const float d = sqrtf(dx * dx + dy * dy) / rim;
      if (d >= 1) continue;
      const float lift = (1 - d) * 2.2f + noise(x * 0.3f, y * 0.3f, seed + 1) * 2.4f - 1;
      const int top = std::clamp(GROUND + static_cast<int>(lroundf(lift)), 7, 12);
      const int depth = 1 + static_cast<int>(powf(1 - d, 1.4f) * 12 + noise(x * 0.5f, y * 0.5f, seed + 2) * 2.5f);
      for (int z = std::max(0, top - depth); z <= top; z++) {
        set(x, y, z, z == top ? GRASS : z >= top - 2 ? DIRT : STONE);
      }
    }
  }
  // Pond with a sandy rim, draining off the +x edge as a waterfall.
  const int px = 8 + rand(3), py = 8 + rand(3);
  const int level = std::max(1, columnTop(px, py) - 1);
  for (int y = py - 3; y <= py + 3; y++) {
    for (int x = px - 3; x <= px + 3; x++) {
      const float e = (x - px) * (x - px) / 5.0f + (y - py) * (y - py) / 2.6f;
      if (columnTop(x, y) < level) continue;
      if (e <= 1) {
        for (int z = level + 1; z < H; z++) set(x, y, z, AIR);
        set(x, y, level, WATER);
      } else if (e <= 2.6f && get(x, y, columnTop(x, y)) == GRASS) {
        set(x, y, columnTop(x, y), SAND);
      }
    }
  }
  for (int x = px + 1; x < N; x++) {
    const int top = columnTop(x, py);
    if (top < 0) {
      for (int z = level; z >= std::max(0, level - 9); z--) set(x, py, z, WATER);
      break;
    }
    if (top < level) break;
    for (int z = level + 1; z < H; z++) set(x, py, z, AIR);
    set(x, py, level, WATER);
  }
  // Trees, rocks, flowers and a lantern on free grass.
  int treeX[6], treeY[6], trees = 0;
  const int wantTrees = 2 + rand(3);
  for (int attempt = 0; attempt < 80 && trees < wantTrees; attempt++) {
    const int x = 2 + rand(N - 4), y = 2 + rand(N - 4), top = columnTop(x, y);
    if (top < 0 || get(x, y, top) != GRASS || std::abs(x - px) + std::abs(y - py) < 4) continue;
    bool crowded = false;
    for (int t = 0; t < trees; t++) crowded = crowded || std::abs(x - treeX[t]) + std::abs(y - treeY[t]) < 4;
    if (crowded) continue;
    treeX[trees] = x, treeY[trees++] = y;
    const int trunk = 3 + rand(2);
    for (int z = 1; z <= trunk; z++) set(x, y, top + z, WOOD);
    // Rounded crown: 3x3 below, a wide diamond in the middle, tapering to a tip.
    static constexpr int8_t CROWN[4] = {1, 2, 1, 0};
    for (int dz = -1; dz <= 2; dz++) {
      const int r = CROWN[dz + 1];
      for (int ly = -r; ly <= r; ly++) {
        for (int lx = -r; lx <= r; lx++) {
          const int reach = std::abs(lx) + std::abs(ly);
          const bool keep =
              r == 2 ? reach <= 2 || (reach == 3 && rand(2) == 0) : reach < 2 || (reach == 2 && rand(3) != 0);
          if (!keep || get(x + lx, y + ly, top + trunk + dz) != AIR) continue;
          set(x + lx, y + ly, top + trunk + dz, LEAVES);
        }
      }
    }
  }
  const auto scatter = [&](const uint8_t m, const int count, const uint8_t on) {
    for (int placed = 0, attempt = 0; placed < count && attempt < 60; attempt++) {
      const int x = rand(N), y = rand(N), top = columnTop(x, y);
      if (top < 0 || top + 1 >= H || get(x, y, top) != on) continue;
      set(x, y, top + 1, m);
      placed++;
    }
  };
  scatter(STONE, 3, GRASS);
  scatter(FLOWER, 9, GRASS);
  scatter(LANTERN, 1, SAND);
  // Two small rocks drifting beside the island.
  static constexpr int8_t ISLETS[2][2] = {{2, 16}, {16, 2}};
  for (const auto& islet : ISLETS) {
    const int z = 3 + rand(3);
    if (columnTop(islet[0], islet[1]) >= 0) continue;
    set(islet[0], islet[1], z - 1, STONE);
    set(islet[0], islet[1], z, GRASS);
    set(islet[0] + 1, islet[1], z, STONE);
    set(islet[0], islet[1] + 1, z, STONE);
  }
  curX = px - 3;
  curY = py;
  curZ = H - 1;
  placeCursorZ();
  dirtyWorld = true;
}

// The cursor walks on the surface nearest its current height: it steps up at
// most one block, drops down freely, ignores canopies overhead, climbs walls
// only when there is no ground below, and keeps its height over the void so
// bridges stay level. Planks and decorations are surfaces at their own level.
void VoxelActivity::placeCursorZ() {
  const auto surface = [this](const int z) {
    const uint8_t m = get(curX, curY, z);
    return m == PLANK || m == FLOWER || m == LANTERN || (m == AIR && z > 0 && get(curX, curY, z - 1) != AIR);
  };
  for (int z = std::min(curZ + 1, H - 1); z > 0; z--) {
    if (surface(z)) {
      curZ = z;
      return;
    }
  }
  for (int z = curZ + 2; z < H; z++) {
    if (surface(z)) {
      curZ = z;
      return;
    }
  }
}

bool VoxelActivity::load() {
  HalFile file;
  if (!Storage.exists(SAVE_PATH) || !Storage.openFileForRead("VOXEL", SAVE_PATH, file)) return false;
  uint8_t header[SAVE_HEADER];
  if (file.read(header, SAVE_HEADER) != SAVE_HEADER || memcmp(header, "VOX1", 4) != 0) return false;
  if (file.read(world.get(), N * N * H) != N * N * H) return false;
  memcpy(&seed, header + 4, 4);
  rotation = header[8] & 3;
  selected = std::clamp<uint8_t>(header[9], GRASS, FLOWER);
  curX = std::min<int>(header[10], N - 1);
  curY = std::min<int>(header[11], N - 1);
  curZ = GROUND;
  placeCursorZ();
  return true;
}

void VoxelActivity::save() {
  HalFile file;
  if (!Storage.openFileForWrite("VOXEL", SAVE_PATH, file)) {
    LOG_ERR("VOXEL", "Failed to write %s", SAVE_PATH);
    return;
  }
  uint8_t header[SAVE_HEADER] = {'V', 'O', 'X', '1'};
  memcpy(header + 4, &seed, 4);
  header[8] = rotation;
  header[9] = selected;
  header[10] = static_cast<uint8_t>(curX);
  header[11] = static_cast<uint8_t>(curY);
  file.write(header, SAVE_HEADER);
  file.write(world.get(), N * N * H);
  dirtyWorld = false;
}

// ---- view ------------------------------------------------------------------------

void VoxelActivity::toWorld(const int rx, const int ry, int& x, int& y) const {
  switch (rotation) {
    case 1:
      x = ry, y = N - 1 - rx;
      break;
    case 2:
      x = N - 1 - rx, y = N - 1 - ry;
      break;
    case 3:
      x = N - 1 - ry, y = rx;
      break;
    default:
      x = rx, y = ry;
      break;
  }
}

void VoxelActivity::toView(const int x, const int y, int& rx, int& ry) const {
  switch (rotation) {
    case 1:
      rx = N - 1 - y, ry = x;
      break;
    case 2:
      rx = N - 1 - x, ry = N - 1 - y;
      break;
    case 3:
      rx = y, ry = N - 1 - x;
      break;
    default:
      rx = x, ry = y;
      break;
  }
}

uint8_t VoxelActivity::viewGet(const int rx, const int ry, const int z) const {
  if (rx < 0 || ry < 0 || rx >= N || ry >= N || z >= zLimit) return AIR;
  int x = 0, y = 0;
  toWorld(rx, ry, x, y);
  return get(x, y, z);
}

bool VoxelActivity::occludes(const int rx, const int ry, const int z) const { return solid(viewGet(rx, ry, z)); }

void VoxelActivity::project(const int rx, const int ry, const int z, int& cx, int& ty) const {
  cx = originX + (rx - ry) * HALF;
  ty = originY + (rx + ry) * QUARTER - z * CUBE;
}

// Centres the island's drawn extent between top and bottom.
void VoxelActivity::layout(const int top, const int bottom) {
  originX = renderer.getScreenWidth() / 2;
  originY = 0;
  int lo = 1 << 20, hi = -(1 << 20), left = 1 << 20, right = -(1 << 20);
  for (int z = 0; z < H; z++) {
    for (int ry = 0; ry < N; ry++) {
      for (int rx = 0; rx < N; rx++) {
        if (viewGet(rx, ry, z) == AIR) continue;
        int cx = 0, ty = 0;
        project(rx, ry, z, cx, ty);
        lo = std::min(lo, ty);
        hi = std::max(hi, ty + 2 * QUARTER + CUBE);
        left = std::min(left, cx - HALF);
        right = std::max(right, cx + HALF);
      }
    }
  }
  if (lo > hi) lo = hi = left = right = 0;
  originY = (top + bottom) / 2 - (lo + hi) / 2;
  island = {left, lo + originY, right, hi + originY};
}

VoxelActivity::Rect VoxelActivity::cellRect(const int x, const int y, const int z, const int grow) const {
  int rx = 0, ry = 0, cx = 0, ty = 0;
  toView(x, y, rx, ry);
  project(rx, ry, z, cx, ty);
  return {cx - HALF - grow, ty - grow, cx + HALF - 1 + grow, ty + 2 * QUARTER + CUBE + grow};
}

void VoxelActivity::markDirty(const Rect& r) {
  for (int i = 0; i < dirtyCount; i++) {
    Rect& d = dirty[i];
    if (r.x0 <= d.x1 && d.x0 <= r.x1 && r.y0 <= d.y1 && d.y0 <= r.y1) {
      d = {std::min(d.x0, r.x0), std::min(d.y0, r.y0), std::max(d.x1, r.x1), std::max(d.y1, r.y1)};
      return;
    }
  }
  if (dirtyCount == MAX_DIRTY) {
    sceneRedraw = true;
    return;
  }
  dirty[dirtyCount++] = r;
}

void VoxelActivity::startReveal() {
  revealStage = 1;
  shownStage = 0;
  fullRedraw = true;
}

// ---- input -------------------------------------------------------------------------

// Presses made while the panel refreshes simply move the target; the next frame
// repaints only where the cursor was drawn and where it is now.
void VoxelActivity::moveCursor(const int drx, const int dry) {
  int rx = 0, ry = 0;
  toView(curX, curY, rx, ry);
  toWorld(std::clamp(rx + drx, 0, N - 1), std::clamp(ry + dry, 0, N - 1), curX, curY);
  placeCursorZ();
  requestUpdate();
}

void VoxelActivity::place() {
  int z = curZ;
  if (get(curX, curY, z) == PLANK) z++;  // build on top of a bridge
  if (z >= H || get(curX, curY, z) != AIR) return;
  set(curX, curY, z, selected);
  Rect r = cellRect(curX, curY, z, HALF + 2);
  r.y1 += 3 * CUBE;  // shade cast on the blocks below
  markDirty(r);
  placeCursorZ();
  dirtyWorld = true;
  requestUpdate();
}

// Removes what the cursor holds (a plank or decoration) or the block beneath it.
void VoxelActivity::remove() {
  const int top = get(curX, curY, curZ) != AIR ? curZ : curZ - 1;
  if (top < 0 || get(curX, curY, top) == AIR) return;
  set(curX, curY, top, AIR);
  Rect r = cellRect(curX, curY, top, HALF + 2);
  r.y0 -= CUBE;  // the cursor sat one block higher
  r.y1 += 3 * CUBE;
  markDirty(r);
  curZ = top;
  placeCursorZ();
  dirtyWorld = true;
  requestUpdate();
}

void VoxelActivity::runMenu(const int item) {
  menuOpen = false;
  switch (item) {
    case 1:
      rotation = (rotation + 1) & 3;
      fullRedraw = true;
      break;
    case 2:
      generate(seed * 1103515245u + static_cast<uint32_t>(millis()) + 12345u);
      save();
      startReveal();
      break;
    case 3:
      wallpaperRequested = true;
      break;
    case 4:
      finish();
      return;
    default:
      sceneRedraw = true;
      break;
  }
  requestUpdate();
}

void VoxelActivity::onEnter() {
  Activity::onEnter();
  world = makeUniqueNoThrow<uint8_t[]>(N * N * H);
  if (!world) {
    LOG_ERR("VOXEL", "OOM: world %d bytes", N * N * H);
    finish();
    return;
  }
  if (!load()) generate(static_cast<uint32_t>(millis()) | 1u);
  startReveal();
  requestUpdate();
}

void VoxelActivity::onExit() {
  if (world && dirtyWorld) save();
  world.reset();
  Activity::onExit();
}

void VoxelActivity::loop() {
  if (!world) return;
  using B = MappedInputManager::Button;
  RenderLock lock(*this);
  if (revealStage < REVEAL_STAGES) {
    const bool pressed = mappedInput.wasPressed(B::Confirm) || mappedInput.wasPressed(B::Back) ||
                         mappedInput.wasPressed(B::Left) || mappedInput.wasPressed(B::Right) ||
                         mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down);
    if (pressed || shownStage == revealStage) {
      revealStage = pressed ? REVEAL_STAGES : revealStage + 1;
      sceneRedraw = true;
      requestUpdate();
    }
    ignoreRelease = ignoreRelease || pressed;
    return;
  }
  if (ignoreRelease) {  // the press that skipped the reveal must not also place or remove
    if (mappedInput.wasReleased(B::Confirm) || mappedInput.wasReleased(B::Back) ||
        (!mappedInput.isPressed(B::Confirm) && !mappedInput.isPressed(B::Back))) {
      ignoreRelease = false;
    }
    return;
  }
  if (menuOpen) {
    const int before = menuIndex;
    if (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Left)) {
      menuIndex = (menuIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    }
    if (mappedInput.wasPressed(B::Down) || mappedInput.wasPressed(B::Right)) menuIndex = (menuIndex + 1) % MENU_ITEMS;
    if (menuIndex != before) requestUpdate();
    if (mappedInput.wasReleased(B::Confirm)) {
      lock.unlock();
      runMenu(menuIndex);
    } else if (mappedInput.wasReleased(B::Back)) {
      runMenu(0);
    }
    return;
  }
  if (mappedInput.wasLongPressed(B::Back, HOLD_MS)) {
    menuOpen = true;
    menuIndex = 0;
    requestUpdate();
    return;
  }
  if (mappedInput.wasLongPressed(B::Confirm, HOLD_MS)) {
    selected = static_cast<uint8_t>(selected == FLOWER ? GRASS : selected + 1);
    headerDirty = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) place();
  if (mappedInput.wasReleased(B::Back)) remove();
  if (mappedInput.wasPressed(B::Left)) moveCursor(-1, 0);
  if (mappedInput.wasPressed(B::Right)) moveCursor(1, 0);
  if (mappedInput.wasPressed(B::Up)) moveCursor(0, -1);
  if (mappedInput.wasPressed(B::Down)) moveCursor(0, 1);
}

// ---- drawing ------------------------------------------------------------------------

// White sky with a few outlined, softly shaded clouds; a pure function of the
// pixel so any rectangle can be repainted in place.
void VoxelActivity::drawBackground(const Rect& r) const {
  renderer.fillRect(r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1, false);
  const Pen pen(renderer, r.x0, r.y0, r.x1, r.y1);
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  for (int c = 0; c < 5; c++) {
    const uint32_t h = hash2(static_cast<int>(seed), c);
    // One cloud per band of the screen, skipped where it would touch the island.
    const int cr = 16 + static_cast<int>(h % 14);
    const int ccx = c * width / 5 + width / 10 + static_cast<int>((h >> 8) % 41) - 20;
    const int ccy = HEADER + 30 + static_cast<int>((h >> 16) % (height - HEADER - 140));
    if (ccx + 2 * cr + 12 > island.x0 && ccx - 2 * cr - 12 < island.x1 && ccy + cr > island.y0 - 12 &&
        ccy - 2 * cr < island.y1 + 12) {
      continue;
    }
    const auto inside = [&](const int x, const int y) {
      if (y > ccy + cr / 2) return false;
      const int a = x - (ccx - cr), b = x - ccx, d = x - (ccx + cr), yy = y - ccy, up = y - (ccy - cr * 2 / 5);
      return a * a + yy * yy < cr * cr * 1 / 2 || b * b + up * up < cr * cr || d * d + yy * yy < cr * cr * 9 / 16;
    };
    const int x0 = std::max(r.x0, ccx - 2 * cr - 2), x1 = std::min(r.x1, ccx + 2 * cr + 2);
    const int y0 = std::max(r.y0, ccy - 2 * cr), y1 = std::min(r.y1, ccy + cr / 2 + 1);
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        if (!inside(x, y)) continue;
        const bool edge = !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1);
        if (edge || (y > ccy + cr / 2 - 5 && bayer(3, x, y))) pen.put(x, y, true);
      }
    }
  }
  // A few distant birds.
  for (int b = 0; b < 3; b++) {
    const uint32_t h = hash2(static_cast<int>(seed) + 7, b);
    const int bx = static_cast<int>(h % width), by = HEADER + 16 + static_cast<int>((h >> 12) % 120);
    for (int k = -5; k <= 5; k++) pen.put(bx + k, by - std::abs(k) / 2 + (std::abs(k) == 5 ? 1 : 0), true);
  }
}

void VoxelActivity::drawCube(const int cx, const int ty, const int rx, const int ry, const int z, const uint8_t m,
                             const int height, const Rect& clip) const {
  const Pen pen(renderer, clip.x0, clip.y0, clip.x1, clip.y1);
  const bool slab = height < CUBE;
  const bool topVis = slab || !occludes(rx, ry, z + 1);
  const bool leftVis = !occludes(rx, ry + 1, z);
  const bool rightVis = !occludes(rx + 1, ry, z);
  int shade = 0;  // ambient occlusion from blocks overhead
  for (int k = 1; k <= 3 && !slab; k++) shade = occludes(rx, ry, z + k) ? 5 - k : shade;
  const int depth = std::max(0, GROUND - z) / 2;
  if (topVis) {
    for (int dy = 0; dy < 2 * QUARTER; dy++) {
      const int w = dy < QUARTER ? 2 * dy + 2 : 2 * (2 * QUARTER - dy);
      for (int dx = -w; dx < w; dx++) {
        pen.put(cx + dx, ty + dy, ink(m, 0, cx + dx, ty + dy, 2 * dy + dx, 2 * dy - dx, shade));
      }
    }
  }
  for (int i = 0; i < HALF; i++) {
    const int ly = ty + QUARTER + (i + 1) / 2, ry0 = ty + 2 * QUARTER - (i + 1) / 2;
    for (int j = 0; j < height; j++) {
      if (leftVis) pen.put(cx - HALF + i, ly + j, ink(m, 1, cx - HALF + i, ly + j, i, j, depth));
      if (rightVis) pen.put(cx + i, ry0 + j, ink(m, 2, cx + i, ry0 + j, i, j, depth));
    }
  }
  // Outlines: silhouettes and creases, skipping seams between coplanar faces.
  const bool glass = m == GLASS || slab;
  const auto flatTop = [&](const int a, const int b) { return !glass && occludes(a, b, z) && !occludes(a, b, z + 1); };
  const auto flatLeft = [&](const int a, const int b, const int c) {
    return !glass && occludes(a, b, c) && !occludes(a, b + 1, c);
  };
  const auto flatRight = [&](const int a, const int b, const int c) {
    return !glass && occludes(a, b, c) && !occludes(a + 1, b, c);
  };
  const int h = height - 1;
  const int lx = cx - HALF, rx1 = cx + HALF - 1, my = ty + QUARTER, by = ty + 2 * QUARTER;
  if (topVis) {
    if (!flatTop(rx - 1, ry)) pen.line(lx, my, cx - 1, ty);
    if (!flatTop(rx, ry - 1)) pen.line(cx, ty, rx1, my);
    if (leftVis || !flatTop(rx, ry + 1)) pen.line(lx, my, cx - 1, by);
    if (rightVis || !flatTop(rx + 1, ry)) pen.line(cx, by, rx1, my);
  }
  if (leftVis) {
    if (!flatLeft(rx - 1, ry, z)) pen.line(lx, my, lx, my + h);
    if (!flatLeft(rx, ry, z - 1) || slab) pen.line(lx, my + h, cx - 1, by + h);
    if (!topVis && !flatLeft(rx, ry, z + 1)) pen.line(lx, my, cx - 1, by);
  }
  if (rightVis) {
    if (!flatRight(rx, ry - 1, z)) pen.line(rx1, my, rx1, my + h);
    if (!flatRight(rx, ry, z - 1) || slab) pen.line(cx, by + h, rx1, my + h);
    if (!topVis && !flatRight(rx, ry, z + 1)) pen.line(cx, by, rx1, my);
  }
  if (leftVis && rightVis) {
    pen.line(cx, by, cx, by + h);
  } else if (leftVis && !flatLeft(rx + 1, ry, z)) {
    pen.line(cx - 1, by, cx - 1, by + h);
  } else if (rightVis && !flatRight(rx, ry + 1, z)) {
    pen.line(cx, by, cx, by + h);
  }
}

// Flowers and lanterns stand on the centre of the block below.
void VoxelActivity::drawDecoration(const int cx, const int ty, const uint8_t m, const Rect& clip) const {
  const Pen pen(renderer, clip.x0, clip.y0, clip.x1, clip.y1);
  const int fy = ty + CUBE + QUARTER;
  if (m == FLOWER) {
    pen.line(cx, fy, cx, fy - 7);
    pen.put(cx - 1, fy - 3, true), pen.put(cx - 2, fy - 4, true), pen.put(cx + 1, fy - 5, true);
    for (int k = -2; k <= 2; k++) {
      pen.put(cx + k, fy - 10 + (std::abs(k) == 2 ? 0 : std::abs(k) == 1 ? -2 : 0), true);
      pen.put(cx + k, fy - 10 + (std::abs(k) == 2 ? 0 : 2), true);
    }
    pen.put(cx - 1, fy - 11, true), pen.put(cx + 1, fy - 11, true), pen.put(cx, fy - 13, true);
    pen.put(cx, fy - 10, false);
    return;
  }
  // Lantern on a post: dark frame, glowing white core.
  pen.line(cx - 1, fy, cx - 1, fy - 9), pen.line(cx, fy, cx, fy - 9);
  for (int y = fy - 18; y <= fy - 9; y++) {
    for (int x = cx - 4; x <= cx + 3; x++) {
      const bool frame = x == cx - 4 || x == cx + 3 || y == fy - 18 || y == fy - 9;
      pen.put(x, y, frame || (y == fy - 14 && x == cx - 1) || (y == fy - 14 && x == cx));
    }
  }
  pen.line(cx - 6, fy - 19, cx - 1, fy - 22), pen.line(cx, fy - 22, cx + 5, fy - 19);
  pen.line(cx - 5, fy - 19, cx + 4, fy - 19);
  for (int k = 0; k < 8; k++) {  // faint glow
    const int gx = cx + (k % 2 ? 7 : -8) + (k / 2 % 2), gy = fy - 16 + k;
    if ((gx + gy) % 2 == 0) pen.put(gx, gy, true);
  }
}

void VoxelActivity::drawVoxel(const int rx, const int ry, const int z, const uint8_t m, const Rect& clip) const {
  int cx = 0, ty = 0;
  project(rx, ry, z, cx, ty);
  if (cx + HALF < clip.x0 || cx - HALF > clip.x1 || ty > clip.y1 || ty + 2 * QUARTER + CUBE < clip.y0) return;
  if (m == FLOWER || m == LANTERN) {
    drawDecoration(cx, ty, m, clip);
  } else {
    drawCube(cx, ty, rx, ry, z, m, m == PLANK ? SLAB : CUBE, clip);
  }
}

void VoxelActivity::drawCursor(const Rect& clip) const {
  const Pen pen(renderer, clip.x0, clip.y0, clip.x1, clip.y1);
  int rx = 0, ry = 0, cx = 0, ty = 0;
  toView(curX, curY, rx, ry);
  project(rx, ry, curZ, cx, ty);
  const int lx = cx - HALF, rx1 = cx + HALF - 1, my = ty + QUARTER, by = ty + 2 * QUARTER, h = CUBE - 1;
  const int pts[9][4] = {{lx, my, cx, ty},           {cx, ty, rx1, my},           {lx, my, cx, by},
                         {cx, by, rx1, my},          {lx, my, lx, my + h},        {cx, by, cx, by + h},
                         {rx1, my, rx1, my + h},     {lx, my + h, cx, by + h},    {cx, by + h, rx1, my + h}};
  // A white halo under a solid black frame keeps the cursor legible on any texture.
  for (int pass = 0; pass < 2; pass++) {
    for (const auto& p : pts) {
      for (int o = -1 + pass; o <= 1 - pass; o++) {
        const bool vertical = p[0] == p[2];
        const int ox = vertical ? o : 0, oy = vertical ? 0 : o;
        pen.line(p[0] + ox, p[1] + oy, p[2] + ox, p[3] + oy,
                 pass == 0 ? -1 : 0);
      }
    }
  }
}

void VoxelActivity::drawScene(const Rect& clip, const bool withCursor) const {
  const auto saved = renderer.getClipRect();
  renderer.setClipRect(clip.x0, clip.y0, clip.x1 - clip.x0 + 1, clip.y1 - clip.y0 + 1);
  drawBackground(clip);
  for (int s = 0; s <= 2 * (N - 1); s++) {
    for (int rx = std::max(0, s - N + 1); rx <= std::min(N - 1, s); rx++) {
      for (int z = 0; z < H; z++) {
        const uint8_t m = viewGet(rx, s - rx, z);
        if (m != AIR) drawVoxel(rx, s - rx, z, m, clip);
      }
    }
  }
  if (withCursor) drawCursor(clip);
  renderer.setClipRect(saved[0], saved[1], saved[2], saved[3]);
}

void VoxelActivity::drawHeader() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER, false);
  renderer.drawText(UI_12_FONT_ID, 20, 8, tr(STR_PLAY_VOXEL), true, EpdFontFamily::BOLD);
  renderer.drawText(SMALL_FONT_ID, 20, 38, toast ? toast : tr(STR_VX_HINT));
  const char* name = I18N.get(NAMES[selected]);
  const int nameWidth = renderer.getTextWidth(UI_10_FONT_ID, name, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, width - 54 - nameWidth, 10, name, true, EpdFontFamily::BOLD);
  const Rect all{0, 0, width - 1, HEADER - 1};
  if (selected == FLOWER || selected == LANTERN) {
    drawDecoration(width - 30, 4, selected, all);
  } else {
    drawCube(width - 30, 8, -9, -9, GROUND, selected, selected == PLANK ? SLAB : CUBE, all);
  }
  renderer.drawLine(0, HEADER - 1, width - 1, HEADER - 1, true);
}

void VoxelActivity::drawMenu() const {
  const int width = renderer.getScreenWidth();
  constexpr int ROW = 44;
  const int boxW = 300, boxH = ROW * MENU_ITEMS + 20;
  const int x = (width - boxW) / 2, y = (renderer.getScreenHeight() - boxH) / 2;
  renderer.fillRoundedRect(x, y, boxW, boxH, 10, Color::White);
  renderer.drawRoundedRect(x, y, boxW, boxH, 2, 10, true);
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int ry = y + 10 + i * ROW;
    if (i == menuIndex) renderer.fillRoundedRect(x + 10, ry, boxW - 20, ROW - 4, 6, Color::Black);
    renderer.drawText(UI_12_FONT_ID, x + 26, ry + 8, I18N.get(MENU[i]), i != menuIndex,
                      i == menuIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

bool VoxelActivity::saveWallpaper() {
  char path[48];
  for (int i = 1; i < 1000; i++) {
    snprintf(path, sizeof(path), "%s/island-%03d.bmp", wallpaper::DIR, i);
    if (!Storage.exists(path)) break;
  }
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  renderer.clearScreen();
  layout(0, height);
  drawScene({0, 0, width - 1, height - 1}, false);
  const bool ok = ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                                       renderer.getDisplayHeight());
  LOG_INF("VOXEL", "%s %s", ok ? "saved" : "failed to save", path);
  return ok;
}

void VoxelActivity::render(RenderLock&& lock) {
  if (!world) return;
  static constexpr int REVEAL_LIMITS[REVEAL_STAGES] = {6, 11, H};
  const int width = renderer.getScreenWidth();
  const int bottom = renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 2;
  const Rect area{0, HEADER, width - 1, bottom};
  const bool finished = revealStage >= REVEAL_STAGES;
  bool clean = fullRedraw;
  if (wallpaperRequested) {
    wallpaperRequested = false;
    toast = saveWallpaper() ? tr(STR_VX_SAVED) : tr(STR_SK_SAVE_FAILED);
    clean = true;
  } else if (!clean && (dirtyCount > 0 || (cursorShown && toast))) {
    toast = nullptr;
    headerDirty = true;
  }
  if (clean) {
    renderer.clearScreen();
    layout(HEADER, bottom);
  }
  zLimit = REVEAL_LIMITS[std::clamp(revealStage, 1, REVEAL_STAGES) - 1];
  const Rect cursor = cellRect(curX, curY, curZ, 2);
  if (clean || sceneRedraw) {
    drawScene(area, finished);
  } else {
    if (!cursorShown || cursor.x0 != drawnCursor.x0 || cursor.y0 != drawnCursor.y0) {
      if (cursorShown) markDirty(drawnCursor);
      markDirty(cursor);
    }
    if (sceneRedraw) {
      drawScene(area, finished);
    } else {
      for (int i = 0; i < dirtyCount; i++) {
        const Rect r{std::max(0, dirty[i].x0), std::max(HEADER, dirty[i].y0), std::min(width - 1, dirty[i].x1),
                     std::min(bottom, dirty[i].y1)};
        if (r.x0 <= r.x1 && r.y0 <= r.y1) drawScene(r, true);
      }
    }
  }
  zLimit = H;
  if (clean || headerDirty) drawHeader();
  if (clean) {
    const auto labels =
        mappedInput.mapLabels(tr(STR_VX_REMOVE), tr(STR_VX_PLACE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  if (menuOpen) drawMenu();
  drawnCursor = cursor;
  cursorShown = finished;
  shownStage = revealStage;
  fullRedraw = headerDirty = sceneRedraw = false;
  dirtyCount = 0;
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
