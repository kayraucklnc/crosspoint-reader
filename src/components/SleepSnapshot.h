#pragma once

class GfxRenderer;

// Chrome for a dashboard left on the panel through deep sleep: a hatched frame
// that reads as "not live" at a glance, and a badge naming the data's age and
// how to wake the device.
namespace sleep_snapshot {

// Width of the hatched band along every screen edge.
constexpr int FRAME_WIDTH = 10;

// Hatched band around the whole screen, with a solid rule on its inner edge.
void drawFrame(const GfxRenderer& renderer);

// "(moon) Asleep · as of 14:32" in a black chip, plus "(power) Hold to wake".
// Side by side when `width` allows, otherwise stacked. `asOfClock` is "HH:MM".
// Returns the bottom y.
int drawBadge(const GfxRenderer& renderer, int x, int y, int width, bool alignRight, const char* asOfClock);

}  // namespace sleep_snapshot
