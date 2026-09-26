#pragma once

#include <cstdint>

#include "activities/Activity.h"

// Sketch character creator: a pencil-sketch face assembled from procedurally
// drawn parts (no bitmaps). Side Up/Down pick the part, Left/Right cycle its
// options, Confirm randomizes everything, holding Confirm saves a full-screen
// portrait to /sleep as a wallpaper BMP, Back exits.
class SketchActivity final : public Activity {
 public:
  explicit SketchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sketch", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  enum Part : uint8_t { FACE, EYES, BROWS, NOSE, MOUTH, HAIR, HAT, GLASSES, EXTRA, PART_COUNT };

 private:
  uint8_t choice[PART_COUNT] = {};
  uint8_t part = FACE;
  uint32_t rng = 1;
  uint16_t seed = 1;  // stroke jitter; changes on randomize so every face is freshly "drawn"

  // Guarded by the render lock.
  bool fullRedraw = true;
  bool saveRequested = false;
  const char* toast = nullptr;

  void randomize();
  void cycle(int delta);
  bool saveWallpaper();
  void drawCharacter(int cx, int cy, int scale) const;
  void drawChrome() const;
  void drawLabel() const;
  int areaTop() const;
  int areaBottom() const;
};
