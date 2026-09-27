#pragma once

#include <memory>

#include "activities/Activity.h"
#include "util/ReadingStats.h"

// Reading statistics dashboard: today / this week / streak / total, a
// weekday-average chart, the last eight weeks, and the most-read books.
// Adapts to portrait (stacked) and landscape (two columns).
class ReadingStatsActivity final : public Activity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  // ~1.2 KB, only while the screen is open.
  std::unique_ptr<reading_stats::Summary> summary;

  int drawHero(int x, int y, int width) const;
  int drawInsight(int x, int y, int width) const;
  void drawWeekdayChart(int x, int y, int width, int height) const;
  void drawWeeksChart(int x, int y, int width, int height) const;
  void drawTopBooks(int x, int y, int width, int height) const;
};
