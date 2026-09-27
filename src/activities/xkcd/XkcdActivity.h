#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>
#include <vector>

#include "XkcdPack.h"
#include "activities/Activity.h"

// xkcd reader. Comics come from the offline pack on the SD card
// (scripts/gen_xkcd_pack.py: pre-scaled, pre-dithered 1-bit images) plus a
// small online cache for comics newer than the pack. Opening and browsing
// never touch the network; "Check new" (Side Down at the newest comic)
// downloads what xkcd.com has published since.
//
//   Left / Right   previous / next comic (Right at the newest: random)
//   Confirm        show / hide the alt text
//   Side Up        random comic
//   Side Down      latest comic; at the latest, check xkcd.com for new ones
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
  bool skipLoopDelay() override { return checking; }
  bool preventAutoSleep() override { return checking; }

 private:
  static constexpr int HEADER_HEIGHT = 34;
  static constexpr float MAX_UPSCALE = 1.5f;

  // Why the last check for new comics failed; shown instead of the date.
  enum class NetError : uint8_t { None, NoWifi, LowMemory, Server, Network, SdCard, BadData };

  struct Comic {
    uint16_t num = 0;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    std::string title;
    std::string alt;
    bool fromPack = false;
    XkcdPack::Entry packEntry;
    std::string imagePath;  // online-cache image; empty when unsupported
    int16_t width = 0;      // online-cache source image size
    int16_t height = 0;
  };

  XkcdPack pack;
  Comic comic;
  bool haveComic = false;
  bool landscape = false;
  bool showAlt = false;

  bool checking = false;  // a "Check new" is queued or running
  bool waitingForWifi = false;
  bool wifiUsed = false;
  NetError netError = NetError::None;
  int netStatus = 0;     // HTTP status behind NetError::Server
  int checkResult = -1;  // comics added by the last check; -1 = no check yet
  int fetchingNum = 0;   // comic being downloaded, for the progress popup

  int latest = 0;                // newest number xkcd.com reported
  int lastViewed = 0;            // reopened on the next visit
  std::vector<uint16_t> cached;  // online cache, ascending
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  GfxRenderer::Orientation drawnOrientation = GfxRenderer::Orientation::Portrait;
  bool firstFrame = true;

  // --- browsing (XkcdActivity.cpp) -----------------------------------------
  bool show(int num);
  bool loadFromPack(int32_t index);
  bool loadCached(int num);
  int neighbour(int num, int direction) const;
  int newestAvailable() const;
  void showRandom();
  void chooseOrientation();

  // --- network (XkcdNetwork.cpp) --------------------------------------------
  void startCheck();
  void runCheck();
  bool ensureWifi();
  void stopWifi();
  int downloadNew();
  bool tlsHeapOk();
  bool fetchJson(int num, std::string& meta);
  bool fetchComic(int num, const std::string& meta);
  bool downloadImage(const char* url, const std::string& path);
  void addCached(uint16_t num);
  void loadState();
  void saveState() const;

  // --- drawing (XkcdViews.cpp) ---------------------------------------------
  void drawComic();
  void drawImage(int top, int bottom);
  void drawAltText(int top, int bottom) const;
  void drawMessage() const;
  void drawHints() const;
  const char* statusText(char* buffer, size_t size) const;
  bool atNewest() const;
};
