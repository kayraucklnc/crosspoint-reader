#include "PlayCommon.h"

#include <HalTiltSensor.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "fontIds.h"

namespace play {
namespace {
// Accelerometer axes seen from a Portrait screen. On the X3 the IMU's Y axis
// runs across the portrait screen and X along it (see HalTiltSensor::update).
// Flip these if the ball rolls uphill on a new board revision.
constexpr float PORTRAIT_X_FROM_AY = -1.0f;
constexpr float PORTRAIT_Y_FROM_AX = 1.0f;
constexpr float MAX_TILT_G = 1.0f;
}  // namespace

std::atomic<uint32_t> FramePacer::averageMs{0};

bool Tilt::begin() {
#ifdef SIMULATOR
  active = false;
#else
  active = halTiltSensor.acquireAccel();
#endif
  return active;
}

void Tilt::end() {
#ifndef SIMULATOR
  if (active) halTiltSensor.releaseAccel();
#endif
  active = false;
}

bool Tilt::read(const GfxRenderer& renderer, float& x, float& y) const {
#ifdef SIMULATOR
  (void)renderer;
  (void)x;
  (void)y;
  return false;
#else
  if (!active) return false;
  float ax = 0, ay = 0, az = 0;
  if (!halTiltSensor.readAccel(ax, ay, az)) return false;
  const float px = std::clamp(ay * PORTRAIT_X_FROM_AY, -MAX_TILT_G, MAX_TILT_G);
  const float py = std::clamp(ax * PORTRAIT_Y_FROM_AX, -MAX_TILT_G, MAX_TILT_G);
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      x = px;
      y = py;
      break;
    case GfxRenderer::PortraitInverted:
      x = -px;
      y = -py;
      break;
    case GfxRenderer::LandscapeClockwise:
      x = py;
      y = -px;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      x = -py;
      y = px;
      break;
  }
  return true;
#endif
}

HalDisplay::RefreshMode FramePacer::next(const bool forceClean) {
  if (first || forceClean || ++framesSinceClean >= FRAMES_PER_CLEAN) {
    first = false;
    framesSinceClean = 0;
    return HalDisplay::HALF_REFRESH;
  }
  return HalDisplay::FAST_REFRESH;
}

void FramePacer::present(const GfxRenderer& renderer, const HalDisplay::RefreshMode mode) {
  const unsigned long start = millis();
  renderer.displayBuffer(mode);
  const auto elapsed = static_cast<uint32_t>(millis() - start);
  if (mode != HalDisplay::FAST_REFRESH) {
    LOG_DBG("PLAY", "clean frame %lu ms", static_cast<unsigned long>(elapsed));
    return;
  }
  const uint32_t prev = averageMs.load();
  averageMs.store(prev == 0 ? elapsed : (prev * 3 + elapsed) / 4);
  LOG_DBG("PLAY", "fast frame %lu ms (avg %lu)", static_cast<unsigned long>(elapsed),
          static_cast<unsigned long>(averageMs.load()));
}

void drawFrameCost(const GfxRenderer& renderer, const int x, const int y) {
  const uint32_t ms = FramePacer::averageFrameMs();
  if (ms == 0) return;
  char text[16];
  snprintf(text, sizeof(text), "%lu ms", static_cast<unsigned long>(ms));
  renderer.drawText(SMALL_FONT_ID, x, y, text);
}

}  // namespace play
