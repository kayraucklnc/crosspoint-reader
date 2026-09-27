#pragma once

// Which pack puzzle is the daily for a given day, freestanding.
//
// Days count from a language's launch day. Each run of `count` days (a cycle)
// deals every puzzle in the pack exactly once, in an order shuffled by the
// cycle number and a per-language salt, so the daily never repeats until the
// pack is used up and needs no state: the same day always deals the same
// puzzle for as long as the pack is unchanged.

#include <cstdint>

namespace connections {

// Pack index of the daily for `day` (0 = launch day), or -1 when count < 1.
int dailyPuzzle(uint32_t day, int count, uint32_t salt);

}  // namespace connections
