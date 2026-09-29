#pragma once

#include <cstdint>
#include <ctime>

#include "StripeClient.h"

// Month-to-date income persisted on SD so a refresh only fetches days that are
// not final yet. Gross freezes a day once it was fetched SETTLE_SECONDS after
// it ended (stripe_gross); Net once it leaves the five-day window.
struct IncomeCache {
  static constexpr const char* PATH = "/.crosspoint/income_cache.json";  // Net
  static constexpr const char* GROSS_PATH = "/.crosspoint/income_gross.json";
  static constexpr int MAX_DAYS = 31;

  int monthKey = 0;  // YYYYMM
  uint8_t amountMode = 0;
  int64_t closedCents[MAX_DAYS + 1] = {};    // indexed by day of month
  uint16_t closedNonUsd[MAX_DAYS + 1] = {};  // uncounted non-USD charges, same days
  uint32_t closedKnown = 0;                  // bit n set = closedCents[n] is final
  IncomeSummary lastSummary;                 // what was last shown, for instant display
  bool hasSummary = false;
  time_t refundsCheckedAt = 0;  // refunds created since then have not been looked at yet (Gross)

  bool isKnown(const int day) const { return (closedKnown >> day) & 1u; }
  void setClosed(const int day, const int64_t cents, const uint16_t nonUsd) {
    closedCents[day] = cents;
    closedNonUsd[day] = nonUsd;
    closedKnown |= 1u << day;
  }
  void forget(const int day) { closedKnown &= ~(1u << day); }

  // Missing or corrupt files leave the cache empty and return false.
  bool load(const char* path = PATH);
  bool save(const char* path = PATH) const;
};
