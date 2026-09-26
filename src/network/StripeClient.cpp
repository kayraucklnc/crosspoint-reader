#include "StripeClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include "IncomeCache.h"
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

// Cached numbers are only valid for the amount mode and day boundary they were computed with.
uint8_t cacheMode(const StripeClient::Amount amount) {
  return static_cast<uint8_t>(static_cast<uint8_t>(amount) | (SETTINGS.incomeDayUtc ? 2 : 0));
}

// Day boundaries follow the device timezone, or UTC when incomeDayUtc is set
// (Stripe's dashboard buckets by UTC day).
bool utcDays() { return SETTINGS.incomeDayUtc != 0; }

void calendarOf(const time_t t, struct tm& out) {
  if (utcDays()) {
    gmtime_r(&t, &out);
  } else {
    localtime_r(&t, &out);
  }
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
bool recordValue(const StripeClient::Amount amount, const StripeCharge& record, int64_t& cents) {
  if (record.currency != "usd") return false;
  if (amount == StripeClient::Amount::GROSS) {
    if (record.status != "succeeded") return false;
    cents = record.amount - record.amountRefunded;
    return true;
  }
  static constexpr const char* NET_TYPES[] = {"charge", "payment", "refund", "payment_refund", "adjustment"};
  for (const char* type : NET_TYPES) {
    if (record.type == type) {
      cents = record.net;
      return true;
    }
  }
  return false;
}


// Streams every record created at or after `since` into onRecord.
StripeClient::Result fetchRecords(const StripeClient::Amount amount, const std::string& authorization,
                                  const time_t since, const StripeChargeScanner::ChargeCallback& onRecord) {
  const char* base = amount == StripeClient::Amount::GROSS ? CHARGES_URL : BALANCE_URL;
  std::string startingAfter;
  for (int page = 0; page < MAX_PAGES; page++) {
    char url[256];
    snprintf(url, sizeof(url), "%s?limit=%d&created%%5Bgte%%5D=%lld%s%s", base, PAGE_LIMIT,
             static_cast<long long>(since), startingAfter.empty() ? "" : "&starting_after=", startingAfter.c_str());

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

bool StripeClient::loadCachedSummary(const Amount amount, IncomeSummary& out) {
  IncomeCache cache;
  if (!cache.load() || cache.amountMode != cacheMode(amount) || !cache.hasSummary) return false;
  out = cache.lastSummary;
  return true;
}

int StripeClient::lastRequestCount() { return requestCount; }

StripeClient::Result StripeClient::fetchIncome(const Amount amount, IncomeSummary& out) {
  requestCount = 0;
  std::string key;
  if (!loadKey(key)) return Result::NO_KEY;

  time_t now = 0;
  if (!network_time::ensure(now)) {
    LOG_ERR("STRIPE", "Clock not set; cannot bucket income by day");
    return Result::NO_TIME;
  }

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
  const bool cacheValid = cache.load() && cache.monthKey == monthKey && cache.amountMode == cacheMode(amount) &&
                          cache.lastSummary.fetchedAt <= now + FUTURE_SLACK;
  if (!cacheValid) {
    cache = IncomeCache{};
    cache.monthKey = monthKey;
    cache.amountMode = cacheMode(amount);
  } else if (cache.hasSummary) {
    // Freeze days that have slid out of the window since the last refresh.
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
      const time_t start = cache.lastSummary.dayStart[i];
      if (start < fresh.monthStart || start >= fresh.dayStart[WINDOW_OLDEST]) continue;
      struct tm day;
      calendarOf(start, day);
      // Only freeze days whose boundary matches today's calendar (TZ/clock consistency); others refetch.
      if (localMidnight(start + 3600, 0) != start) continue;
      if (!cache.isKnown(day.tm_mday)) cache.setClosed(day.tm_mday, cache.lastSummary.cents[i]);
    }
  }

  int firstUnknown = 0;
  for (int day = 1; day <= lastClosedDay && firstUnknown == 0; day++) {
    if (!cache.isKnown(day)) firstUnknown = day;
  }
  const time_t since =
      firstUnknown > 0 ? std::min(localMidnight(now, 0, firstUnknown), fresh.dayStart[WINDOW_OLDEST])
                       : fresh.dayStart[WINDOW_OLDEST];

  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("STRIPE", "Not enough heap for TLS: free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    return Result::LOW_MEMORY;
  }

  int64_t fetchedByDay[IncomeCache::MAX_DAYS + 1] = {};
  int futureRecords = 0;
  auto onRecord = [&](const StripeCharge& record) {
    int64_t cents = 0;
    if (!recordValue(amount, record, cents)) return;
    // A record "from the future" means our clock is behind; never let it land in today.
    if (record.created > now + FUTURE_SLACK) {
      futureRecords++;
      return;
    }
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
      if (record.created >= fresh.dayStart[i]) {
        fresh.cents[i] += cents;
        break;
      }
    }
    if (record.created >= fresh.monthStart) {
      const time_t created = static_cast<time_t>(record.created);
      struct tm day;
      calendarOf(created, day);
      fetchedByDay[day.tm_mday] += cents;
    }
  };

  const Result result = fetchRecords(amount, "Bearer " + key, since, onRecord);
  if (result != Result::OK) return result;
  if (futureRecords > 0) LOG_ERR("STRIPE", "Ignored %d records dated after now; clock is behind", futureRecords);

  for (int day = std::max(1, firstUnknown); firstUnknown > 0 && day <= lastClosedDay; day++) {
    cache.setClosed(day, fetchedByDay[day]);
  }
  for (int day = 1; day <= lastClosedDay; day++) fresh.monthCents += cache.closedCents[day];
  for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
    if (fresh.dayStart[i] >= fresh.monthStart) fresh.monthCents += fresh.cents[i];
  }

  cache.lastSummary = fresh;
  cache.hasSummary = true;
  cache.save();
  out = fresh;
  LOG_INF("STRIPE", "Income refreshed with %d request(s)%s", requestCount, firstUnknown > 0 ? " (month backfill)" : "");
  return Result::OK;
}
