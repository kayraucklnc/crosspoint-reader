#pragma once

#include "IncomeView.h"

class GfxRenderer;

// Smoothed line chart with a dithered gradient fill, oldest bucket on the left.
// The present bucket gets a hollow dot and an underlined label; future and
// missing buckets get a faded label and no point.
void drawIncomeChart(const GfxRenderer& renderer, const IncomeView& view, int x, int y, int width, int height);
