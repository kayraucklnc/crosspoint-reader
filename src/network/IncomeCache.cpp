#include "IncomeCache.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

namespace {
constexpr int CACHE_VERSION = 3;  // 3: Gross from charges, Net includes fee rows
}

bool IncomeCache::load(const char* path) {
  *this = IncomeCache{};
  if (!Storage.exists(path)) return false;
  const String raw = Storage.readFile(path);
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok || doc["v"] != CACHE_VERSION) {
    LOG_ERR("INCOME", "Ignoring unreadable %s", path);
    return false;
  }
  monthKey = doc["month"] | 0;
  amountMode = doc["mode"] | 0;
  closedKnown = doc["known"] | 0u;
  JsonArrayConst closed = doc["closed"];
  for (int day = 1; day <= MAX_DAYS && day <= static_cast<int>(closed.size()); day++) {
    closedCents[day] = closed[day - 1] | static_cast<int64_t>(0);
  }
  JsonArrayConst closedNonUsdJson = doc["closedNonUsd"];
  for (int day = 1; day <= MAX_DAYS && day <= static_cast<int>(closedNonUsdJson.size()); day++) {
    closedNonUsd[day] = closedNonUsdJson[day - 1] | static_cast<uint16_t>(0);
  }
  refundsCheckedAt = doc["refundsAt"] | static_cast<int64_t>(0);

  JsonObjectConst last = doc["last"];
  if (!last.isNull()) {
    JsonArrayConst cents = last["cents"];
    JsonArrayConst starts = last["dayStart"];
    if (cents.size() == IncomeSummary::DAY_COUNT && starts.size() == IncomeSummary::DAY_COUNT) {
      for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
        lastSummary.cents[i] = cents[i] | static_cast<int64_t>(0);
        lastSummary.dayStart[i] = starts[i] | static_cast<int64_t>(0);
      }
      lastSummary.monthCents = last["month"] | static_cast<int64_t>(0);
      lastSummary.monthStart = last["monthStart"] | static_cast<int64_t>(0);
      lastSummary.fetchedAt = last["fetchedAt"] | static_cast<int64_t>(0);
      JsonArrayConst nonUsd = last["nonUsd"];
      for (int i = 0; i < IncomeSummary::DAY_COUNT && i < static_cast<int>(nonUsd.size()); i++) {
        lastSummary.nonUsd[i] = nonUsd[i] | static_cast<uint16_t>(0);
      }
      lastSummary.nonUsdCharges = last["nonUsdMonth"] | 0;
      hasSummary = true;
    }
  }
  return monthKey != 0;
}

bool IncomeCache::save(const char* path) const {
  JsonDocument doc;
  doc["v"] = CACHE_VERSION;
  doc["month"] = monthKey;
  doc["mode"] = amountMode;
  doc["known"] = closedKnown;
  JsonArray closed = doc["closed"].to<JsonArray>();
  for (int day = 1; day <= MAX_DAYS; day++) closed.add(closedCents[day]);
  JsonArray closedNonUsdJson = doc["closedNonUsd"].to<JsonArray>();
  for (int day = 1; day <= MAX_DAYS; day++) closedNonUsdJson.add(closedNonUsd[day]);
  if (refundsCheckedAt > 0) doc["refundsAt"] = static_cast<int64_t>(refundsCheckedAt);

  if (hasSummary) {
    JsonObject last = doc["last"].to<JsonObject>();
    JsonArray cents = last["cents"].to<JsonArray>();
    JsonArray starts = last["dayStart"].to<JsonArray>();
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
      cents.add(lastSummary.cents[i]);
      starts.add(static_cast<int64_t>(lastSummary.dayStart[i]));
    }
    last["month"] = lastSummary.monthCents;
    last["monthStart"] = static_cast<int64_t>(lastSummary.monthStart);
    last["fetchedAt"] = static_cast<int64_t>(lastSummary.fetchedAt);
    JsonArray nonUsd = last["nonUsd"].to<JsonArray>();
    for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) nonUsd.add(lastSummary.nonUsd[i]);
    last["nonUsdMonth"] = lastSummary.nonUsdCharges;
  }

  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(path, out)) {
    LOG_ERR("INCOME", "Failed to write %s", path);
    return false;
  }
  return true;
}
