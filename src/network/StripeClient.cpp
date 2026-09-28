#include "StripeClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include "IncomeCache.h"
#include "IncomeCalendar.h"
#include "IncomeHistory.h"
#include "NetworkTime.h"
#include "StripeChargeScanner.h"

namespace {
constexpr const char* CHARGES_URL = "https://api.stripe.com/v1/charges";
constexpr const char* BALANCE_URL = "https://api.stripe.com/v1/balance_transactions";
constexpr int PAGE_LIMIT = 100;
constexpr int MAX_PAGES = 30;
constexpr int WINDOW_OLDEST = IncomeSummary::DAY_COUNT - 1;

int requestCount = 0;
constexpr time_t FUTURE_SLACK = 600;
// A key found without Balance read goes straight to the charges list for this
// long, so a Gross refresh stays one request; then balance is tried again.
constexpr time_t BALANCE_REPROBE_SECONDS = 12 * 3600;

// Where GROSS numbers come from: balance transactions (every currency, settled
// in USD) or, for keys without Balance read, the charges list (USD only).
enum class Source : uint8_t { BALANCE, CHARGES };
constexpr uint8_t CHARGES_SOURCE_BIT = 4;

// Cached numbers are only valid for the amount mode, source and day boundary they were computed with.
uint8_t cacheMode(const StripeClient::Amount amount, const Source source) {
  return static_cast<uint8_t>(static_cast<uint8_t>(amount) | (SETTINGS.incomeDayUtc ? 2 : 0) |
                              (source == Source::CHARGES ? CHARGES_SOURCE_BIT : 0));
}

using income_calendar::calendarOf;
using income_calendar::utcDays;

Source sourceFor(const StripeClient::Amount amount) {
  return amount == StripeClient::Amount::GROSS ? Source::CHARGES : Source::BALANCE;
}

time_t localMidnight(const time_t now, const int daysAgo, const int dayOfMonth = 0) {
  struct tm local;
  calendarOf(now, local);
  if (utcDays()) {
    // UTC has no DST: midnight is plain arithmetic, no mktime (which is local).
    const time_t todayStart = now - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    const int back = dayOfMonth > 0 ? local.tm_mday - dayOfMonth + daysAgo : daysAgo;
    return todayStart - static_cast<time_t>(back) * 86400;
  }
  if (dayOfMonth > 0) local.tm_mday = dayOfMonth;
  local.tm_mday -= daysAgo;  // mktime normalizes month/year rollover and DST
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return mktime(&local);
}

// Value of one record in cents, or false if it doesn't count toward income.
bool recordValue(const StripeClient::Amount amount, const Source source, const StripeCharge& record, int64_t& cents) {
  if (record.currency != "usd") return false;
  if (source == Source::CHARGES) {
    if (record.status != "succeeded") return false;
    cents = record.amount - record.amountRefunded;
    return true;
  }
  // Net: every balance movement except money leaving to the bank or other
  // accounts. Stripe may bill fees as separate "stripe_fee" rows (fee 0 on the
  // charge), so fee and adjustment rows must count.
  static constexpr const char* OUTFLOW_PREFIXES[] = {"payout", "transfer", "topup", "reserve"};
  for (const char* prefix : OUTFLOW_PREFIXES) {
    if (record.type.rfind(prefix, 0) == 0) return false;
  }
  cents = record.net;
  return true;
}

// A second balance currency (e.g. GBP), converted to USD at the first exchange
// rate its charge rows carry in the same fetch.
struct ForeignRate {
  std::string currency;
  double perUsd = 0;

