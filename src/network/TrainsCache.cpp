#include "TrainsCache.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace {
constexpr int CACHE_VERSION = 1;

void writeBoard(JsonObject obj, const trenord::Board& board) {
  obj["origin"] = board.origin;
  obj["towards"] = board.towards;
  obj["fetchedAt"] = static_cast<int64_t>(board.fetchedAt);
  JsonArray deps = obj["deps"].to<JsonArray>();
  for (int i = 0; i < board.count; i++) {
    const trenord::Departure& d = board.departures[i];
    JsonObject o = deps.add<JsonObject>();
    o["t"] = static_cast<int64_t>(d.scheduled);
    o["d"] = d.delayMin;
    o["x"] = d.cancelled;
    o["c"] = d.category;
    o["n"] = d.number;
    o["to"] = d.direction;
    o["p"] = d.platform;
  }
}

bool readBoard(JsonObjectConst obj, trenord::Board& board) {
  if (obj.isNull()) return false;
  board = trenord::Board{};
  const int origin = obj["origin"] | -1;
  const int towards = obj["towards"] | -1;
  if (origin < 0 || origin >= trenord::STATION_COUNT || towards < 0 || towards >= trenord::STATION_COUNT) {
    return false;
  }
  board.origin = static_cast<int8_t>(origin);
  board.towards = static_cast<int8_t>(towards);
  board.fetchedAt = obj["fetchedAt"] | static_cast<int64_t>(0);
  for (JsonObjectConst o : obj["deps"].as<JsonArrayConst>()) {
    if (board.count >= trenord::Board::MAX_DEPARTURES) break;
    trenord::Departure& d = board.departures[board.count++];
    d.scheduled = o["t"] | static_cast<int64_t>(0);
    d.delayMin = o["d"] | trenord::DELAY_UNKNOWN;
    d.cancelled = o["x"] | false;
    snprintf(d.category, sizeof(d.category), "%s", o["c"] | "");
    snprintf(d.number, sizeof(d.number), "%s", o["n"] | "");
    snprintf(d.direction, sizeof(d.direction), "%s", o["to"] | "");
    snprintf(d.platform, sizeof(d.platform), "%s", o["p"] | "");
  }
  return true;
}
}  // namespace

bool TrainsCache::load() {
  *this = TrainsCache{};
  if (!Storage.exists(PATH)) return false;
  const String raw = Storage.readFile(PATH);
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok || doc["v"] != CACHE_VERSION) {
    LOG_ERR("TRAIN", "Ignoring unreadable %s", PATH);
    return false;
  }
  hasOutbound = readBoard(doc["out"], outbound);
  hasInbound = readBoard(doc["in"], inbound);
  JsonObjectConst a = doc["alerts"];
  if (!a.isNull()) {
    alerts.fetchedAt = a["at"] | static_cast<int64_t>(0);
    alerts.lineCritical = a["critical"] | false;
    alerts.strike.start = a["strikeStart"] | static_cast<int64_t>(0);
    alerts.strike.end = a["strikeEnd"] | static_cast<int64_t>(0);
    snprintf(alerts.strike.text, sizeof(alerts.strike.text), "%s", a["strikeText"] | "");
    for (JsonObjectConst n : a["notices"].as<JsonArrayConst>()) {
      if (alerts.noticeCount >= trenord_alerts::Alerts::MAX_NOTICES) break;
      trenord_alerts::Notice& notice = alerts.notices[alerts.noticeCount++];
      notice.at = n["at"] | static_cast<int64_t>(0);
      notice.critical = true;
      snprintf(notice.text, sizeof(notice.text), "%s", n["text"] | "");
    }
  }
  int i = 0;
  for (const int station : doc["recent"].as<JsonArrayConst>()) {
    if (i >= MAX_RECENT) break;
    if (station >= 0 && station < trenord::STATION_COUNT) recent[i++] = static_cast<int8_t>(station);
  }
  return true;
}

