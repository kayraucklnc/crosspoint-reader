#pragma once

#include <cstdint>
#include <memory>

#include "activities/Activity.h"

// Sky Island: a seed-generated floating voxel island drawn in isometric
// projection with dithered, three-tone materials. Move the 3D cursor with
// Left/Right and the side buttons, Confirm places the selected block, holding
// Confirm picks the next block type, Back removes, holding Back opens the menu
// (rotate, new island, save wallpaper, exit). The world is saved to SD.
//
// A block change or cursor move re-renders only the screen rectangles it
// touches (background + every cube intersecting them, in painter order) and
// FAST-refreshes; rotation and new islands use a HALF refresh.
class VoxelActivity final : public Activity {
 public:
  explicit VoxelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SkyIsland", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int N = 20;  // world is N x N columns
  static constexpr int H = 16;  // and H levels high

 private:
  struct Rect {
    int x0, y0, x1, y1;
  };
  static constexpr int MAX_DIRTY = 6;
  static constexpr int REVEAL_STAGES = 3;
  static constexpr int MENU_ITEMS = 5;

  std::unique_ptr<uint8_t[]> world;  // N*N*H materials, index (z * N + y) * N + x
  uint32_t seed = 1;
  uint8_t rotation = 0;
  uint8_t selected = 1;
  int curX = N / 2, curY = N / 2, curZ = 0;
  int originX = 0, originY = 0;
  Rect island = {};  // screen bounds of the island, kept clear of clouds
  bool dirtyWorld = false;
  // Island reveal: stages 1..REVEAL_STAGES draw successively higher layers,
  // one refresh each; any press jumps to the finished island.
  int revealStage = REVEAL_STAGES;
  int shownStage = REVEAL_STAGES;
  mutable int zLimit = H;  // voxels at or above this level are treated as air
  bool menuOpen = false;
  bool ignoreRelease = false;
  int menuIndex = 0;

  // Guarded by the render lock.
  bool fullRedraw = true;
  bool headerDirty = false;
  bool wallpaperRequested = false;
  const char* toast = nullptr;
  bool sceneRedraw = false;  // repaint the whole scene without a HALF refresh
  Rect dirty[MAX_DIRTY] = {};
  int dirtyCount = 0;
  Rect drawnCursor = {};  // where the cursor is on screen; moves coalesce against it
  bool cursorShown = false;

  // World access in world coordinates.
  uint8_t get(int x, int y, int z) const;
  void set(int x, int y, int z, uint8_t m);
  int columnTop(int x, int y) const;
  void generate(uint32_t newSeed);
  void placeCursorZ();
  bool load();
  void save();

  // View helpers (rotated coordinates rx, ry).
  void toWorld(int rx, int ry, int& x, int& y) const;
  void toView(int x, int y, int& rx, int& ry) const;
  uint8_t viewGet(int rx, int ry, int z) const;
  bool occludes(int rx, int ry, int z) const;
  void project(int rx, int ry, int z, int& cx, int& ty) const;
  void layout(int top, int bottom);
  Rect cellRect(int x, int y, int z, int grow) const;
  void markDirty(const Rect& r);
  void startReveal();

  // Input.
  void moveCursor(int dx, int dy);
  void place();
  void remove();
  void runMenu(int item);

  // Drawing.
  void drawScene(const Rect& clip, bool withCursor) const;
  void drawBackground(const Rect& r) const;
  void drawVoxel(int rx, int ry, int z, uint8_t m, const Rect& clip) const;
  void drawCube(int cx, int ty, int rx, int ry, int z, uint8_t m, int height, const Rect& clip) const;
  void drawDecoration(int cx, int ty, uint8_t m, const Rect& clip) const;
  void drawCursor(const Rect& clip) const;
  void drawHeader() const;
  void drawMenu() const;
  bool saveWallpaper();
};
