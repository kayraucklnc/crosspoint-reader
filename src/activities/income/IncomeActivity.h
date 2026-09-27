#pragma once

#include <GfxRenderer.h>

#include "activities/Activity.h"
#include "network/StripeClient.h"

// Stripe income dashboard: today's income in whole US dollars as a hero number
// plus a dithered, smoothed five-day line chart. Refreshes in place: joins a saved WiFi
// network silently, fetches, then drops WiFi. Confirm refreshes, Back exits, Left
// sets the auto-refresh interval while asleep, Right switches Gross/Net.
class IncomeActivity final : public Activity {
 public:
  explicit IncomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                          BackgroundMode background = BackgroundMode::Off)
      : Activity("Income", renderer, mappedInput), background(background) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return state == State::CONNECTING || state == State::LOADING; }
  void render(RenderLock&&) override;
  bool hasSleepSnapshot() const override { return hasSummary && state != State::NO_KEY; }
  void renderSleepSnapshot() override;
  BackgroundRefresh backgroundRefreshState() const override {
    return background == BackgroundMode::Off ? BackgroundRefresh::None : backgroundState;
  }
  void leaveBackgroundRefresh() override;

 private:
  // PICKING: the WiFi picker is open because no saved network could be joined.
  enum class State { CONNECTING, PICKING, LOADING, READY, NO_KEY, ERROR };
  State state = State::CONNECTING;
  const char* errorMessage = "";
  IncomeSummary summary;
  bool hasSummary = false;
  bool wifiStartedHere = false;
  bool firstRenderDone = false;
  // Sleep snapshot: no button hints, and the status line becomes the asleep badge.
  bool frozen = false;
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  // Timer-wake refresh: fetch without drawing or the WiFi picker.
  BackgroundMode background;
  BackgroundRefresh backgroundState = BackgroundRefresh::Pending;

  static StripeClient::Amount amountMode();
  void startFetch();
  void runFetch();
  void runConnect();
  void onWifiSelectionComplete(bool connected);
  void stopWifi();
  void finishBackground(bool succeeded);
  void openAutoRefreshPicker();
  void toggleAmount();

  void renderDashboard(int width, int height) const;
  // Returns the bottom y of the hero block.
  int renderHero(int x, int y, int width, bool landscape) const;
  // Status under the month total: progress while refreshing, time or error otherwise.
  int renderStatus(int x, int y, int width, bool alignRight) const;
  void renderChart(int x, int y, int width, int height) const;
  void renderMessage(const char* title, const char* detail) const;
};
