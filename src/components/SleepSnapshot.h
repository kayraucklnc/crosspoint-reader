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
// The chip also carries the auto-refresh state: "· offline" after a failed
// refresh, "· battery low, auto off", and "· next 14:40" when a wake is planned;
// when that does not fit in `width` the notes get their own line under the chip.
// Side by side when `width` allows, otherwise stacked. `asOfClock` is "HH:MM".
// Returns the bottom y.
int drawBadge(const GfxRenderer& renderer, int x, int y, int width, bool alignRight, const char* asOfClock);

// Width the whole badge needs to fit on one line, notes included.
int inlineBadgeWidth(const GfxRenderer& renderer, const char* asOfClock);

}  // namespace sleep_snapshot
