#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

// xkcd reader: fetches comics straight from xkcd.com (info.0.json + image),
// caches the most recent ones on the SD card and draws them dithered to 1 bit,
// scaled to the panel. A wide comic turns the screen to landscape.
//
//   Left / Right   previous / next comic
//   Confirm        show / hide the alt text
//   Side Up        random comic
//   Side Down      latest comic
//   Back           closes the alt text, then exits
class XkcdActivity final : public Activity {
 public:
  XkcdActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("Xkcd", renderer, mappedInput) {}

  // Play-menu preview drawn inside a size x size box.
  static void drawMenuIcon(const GfxRenderer& renderer, int x, int y, int size);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return loading; }
  bool preventAutoSleep() override { return loading; }

 private:
  static constexpr int HEADER_HEIGHT = 34;
  static constexpr float MAX_UPSCALE = 1.5f;

  struct Comic {
    uint16_t num = 0;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    std::string title;
    std::string alt;
    std::string imagePath;  // cached image on the SD card; empty when unsupported
    int16_t width = 0;      // source image size
    int16_t height = 0;
  };

  Comic comic;
  bool haveComic = false;
  bool landscape = false;
  bool showAlt = false;
  bool offline = false;  // showing a cached comic because xkcd.com was unreachable
  bool failed = false;

  bool loading = false;
  int target = 0;     // comic to load; 0 = latest
  int direction = 0;  // -1 / +1 while stepping, so a missing number is skipped
  bool waitingForWifi = false;
  bool wifiUsed = false;

  int latest = 0;
  std::vector<uint16_t> recent;  // cached comics, most recently viewed first
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  GfxRenderer::Orientation drawnOrientation = GfxRenderer::Orientation::Portrait;
  bool firstFrame = true;

  void request(int num, int step);
  void runLoad();
  bool ensureWifi();
  bool loadCached(int num);
  bool fetch(int num);
  bool readMeta(const std::string& json, Comic& out) const;
  void chooseOrientation();
  void remember(uint16_t num);
  void loadState();
  void saveState() const;

  // --- drawing (XkcdViews.cpp) ---------------------------------------------
  void drawComic();
  void drawAltText(int top, int bottom) const;
  void drawMessage() const;
  void drawHints() const;
};
