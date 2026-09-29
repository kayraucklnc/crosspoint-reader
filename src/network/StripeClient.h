#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "StripeHttp.h"

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
  // PARTIAL: the time budget ran out; what was fetched is saved, call again to continue.
  enum class Result { OK, PARTIAL, NO_KEY, NO_TIME, LOW_MEMORY, HTTP_ERROR, INVALID_KEY, FORBIDDEN };
  // GROSS: successful USD charges less refunds, by the charge's day (Stripe's
  // Gross volume); other currencies are counted as nonUsd. NET: balance-
  // transaction net (after fees and refunds), a second balance currency
  // converted at its exchange rate.
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

  // Blocking; requires WiFi. Gross fetches only the days not yet final on SD:
  // the five-day window first, then this month's missing days, newest first,
  // saving after every page; it starts no new page after budgetMs and then
  // returns PARTIAL. backfillOnly skips the window (it was just refreshed).
  // Net re-fetches the window plus any missing closed days of this month.
  static Result fetchIncome(Amount amount, IncomeSummary& out, uint32_t budgetMs, bool backfillOnly = false);

  // Blocking; requires WiFi. Fetches the missing days of one month that has
  // ended and stores them on SD (income_history), so each is fetched once.
  // Gross saves after every page and may return PARTIAL like fetchIncome.
  static Result fetchMonth(Amount amount, int year, int month, IncomeMonth& out, uint32_t budgetMs);

  // Days of this month still missing after the last fetchIncome().
  static int pendingDays();
  // Days the last Gross fetchIncome() or fetchMonth() stored for good, even when it then failed.
  static int lastDaysSaved();

  // Requests (with retries) and body bytes of the last fetchIncome() or fetchMonth() call.
  static int lastRequestCount();
  static uint32_t lastBytes();

  // Why the last failed call failed.
  static const stripe_http::Report& lastFailure();
};
