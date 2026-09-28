#pragma once

#include <GfxRenderer.h>

#include "IncomeView.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "network/IncomeCache.h"
#include "network/IncomeHistory.h"
#include "network/StripeClient.h"

// Stripe income dashboard: the present bucket (today, this week or this month)
// as a hero number, the period total, and a dithered, smoothed line chart of
// the selected range. Refreshes in place: joins a saved WiFi network silently,
// fetches, then drops WiFi. Confirm refreshes, Back exits, Left sets the
// auto-refresh interval while asleep, Right switches Gross/Net. The side
// buttons slide one period back (Up) or forward (Down); holding either picks the range.
class IncomeActivity final : public Activity {
 public:
  explicit IncomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                          BackgroundMode background = BackgroundMode::Off)
      : Activity("Income", renderer, mappedInput), background(background) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override {
    return state == State::CONNECTING || state == State::LOADING || state == State::BACKFILL;
  }
  void render(RenderLock&&) override;
  bool hasSleepSnapshot() const override { return hasSummary && state != State::NO_KEY; }
  void renderSleepSnapshot() override;
  BackgroundRefresh backgroundRefreshState() const override {
    return background == BackgroundMode::Off ? BackgroundRefresh::None : backgroundState;
  }
  void leaveBackgroundRefresh() override;

 private:
  // PICKING: the WiFi picker is open because no saved network could be joined.
  // LOADING: refreshing this month. BACKFILL: fetching closed months the view needs, one per loop pass.
  enum class State { CONNECTING, PICKING, LOADING, BACKFILL, READY, NO_KEY, ERROR };
  State state = State::CONNECTING;
  const char* errorMessage = "";
  // This month's closed days plus the last five-day summary; the view's anchor is its fetch time.
  IncomeCache income;
  bool hasSummary = false;
  bool wifiStartedHere = false;
  bool firstRenderDone = false;
  // Sleep snapshot: no button hints, and the status line becomes the asleep badge.
  bool frozen = false;
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  // Timer-wake refresh: fetch without drawing or the WiFi picker.
  BackgroundMode background;
  BackgroundRefresh backgroundState = BackgroundRefresh::Pending;

  // Periods back from the present; slides only change this and request a frame,
  // so presses made during a panel refresh coalesce into the next one.
  int offset = 0;
  // The offset the current `view` was built for; render reads only this one.
  int viewOffset = 0;
  // Whether LOADING refreshes this month first, or only fills closed months.
  bool refreshCurrent = true;
  // A slide or range change may need closed months; checked once input settles.
  bool fillPending = false;
  unsigned long lastViewChangeAt = 0;
  int framesSinceHalfRefresh = 0;
  int backfillDone = 0;
  int backfillTotal = 0;
  int lastBackfillKey = 0;
  OptionPopup rangePopup;
  // Built under the render lock (render() or the loop holding a RenderLock); it reads SD.
  IncomeView view;
  IncomeMonth scratch;

  static StripeClient::Amount amountMode();
  static IncomeRange range();
  void startFetch(bool includeCurrent);
  void runFetch();
  void runBackfillStep();
  void finishFetch(StripeClient::Result result);
  void runConnect();
  void onWifiSelectionComplete(bool connected);
  void stopWifi();
  void finishBackground(bool succeeded);
  void openAutoRefreshPicker();
  void openRangePicker();
  void toggleAmount();
  void slide(int periods);
  void checkFill();
  bool handleSideButtons();

  // Rebuilds `view` for the current range and offset; caller holds the render lock.
  void rebuildView();
  static void loadMonth(void* self, int year, int month, IncomeMonth& out);
  void composeMonth(int year, int month, IncomeMonth& out) const;

  void renderDashboard(int width, int height) const;
  // Returns the bottom y of the hero block.
  int renderHero(int x, int y, int width, bool landscape) const;
  // Status under the period total: progress while refreshing, time or error otherwise.
  int renderStatus(int x, int y, int width, bool alignRight) const;
  void renderChart(int x, int y, int width, int height) const;
  void renderMessage(const char* title, const char* detail) const;
};
