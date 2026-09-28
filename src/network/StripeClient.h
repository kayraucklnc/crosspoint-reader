#pragma once

#include <cstdint>
#include <ctime>
#include <string>

struct IncomeCache;
struct IncomeMonth;

// Income in USD. cents[0] is today, cents[4] is four days ago.
struct IncomeSummary {
  static constexpr int DAY_COUNT = 5;
  int64_t cents[DAY_COUNT] = {};
  time_t dayStart[DAY_COUNT] = {};
  int64_t monthCents = 0;  // month to date
  time_t monthStart = 0;
  time_t fetchedAt = 0;
  uint16_t nonUsd[DAY_COUNT] = {};  // per window day, like cents
  // Month-to-date charges in other currencies left out because the key cannot
  // read their USD balance transactions (Gross mode without Balance read).
  int nonUsdCharges = 0;
};

class StripeClient {
 public:
  enum class Result { OK, NO_KEY, NO_TIME, LOW_MEMORY, HTTP_ERROR, INVALID_KEY, FORBIDDEN };
  // GROSS: charges minus refunds before fees; NET: balance-transaction net (after fees and refunds).
  // Both read balance transactions, which Stripe records in the account's USD
  // settlement currency, so charges in any currency count. A GROSS key without
  // Balance read falls back to the charges list, where only USD charges count.
  enum class Amount : uint8_t { GROSS = 0, NET = 1 };

  static constexpr const char* KEY_PATH = "/.crosspoint/stripe.json";

  // Reads {"key":"rk_..."} from the SD card. Never logs the key.
  static bool loadKey(std::string& outKey);

  // Last summary saved by fetchIncome() for this amount mode, for instant display.
  static bool loadCachedSummary(Amount amount, IncomeSummary& out);
  // The whole month-to-date cache (closed days plus the last summary) for this amount mode.
  static bool loadCache(Amount amount, IncomeCache& out);
  // Mode byte closed months of this amount are cached under (income_history).
  static uint8_t monthMode(Amount amount);

  // Blocking; requires WiFi. Re-fetches only the five-day window (plus any
  // closed days of this month missing from the SD cache) and updates the cache.
  static Result fetchIncome(Amount amount, IncomeSummary& out);

  // Blocking; requires WiFi. Fetches one month that has ended, day by day, and
  // stores it on SD (income_history) so it is never fetched again.
  static Result fetchMonth(Amount amount, int year, int month, IncomeMonth& out);

  // Requests made by the last fetchIncome() or fetchMonth() call.
  static int lastRequestCount();
};
