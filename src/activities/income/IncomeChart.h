#pragma once

#include <cstdint>
#include <ctime>

class GfxRenderer;

// Smoothed line chart with a dithered gradient fill, oldest point on the left.
struct IncomeChartData {
  static constexpr int MAX_POINTS = 5;
  int64_t cents[MAX_POINTS] = {};
  time_t dayStart[MAX_POINTS] = {};
  int count = 0;
};

void drawIncomeChart(const GfxRenderer& renderer, const IncomeChartData& data, int x, int y, int width, int height);