bool TrainsCache::save() const {
  JsonDocument doc;
  doc["v"] = CACHE_VERSION;
  if (hasOutbound) writeBoard(doc["out"].to<JsonObject>(), outbound);
  if (hasInbound) writeBoard(doc["in"].to<JsonObject>(), inbound);
  JsonArray list = doc["recent"].to<JsonArray>();
  for (const int8_t station : recent) {
    if (station >= 0) list.add(station);
  }
  if (alerts.fetchedAt) {
    JsonObject a = doc["alerts"].to<JsonObject>();
    a["at"] = static_cast<int64_t>(alerts.fetchedAt);
    a["critical"] = alerts.lineCritical;
    a["strikeStart"] = static_cast<int64_t>(alerts.strike.start);
    a["strikeEnd"] = static_cast<int64_t>(alerts.strike.end);
    a["strikeText"] = alerts.strike.text;
    JsonArray notices = a["notices"].to<JsonArray>();
    for (int i = 0; i < alerts.noticeCount; i++) {
      JsonObject n = notices.add<JsonObject>();
      n["at"] = static_cast<int64_t>(alerts.notices[i].at);
      n["text"] = alerts.notices[i].text;
    }
  }
  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(PATH, out)) {
    LOG_ERR("TRAIN", "Failed to write %s", PATH);
    return false;
  }
  return true;
}

void TrainsCache::markRecent(const int station) {
  int8_t next[MAX_RECENT] = {static_cast<int8_t>(station), -1, -1, -1};
  int n = 1;
  for (const int8_t s : recent) {
    if (s >= 0 && s != station && n < MAX_RECENT) next[n++] = s;
  }
  for (int i = 0; i < MAX_RECENT; i++) recent[i] = next[i];
}

bool TrainsConfig::load() {
  *this = TrainsConfig{};
  if (!Storage.exists(PATH)) return false;
  const String raw = Storage.readFile(PATH);
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str()) != DeserializationError::Ok) {
    LOG_ERR("TRAIN", "Ignoring unreadable %s", PATH);
    return false;
  }
  origin = static_cast<int8_t>(doc["origin"] | static_cast<int>(trenord::HOME));
  towards = static_cast<int8_t>(doc["towards"] | static_cast<int>(trenord::DEFAULT_TOWARDS));
  home = static_cast<int8_t>(doc["home"] | static_cast<int>(trenord::HOME));
  int i = 0;
  for (const int station : doc["favorites"].as<JsonArrayConst>()) {
    if (i < MAX_FAVORITES) favorites[i++] = static_cast<int8_t>(station);
  }
  sanitize();
  return true;
}

bool TrainsConfig::save() const {
  JsonDocument doc;
  doc["origin"] = origin;
  doc["towards"] = towards;
  doc["home"] = home;
  JsonArray list = doc["favorites"].to<JsonArray>();
  for (const int8_t station : favorites) {
    if (station >= 0) list.add(station);
  }
  String out;
  serializeJson(doc, out);
  if (!Storage.writeFile(PATH, out)) {
    LOG_ERR("TRAIN", "Failed to write %s", PATH);
    return false;
  }
  return true;
}

void TrainsConfig::sanitize() {
  const auto valid = [](const int s) { return s >= 0 && s < trenord::STATION_COUNT; };
  if (!valid(origin)) origin = trenord::HOME;
  if (!valid(towards) || towards == origin) towards = origin == trenord::HOME ? trenord::DEFAULT_TOWARDS : trenord::HOME;
  if (!valid(home)) home = trenord::HOME;
  int8_t clean[MAX_FAVORITES] = {-1, -1, -1, -1, -1, -1};
  int n = 0;
  for (const int8_t s : favorites) {
    if (!valid(s) || s == home) continue;
    bool dup = false;
    for (int j = 0; j < n; j++) dup = dup || clean[j] == s;
    if (!dup) clean[n++] = s;
  }
  for (int j = 0; j < MAX_FAVORITES; j++) favorites[j] = clean[j];
}
