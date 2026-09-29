#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "StripeClient.h"

// Gross income per day from successful charges (charges search, or the
// charges list when search is unavailable), fetched in small pages, newest first.
namespace stripe_charges {

// Charges per page: ~3.4 KB each, so a page stays near 85 KB.
constexpr int PAGE_LIMIT = 25;

// Receives each day as soon as no later page can change it.
class DaySink {
 public:
  virtual ~DaySink() = default;
  virtual void day(int day, int64_t cents, uint16_t nonUsd) = 0;
  // After each page that completed at least one day: a point to persist them.
  virtual void pageDone() = 0;
};

// Days firstDay..lastDay of year/month. Requests no new page once millis()
// passes `deadline` (the first page is always requested) and then returns
// PARTIAL; days already handed to the sink stay valid. `now` rejects records
// dated after it (a clock that is behind).
StripeClient::Result fetchDays(const std::string& authorization, int year, int month, int firstDay, int lastDay,
                               time_t now, unsigned long deadline, DaySink& sink);

// Creation times of charges refunded at or after `since` (at most maxCharges).
// `truncated` is set when there were more refunds than could be looked up.
StripeClient::Result refundedCharges(const std::string& authorization, time_t since, time_t* created, int maxCharges,
                                     int& count, bool& truncated);

}  // namespace stripe_charges
