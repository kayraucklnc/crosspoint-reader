#pragma once

#include <cstdint>
#include <ctime>

#include "StripeClient.h"

// Month-to-date income persisted on SD so a refresh only has to re-fetch the
// five-day chart window. Days that leave the window are frozen here.
struct IncomeCache {
  static constexpr const char* PATH = "/.crosspoint/income_cache.json";
  static constexpr int MAX_DAYS = 31;

  int monthKey = 0;  // YYYYMM
  uint8_t amountMode = 0;
  int64_t closedCents[MAX_DAYS + 1] = {};    // indexed by day of month
  uint16_t closedNonUsd[MAX_DAYS + 1] = {};  // uncounted non-USD charges, same days
  uint32_t closedKnown = 0;                  // bit n set = closedCents[n] is final
  IncomeSummary lastSummary;                 // what was last shown, for instant display
  bool hasSummary = false;
  time_t balanceDeniedAt = 0;  // when the key was last found without Balance read (0 = it has it)

  bool isKnown(const int day) const { return (closedKnown >> day) & 1u; }
  void setClosed(const int day, const int64_t cents, const uint16_t nonUsd) {
    closedCents[day] = cents;
    closedNonUsd[day] = nonUsd;
    closedKnown |= 1u << day;
  }

  // Missing or corrupt files leave the cache empty and return false.
  bool load();
  bool save() const;
};