  int64_t toUsd(const int64_t foreignCents) const {
    return static_cast<int64_t>(static_cast<double>(foreignCents) / perUsd + (foreignCents < 0 ? -0.5 : 0.5));
  }
};

// How one record counts: in USD, in the foreign balance currency (converted
// once the fetch is done), as a non-USD charge the charges list cannot value, or not at all.
enum class Counted : uint8_t { NO, USD, FOREIGN, SKIPPED_NON_USD };

Counted classify(const StripeClient::Amount amount, const Source source, const StripeCharge& record, ForeignRate& fx,
                 int64_t& cents) {
  if (source == Source::BALANCE && record.currency != "usd") {
    if (fx.currency.empty()) fx.currency = record.currency;
    if (record.currency != fx.currency) return Counted::NO;
    if (fx.perUsd == 0 && record.exchangeRate > 0) fx.perUsd = record.exchangeRate;
    StripeCharge asUsd = record;
    asUsd.currency = "usd";
    return recordValue(amount, source, asUsd, cents) ? Counted::FOREIGN : Counted::NO;
  }
  if (recordValue(amount, source, record, cents)) return Counted::USD;
  // Only the charges list can hold other currencies without a USD value.
  const bool skippedForeign = source == Source::CHARGES && record.currency != "usd" && record.status == "succeeded";
  return skippedForeign ? Counted::SKIPPED_NON_USD : Counted::NO;
}

// Logs whether the foreign balance currency could be converted; false = its movements are left out.
bool reportForeign(const ForeignRate& fx) {
  if (fx.currency.empty()) return false;
  if (fx.perUsd > 0) {
    LOG_INF("STRIPE", "Converted %s balance at %.4f per USD", fx.currency.c_str(), fx.perUsd);
    return true;
  }
  LOG_ERR("STRIPE", "No %s exchange rate in this fetch; %s movements left out", fx.currency.c_str(),
          fx.currency.c_str());
  return false;
}

// Streams every record created at or after `since` into onRecord.
// `until` (0 = open-ended) is exclusive.
StripeClient::Result fetchRecords(const Source source, const std::string& authorization, const time_t since,
                                  const time_t until, const StripeChargeScanner::ChargeCallback& onRecord) {
  const char* base = source == Source::CHARGES ? CHARGES_URL : BALANCE_URL;
  char untilParam[40] = "";
  if (until > 0) snprintf(untilParam, sizeof(untilParam), "&created%%5Blt%%5D=%lld", static_cast<long long>(until));
  std::string startingAfter;
  for (int page = 0; page < MAX_PAGES; page++) {
    char url[256];
    snprintf(url, sizeof(url), "%s?limit=%d&created%%5Bgte%%5D=%lld%s%s%s", base, PAGE_LIMIT,
             static_cast<long long>(since), untilParam,
             startingAfter.empty() ? "" : "&starting_after=", startingAfter.c_str());

    StripeChargeScanner scanner(onRecord);
    int status = 0;
    requestCount++;
    const bool ok = HttpDownloader::fetchUrlWithAuthorization(
        url,
        [&scanner](const uint8_t* data, const size_t len) {
          scanner.feed(data, len);
          return true;
        },
        authorization, &status);
    if (!ok) {
      LOG_ERR("STRIPE", "Request failed on page %d (HTTP %d)", page, status);
      if (status == 401) return StripeClient::Result::INVALID_KEY;
      if (status == 403) return StripeClient::Result::FORBIDDEN;
      return StripeClient::Result::HTTP_ERROR;
    }
    LOG_DBG("STRIPE", "Page %d: %u records", page, static_cast<unsigned>(scanner.chargeCount()));
    if (!scanner.hasMore() || scanner.lastId().empty()) break;
    startingAfter = scanner.lastId();
  }
  return StripeClient::Result::OK;
}
}  // namespace

bool StripeClient::loadKey(std::string& outKey) {
  outKey.clear();
  if (!Storage.exists(KEY_PATH)) return false;
  const String raw = Storage.readFile(KEY_PATH);
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok) {
    LOG_ERR("STRIPE", "Could not parse %s", KEY_PATH);
    return false;
  }
  const char* key = doc["key"] | "";
  outKey = key;
  return !outKey.empty();
}

bool StripeClient::loadCache(const Amount amount, IncomeCache& out) {
  // Either source's numbers are fine to show while the refresh runs.
  if (!out.load() || (out.amountMode & ~CHARGES_SOURCE_BIT) != cacheMode(amount, Source::BALANCE) || !out.hasSummary) {
    out = IncomeCache{};
    return false;
  }
  return true;
}

bool StripeClient::loadCachedSummary(const Amount amount, IncomeSummary& out) {
  IncomeCache cache;
  if (!loadCache(amount, cache)) return false;
  out = cache.lastSummary;
  return true;
}

uint8_t StripeClient::monthMode(const Amount amount) { return cacheMode(amount, sourceFor(amount)); }

int StripeClient::lastRequestCount() { return requestCount; }

namespace {
// One refresh from one source: re-fetches the five-day window plus any closed
// days of this month missing from the SD cache, then updates the cache.
// `balanceDeniedAt` is stored with it (0 once balance transactions worked).
StripeClient::Result fetchFrom(const StripeClient::Amount amount, const Source source, const std::string& key,
                               const time_t now, const time_t balanceDeniedAt, IncomeSummary& out) {
  IncomeSummary fresh;
  fresh.fetchedAt = now;
  for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) fresh.dayStart[i] = localMidnight(now, i);
  fresh.monthStart = localMidnight(now, 0, 1);
  struct tm local;
  calendarOf(now, local);
  const int monthKey = (local.tm_year + 1900) * 100 + local.tm_mon + 1;
  const int today = local.tm_mday;
  const int lastClosedDay = today - IncomeSummary::DAY_COUNT;  // days <= this are outside the window

