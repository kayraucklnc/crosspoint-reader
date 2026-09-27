#pragma once

#include <GfxRenderer.h>

#include "activities/Activity.h"
#include "network/TrainsCache.h"
#include "network/TrenordClient.h"

// Trenord departures. Opens on home -> default direction (Certosa towards the
// centre); a station strip along the bottom picks a return trip towards home.
// Left/Right move the strip cursor, Confirm loads that board, Up/Down scroll the
// list, Back returns to the default board and then exits. The strip ends in an
// "Auto" chip that sets the auto-refresh interval while asleep. Boards are cached
// on SD and refreshed in place with a silent WiFi join.
class TrainsActivity final : public Activity {
 public:
  explicit TrainsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                          BackgroundMode background = BackgroundMode::Off, uint16_t route = DEFAULT_SLEEP_ROUTE)
      : Activity("Trains", renderer, mappedInput), background(background), initialRoute(route) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override {
    return state == State::CONNECTING || state == State::LOADING || state == State::LOADING_ALERTS;
  }
  void render(RenderLock&&) override;
  bool hasSleepSnapshot() const override { return hasBoard; }
  void renderSleepSnapshot() override;
  uint16_t sleepSnapshotRoute() const override;
  BackgroundRefresh backgroundRefreshState() const override {
    return background == BackgroundMode::Off ? BackgroundRefresh::None : backgroundState;
  }
  void leaveBackgroundRefresh() override;

 private:
  enum class State { CONNECTING, PICKING, LOADING, LOADING_ALERTS, READY, ERROR };
  static constexpr int MAX_STRIP = trenord::STATION_COUNT;

  State state = State::READY;
  const char* errorMessage = "";
  TrainsCache cache;
  TrainsConfig config;
  trenord::Board board;  // what is on screen
  bool hasBoard = false;
  int requestedOrigin = trenord::HOME;
  int requestedTowards = trenord::DEFAULT_TOWARDS;
  int8_t strip[MAX_STRIP] = {};  // station indices, home first then recent then line order
  int stripCount = 0;
  int cursor = 0;
  int scroll = 0;
  bool wifiStartedHere = false;
  bool firstRenderDone = false;
  // Sleep snapshot: countdowns would go stale, so the board shows scheduled
  // times and delays only, without the station strip or button hints.
  bool frozen = false;
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  // Timer-wake refresh: fetch the snapshot's board without drawing or the WiFi picker.
  BackgroundMode background;
  BackgroundRefresh backgroundState = BackgroundRefresh::Pending;
  uint16_t initialRoute;

  void buildStrip();
  void showBoard(int origin, int towards);
  void startRefresh();
  void runConnect();
  void runFetch();
  void runAlerts();
  void onWifiSelectionComplete(bool connected);
  void finishBackground(bool succeeded);
  void openAutoRefreshPicker();
  // The cursor sits on the trailing Auto chip rather than a station.
  bool onAutoChip() const { return cursor == stripCount; }
  bool viewingReturn() const { return board.origin != config.origin || board.towards != config.towards; }
  void showDefault() { showBoard(config.origin, config.towards); }

  void drawPage();
  // Both return the bottom y of what they drew that the hero must clear.
  int renderHeader(int x, int y, int width) const;
  void routeLabel(char* out, size_t size) const;
  // Sleep snapshot on a wide screen whose badge does not fit beside the route.
  bool badgeBelowRoute(int width) const;
  // Strike banner (today) or line (within 3 days); returns the height used.
  int renderStrike(int x, int y, int width, time_t now) const;
  int firstUpcoming(time_t now) const;
  int renderHero(int x, int y, int width, time_t now) const;
  void renderList(int x, int y, int width, int height, time_t now) const;
  void renderStrip(int y, int width) const;
  // Draws the Auto chip right-aligned in the strip row; returns its width.
  int renderAutoChip(int y, int right) const;
  int renderStatus(int x, int y, int width) const;
};
