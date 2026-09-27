#include <gtest/gtest.h>

#include <vector>

#include "activities/play/ConnectionsDaily.h"
#include "activities/play/ConnectionsResults.h"

using connections::dailyPuzzle;

namespace {

constexpr uint32_t SALT_EN = 0x454E3136u;
constexpr uint32_t SALT_TR = 0x54523236u;

// Every puzzle once per cycle, for pack sizes on both sides of a power of four.
TEST(ConnectionsDaily, EachCycleDealsEveryPuzzleOnce) {
  for (const int count : {1, 2, 3, 4, 5, 15, 16, 17, 523, 1196, 4096}) {
    for (uint32_t cycle = 0; cycle < 3; cycle++) {
      std::vector<int> seen(count, 0);
      for (int position = 0; position < count; position++) {
        const int index = dailyPuzzle(cycle * count + position, count, SALT_TR);
        ASSERT_GE(index, 0);
        ASSERT_LT(index, count);
        seen[index]++;
      }
      for (const int n : seen) EXPECT_EQ(n, 1) << "count " << count << " cycle " << cycle;
    }
  }
}

TEST(ConnectionsDaily, SameDaySamePuzzle) {
  for (uint32_t day = 0; day < 100; day++) {
    EXPECT_EQ(dailyPuzzle(day, 523, SALT_TR), dailyPuzzle(day, 523, SALT_TR));
  }
}

TEST(ConnectionsDaily, OrderIsShuffledAndDiffersByLanguageAndCycle) {
  int inOrder = 0, sameAcrossSalts = 0, sameAcrossCycles = 0;
  for (uint32_t day = 0; day < 200; day++) {
    inOrder += dailyPuzzle(day, 1196, SALT_EN) == static_cast<int>(day);
    sameAcrossSalts += dailyPuzzle(day, 1196, SALT_EN) == dailyPuzzle(day, 1196, SALT_TR);
    sameAcrossCycles += dailyPuzzle(day, 1196, SALT_EN) == dailyPuzzle(day + 1196, 1196, SALT_EN);
  }
  EXPECT_LT(inOrder, 5);
  EXPECT_LT(sameAcrossSalts, 5);
  EXPECT_LT(sameAcrossCycles, 5);
}

TEST(ConnectionsDaily, EmptyPackHasNoDaily) { EXPECT_EQ(dailyPuzzle(7, 0, SALT_EN), -1); }

// The streak counts back from today and skips today while it is unplayed.
TEST(ConnectionsDaily, StreakFromToday) {
  connections::Record won;
  won.played = true;
  const uint8_t w = connections::encodeRecord(won);
  const uint8_t results[] = {w, 0, w, w, w, 0};
  const connections::Summary summary = connections::summarise(results, sizeof(results), 5);
  EXPECT_EQ(summary.streak, 3);
  EXPECT_EQ(summary.bestStreak, 3);
  EXPECT_EQ(summary.played, 4);
}

}  // namespace