  IncomeCache cache;
  // A cache written by a run with a wrong clock (fetched "in the future") is not trusted.
  const bool cacheValid = cache.load() && cache.monthKey == monthKey && cache.amountMode == cacheMode(amount, source) &&
                          cache.lastSummary.fetchedAt <= now + FUTURE_SLACK;
  if (!cacheValid) {
    cache = IncomeCache{};
    cache.monthKey = monthKey;
    cache.amountMode = cacheMode(amount, source);
  } else if (cache.hasSummary) {
    // Freeze days that have slid out of the window since the last refresh.
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
      const time_t start = cache.lastSummary.dayStart[i];
      if (start < fresh.monthStart || start >= fresh.dayStart[WINDOW_OLDEST]) continue;
      struct tm day;
      calendarOf(start, day);
      // Only freeze days whose boundary matches today's calendar (TZ/clock consistency); others refetch.
      if (localMidnight(start + 3600, 0) != start) continue;
      if (!cache.isKnown(day.tm_mday)) {
        cache.setClosed(day.tm_mday, cache.lastSummary.cents[i], cache.lastSummary.nonUsd[i]);
      }
    }
  }
  cache.balanceDeniedAt = balanceDeniedAt;

  int firstUnknown = 0;
  for (int day = 1; day <= lastClosedDay && firstUnknown == 0; day++) {
    if (!cache.isKnown(day)) firstUnknown = day;
  }
  const time_t since = firstUnknown > 0 ? std::min(localMidnight(now, 0, firstUnknown), fresh.dayStart[WINDOW_OLDEST])
                                        : fresh.dayStart[WINDOW_OLDEST];

  int64_t fetchedByDay[IncomeCache::MAX_DAYS + 1] = {};
  uint16_t nonUsdByDay[IncomeCache::MAX_DAYS + 1] = {};
  int64_t foreignByDay[IncomeCache::MAX_DAYS + 1] = {};
  int64_t foreignWindow[IncomeSummary::DAY_COUNT] = {};
  ForeignRate fx;
  int futureRecords = 0;
  auto onRecord = [&](const StripeCharge& record) {
    // A record "from the future" means our clock is behind; never let it land in today.
    if (record.created > now + FUTURE_SLACK) {
      futureRecords++;
      return;
    }
    int64_t cents = 0;
    const Counted kind = classify(amount, source, record, fx, cents);
    if (kind == Counted::NO) return;
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
      if (record.created >= fresh.dayStart[i]) {
        if (kind == Counted::USD) fresh.cents[i] += cents;
        if (kind == Counted::FOREIGN) foreignWindow[i] += cents;
        if (kind == Counted::SKIPPED_NON_USD) fresh.nonUsd[i]++;
        break;
      }
    }
    if (record.created >= fresh.monthStart) {
      const time_t created = static_cast<time_t>(record.created);
      struct tm day;
      calendarOf(created, day);
      if (kind == Counted::USD) fetchedByDay[day.tm_mday] += cents;
      if (kind == Counted::FOREIGN) foreignByDay[day.tm_mday] += cents;
      if (kind == Counted::SKIPPED_NON_USD) nonUsdByDay[day.tm_mday]++;
    }
  };

  const StripeClient::Result result = fetchRecords(source, "Bearer " + key, since, 0, onRecord);
  if (result != StripeClient::Result::OK) return result;
  if (futureRecords > 0) LOG_ERR("STRIPE", "Ignored %d records dated after now; clock is behind", futureRecords);
  if (reportForeign(fx)) {
    for (int day = 1; day <= IncomeCache::MAX_DAYS; day++) fetchedByDay[day] += fx.toUsd(foreignByDay[day]);
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) fresh.cents[i] += fx.toUsd(foreignWindow[i]);
  }

  for (int day = std::max(1, firstUnknown); firstUnknown > 0 && day <= lastClosedDay; day++) {
    cache.setClosed(day, fetchedByDay[day], nonUsdByDay[day]);
  }
  for (int day = 1; day <= lastClosedDay; day++) {
    fresh.monthCents += cache.closedCents[day];
    fresh.nonUsdCharges += cache.closedNonUsd[day];
  }
  for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
    if (fresh.dayStart[i] < fresh.monthStart) continue;
    fresh.monthCents += fresh.cents[i];
    fresh.nonUsdCharges += fresh.nonUsd[i];
  }

  cache.lastSummary = fresh;
  cache.hasSummary = true;
  cache.save();
  out = fresh;
  LOG_INF("STRIPE", "Income refreshed from %s with %d request(s)%s", source == Source::CHARGES ? "charges" : "balance",
          requestCount, firstUnknown > 0 ? " (month backfill)" : "");
  return StripeClient::Result::OK;
}
}  // namespace

