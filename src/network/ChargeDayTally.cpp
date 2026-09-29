#include "ChargeDayTally.h"

#include <algorithm>
#include <iterator>

ChargeDayTally::ChargeDayTally(const int firstDay, const int lastDay)
    : firstDay(std::max(1, firstDay)), topDay(std::min(MAX_DAYS, lastDay)) {}

void ChargeDayTally::add(const int day, const int64_t cents, const bool nonUsd) {
  // Out of range means a clock or timezone disagreement; those records belong to no day here.
  if (day < firstDay || day > topDay) return;
  if (nonUsd) {
    pageNonUsd[day]++;
  } else {
    pageCents[day] += cents;
  }
  pageOldest = pageOldest == 0 ? day : std::min(pageOldest, day);
}

void ChargeDayTally::discardPage() {
  std::fill(std::begin(pageCents), std::end(pageCents), 0);
  std::fill(std::begin(pageNonUsd), std::end(pageNonUsd), 0);
  pageOldest = 0;
}

int ChargeDayTally::commitPage(const bool hasMore) {
  for (int day = firstDay; day <= topDay; day++) {
    totalCents[day] += pageCents[day];
    totalNonUsd[day] += pageNonUsd[day];
  }
  int lowest = topDay + 1;
  if (!hasMore) {
    lowest = firstDay;
  } else if (pageOldest > 0) {
    lowest = pageOldest + 1;
  }
  discardPage();
  if (lowest <= topDay) topDay = lowest - 1;
  return lowest;
}
