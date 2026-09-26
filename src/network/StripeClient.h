#pragma once

#include <cstdint>
#include <ctime>
#include <string>

// USD income. cents[0] is today, cents[4] is four days ago.
struct IncomeSummary {
  static constexpr int DAY_COUNT = 5;
  int64_t cents[DAY_COUNT] = {};
  time_t dayStart[DAY_COUNT] = {};
  int64_t monthCents = 0;  // month to date
  time_t monthStart = 0;
  time_t fetchedAt = 0;
};

class StripeClient {
 public:
  enum class Result { OK, NO_KEY, NO_TIME, LOW_MEMORY, HTTP_ERROR, INVALID_KEY, FORBIDDEN };
  // GROSS sums succeeded charges; NET sums balance-transaction net (after fees and refunds).
  enum class Amount : uint8_t { GROSS = 0, NET = 1 };

  static constexpr const char* KEY_PATH = "/.crosspoint/stripe.json";

  // Reads {"key":"rk_..."} from the SD card. Never logs the key.
  static bool loadKey(std::string& outKey);

  // Last summary saved by fetchIncome() for this amount mode, for instant display.
  static bool loadCachedSummary(Amount amount, IncomeSummary& out);

  // Blocking; requires WiFi. Re-fetches only the five-day window (plus any
  // closed days of this month missing from the SD cache) and updates the cache.
  static Result fetchIncome(Amount amount, IncomeSummary& out);

  // Requests made by the last fetchIncome() call.
  static int lastRequestCount();
};