namespace {
// Key, trusted clock and TLS heap, checked before every fetch.
StripeClient::Result preflight(std::string& key, time_t& now) {
  requestCount = 0;
  if (!StripeClient::loadKey(key)) return StripeClient::Result::NO_KEY;
  if (!network_time::ensure(now)) {
    LOG_ERR("STRIPE", "Clock not set; cannot bucket income by day");
    return StripeClient::Result::NO_TIME;
  }
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("STRIPE", "Not enough heap for TLS: free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    return StripeClient::Result::LOW_MEMORY;
  }
  return StripeClient::Result::OK;
}
}  // namespace

StripeClient::Result StripeClient::fetchIncome(const Amount amount, IncomeSummary& out) {
  std::string key;
  time_t now = 0;
  if (const Result ready = preflight(key, now); ready != Result::OK) return ready;

  // Gross matches Stripe's "Gross volume": successful charges less refunds. Balance
  // transactions miss charges that have not settled yet, so they undercount it.
  if (amount == Amount::GROSS) return fetchFrom(amount, Source::CHARGES, key, now, 0, out);

  // A Gross key already known to lack Balance read skips the probe until it is due again.
  IncomeCache previous;
  previous.load();
  const time_t deniedAt = previous.balanceDeniedAt;
  const bool denied = amount == Amount::GROSS && deniedAt > 0 && deniedAt <= now + FUTURE_SLACK &&
                      now - deniedAt < BALANCE_REPROBE_SECONDS;
#ifdef SIMULATOR
  // QA hook: CROSSPOINT_SIM_STRIPE_NO_BALANCE=1 answers balance requests as a key without Balance read.
  const bool simulateNoBalance = getenv("CROSSPOINT_SIM_STRIPE_NO_BALANCE") != nullptr;
#else
  constexpr bool simulateNoBalance = false;
#endif
  if (!denied && !(simulateNoBalance && amount == Amount::GROSS)) {
    const Result result = fetchFrom(amount, Source::BALANCE, key, now, 0, out);
    if (result != Result::FORBIDDEN || amount != Amount::GROSS) return result;
    LOG_INF("STRIPE", "Key lacks Balance read; using USD charges only");
  }
  if (amount != Amount::GROSS) return Result::FORBIDDEN;
  return fetchFrom(amount, Source::CHARGES, key, now, denied ? deniedAt : now, out);
}

StripeClient::Result StripeClient::fetchMonth(const Amount amount, const int year, const int month, IncomeMonth& out) {
  out = IncomeMonth{};
  std::string key;
  time_t now = 0;
  if (const Result ready = preflight(key, now); ready != Result::OK) return ready;

  const time_t start = income_calendar::dayStart(year, month, 1);
  const time_t end = income_calendar::dayStart(year, month + 1, 1);
  if (end > now) {
    LOG_ERR("STRIPE", "Month %04d-%02d has not ended; not caching it", year, month);
    return Result::NO_TIME;
  }

  const Source source = sourceFor(amount);
  // Foreign balance cents per day, converted once the fetch has found a rate.
  auto foreignByDay = makeUniqueNoThrow<int64_t[]>(IncomeMonth::MAX_DAYS + 1);
  if (!foreignByDay) {
    LOG_ERR("STRIPE", "OOM: month buffer");
    return Result::LOW_MEMORY;
  }
  std::fill_n(foreignByDay.get(), IncomeMonth::MAX_DAYS + 1, 0);
  ForeignRate fx;
  auto onRecord = [&](const StripeCharge& record) {
    int64_t cents = 0;
    const Counted kind = classify(amount, source, record, fx, cents);
    if (kind == Counted::NO) return;
    const time_t created = static_cast<time_t>(record.created);
    struct tm day;
    calendarOf(created, day);
    if (day.tm_year + 1900 != year || day.tm_mon + 1 != month) return;  // outside [start, end)
    if (kind == Counted::USD) out.cents[day.tm_mday] += cents;
    if (kind == Counted::FOREIGN) foreignByDay[day.tm_mday] += cents;
    if (kind == Counted::SKIPPED_NON_USD) out.nonUsd[day.tm_mday]++;
  };
  const Result result = fetchRecords(source, "Bearer " + key, start, end, onRecord);
  if (result != Result::OK) return result;

  const bool converted = reportForeign(fx);
  const int days = income_calendar::daysInMonth(year, month);
  for (int day = 1; day <= days; day++) {
    const int64_t foreign = converted ? fx.toUsd(foreignByDay[day]) : 0;
    out.set(day, out.cents[day] + foreign, out.nonUsd[day]);
  }
  income_history::save(income_calendar::monthKey(year, month), amount == Amount::NET, monthMode(amount), out);
  LOG_INF("STRIPE", "Month %04d-%02d cached from %s with %d request(s)", year, month,
          source == Source::CHARGES ? "charges" : "balance", requestCount);
  return Result::OK;
}
