#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <atomic>
#include <cstdint>

// Shared helpers for the Play screens: accelerometer tilt in screen space and
// the per-frame refresh bookkeeping (fast diff frames with a periodic HALF
// refresh to shed ghosting).
namespace play {

// Tilt from the IMU, rotated into the renderer's logical axes (x right, y down),
// in g. The simulator has no accelerometer, so it always reports unavailable
// and the games steer with held buttons instead.
class Tilt {
 public:
  bool begin();
  void end();
  bool available() const { return active; }
  bool read(const GfxRenderer& renderer, float& x, float& y) const;

 private:
  bool active = false;
};

// Fast refreshes before a HALF refresh scrubs accumulated ghosting.
constexpr int FRAMES_PER_CLEAN = 60;

class FramePacer {
 public:
  // Picks the refresh mode for the next frame. `forceClean` asks for HALF now.
  HalDisplay::RefreshMode next(bool forceClean);
  // Wraps displayBuffer and records how long the panel took. Safe to call
  // after RenderLock::unlock(): it touches only renderer and a global.
  static void present(const GfxRenderer& renderer, HalDisplay::RefreshMode mode);
  // Smoothed duration of recent fast frames in ms (0 until measured).
  static uint32_t averageFrameMs() { return averageMs.load(); }

 private:
  int framesSinceClean = 0;
  bool first = true;
  static std::atomic<uint32_t> averageMs;
};

// Small caption in the top-left corner, e.g. "412 ms" frame cost.
void drawFrameCost(const GfxRenderer& renderer, int x, int y);

}  // namespace play
