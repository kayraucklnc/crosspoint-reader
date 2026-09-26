#include "WebAppApi.h"

#include <ArduinoJson.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"
#include "IncomeCache.h"
#include "StripeClient.h"
#include "TrainsCache.h"
#include "WifiCredentialStore.h"
#include "util/Timezones.h"
#include "util/WallpaperStore.h"

namespace {
constexpr size_t MIN_KEY_LEN = 20;
constexpr size_t MAX_KEY_LEN = 200;

void sendJson(WebServer& server, const JsonDocument& doc, const int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendError(WebServer& server, const int code, const char* message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(server, doc, code);
}

bool readBody(WebServer& server, JsonDocument& doc) {
  if (!server.hasArg("plain")) {
    sendError(server, 400, "Missing JSON body");
    return false;
  }
  const String body = server.arg("plain");
  const DeserializationError err = deserializeJson(doc, body.c_str());
  if (err) {
    LOG_DBG("WEBAPP", "Bad JSON (%s): %u bytes", err.c_str(), static_cast<unsigned>(body.length()));
    sendError(server, 400, "Invalid JSON body");
    return false;
  }
  return true;
}

// Restricted (rk_) or secret (sk_) keys, test or live; only [A-Za-z0-9_].
bool validStripeKey(const std::string& key) {
  if (key.size() < MIN_KEY_LEN || key.size() > MAX_KEY_LEN) return false;
  if (key.rfind("rk_", 0) != 0 && key.rfind("sk_", 0) != 0) return false;
  return std::all_of(key.begin(), key.end(), [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}

// "rk_live_…WXYZ": mode prefix and last four characters only.
std::string keyHint(const std::string& key) {
  const size_t prefixEnd = key.find('_', 3);
  const std::string prefix = prefixEnd == std::string::npos ? key.substr(0, 3) : key.substr(0, prefixEnd + 1);
  return prefix + "\xE2\x80\xA6" + key.substr(key.size() - 4);
}

void handleIncomeGet(WebServer& server) {
  std::string key;
  JsonDocument doc;
  const bool hasKey = StripeClient::loadKey(key);
  doc["hasKey"] = hasKey;
  if (hasKey) doc["keyHint"] = keyHint(key);
  sendJson(server, doc);
}

void handleIncomeKey(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  const std::string key = body["key"] | "";
  if (!validStripeKey(key)) {
    sendError(server, 400, "Expected an rk_ or sk_ key (letters, digits, underscores)");
    return;
  }
  JsonDocument file;
  file["key"] = key;
  String out;
  serializeJson(file, out);
  if (!Storage.writeFile(StripeClient::KEY_PATH, out)) {
    sendError(server, 500, "Could not write the key file");
    return;
  }
  Storage.remove(IncomeCache::PATH);  // numbers from another account must not linger
  LOG_INF("WEBAPP", "Stripe key replaced");
  JsonDocument doc;
  doc["hasKey"] = true;
  doc["keyHint"] = keyHint(key);
  sendJson(server, doc);
}

void handleIncomeKeyDelete(WebServer& server) {
  Storage.remove(StripeClient::KEY_PATH);
  Storage.remove(IncomeCache::PATH);
  JsonDocument doc;
  doc["hasKey"] = false;
  sendJson(server, doc);
}

// One minimal list call per permission the income screen needs.
void handleIncomeTest(WebServer& server) {
  std::string key;
  if (!StripeClient::loadKey(key)) {
    sendError(server, 400, "No key saved");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    sendError(server, 503, "Device is not on a network with internet (hotspot mode)");
    return;
  }
  const std::string auth = "Bearer " + key;
  const auto probe = [&auth](const char* url) {
    int status = 0;
    HttpDownloader::fetchUrlWithAuthorization(url, [](const uint8_t*, size_t) { return true; }, auth, &status);
    return status;
  };
  JsonDocument doc;
  doc["charges"] = probe("https://api.stripe.com/v1/charges?limit=1");
  doc["balance"] = probe("https://api.stripe.com/v1/balance_transactions?limit=1");
  sendJson(server, doc);
}

void handleTrainsGet(WebServer& server) {
  TrainsConfig config;
  config.load();
  JsonDocument doc;
  JsonArray stations = doc["stations"].to<JsonArray>();
  for (const auto& station : trenord::STATIONS) {
    JsonObject s = stations.add<JsonObject>();
    s["name"] = station.apiName;
    s["label"] = station.label;
  }
  doc["origin"] = config.origin;
  doc["towards"] = config.towards;
  doc["home"] = config.home;
  JsonArray favorites = doc["favorites"].to<JsonArray>();
  for (const int8_t f : config.favorites) {
    if (f >= 0) favorites.add(f);
  }
  sendJson(server, doc);
}

void handleTrainsPost(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  TrainsConfig config;
  config.load();
  const auto station = [](const JsonVariantConst v, const int8_t fallback) {
    const int s = v | -1;
    return static_cast<int8_t>(s >= 0 && s < trenord::STATION_COUNT ? s : fallback);
  };
  config.origin = station(body["origin"], config.origin);
  config.towards = station(body["towards"], config.towards);
  config.home = station(body["home"], config.home);
  if (config.origin == config.towards) {
    sendError(server, 400, "Origin and direction must differ");
    return;
  }
  if (body["favorites"].is<JsonArrayConst>()) {
    std::fill(std::begin(config.favorites), std::end(config.favorites), static_cast<int8_t>(-1));
    int i = 0;
    for (const JsonVariantConst v : body["favorites"].as<JsonArrayConst>()) {
      if (i < TrainsConfig::MAX_FAVORITES) config.favorites[i++] = station(v, -1);
    }
  }
  config.sanitize();
  if (!config.save()) {
    sendError(server, 500, "Could not save");
    return;
  }
  handleTrainsGet(server);
}

void handleWallpaperGet(WebServer& server) {
  const wallpaper::State state = wallpaper::loadState();
  JsonDocument doc;
  doc["mode"] = SETTINGS.wallpaperMode;
  doc["fixed"] = state.fixedName;
  JsonArray files = doc["files"].to<JsonArray>();
  for (const std::string& name : wallpaper::list()) files.add(name);
  sendJson(server, doc);
}

void handleWallpaperPost(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  if (body["fixed"].is<const char*>()) {
    const std::string fixed = body["fixed"].as<const char*>();
    const auto names = wallpaper::list();
    if (std::find(names.begin(), names.end(), fixed) == names.end()) {
      sendError(server, 400, "No such wallpaper in /sleep");
      return;
    }
    wallpaper::State state = wallpaper::loadState();
    state.fixedName = fixed;
    wallpaper::saveState(state);
  }
  if (body["mode"].is<int>()) {
    const int mode = body["mode"];
    if (mode < CrossPointSettings::WALLPAPER_RANDOM || mode > CrossPointSettings::WALLPAPER_FIXED) {
      sendError(server, 400, "mode must be 0 (Random), 1 (Cycle) or 2 (Fixed)");
      return;
    }
    SETTINGS.wallpaperMode = static_cast<uint8_t>(mode);
    SETTINGS.saveToFile();
  }
  handleWallpaperGet(server);
}

void handleWifiOrder(WebServer& server) {
  JsonDocument body;
  if (!readBody(server, body)) return;
  if (!body["ssids"].is<JsonArrayConst>()) {
    sendError(server, 400, "Expected {ssids: [...]}");
    return;
  }
  std::vector<std::string> ssids;
  ssids.reserve(body["ssids"].size());
  for (const JsonVariantConst v : body["ssids"].as<JsonArrayConst>()) ssids.emplace_back(v | "");
  WIFI_STORE.setOrder(ssids);
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(server, doc);
}

void handleTimezones(WebServer& server) {
  JsonDocument doc;
  JsonArray list = doc.to<JsonArray>();
  const TimezoneInfo* table = timezones::table();
  char offset[16];
  for (size_t i = 0; i < timezones::count(); i++) {
    timezones::formatOffset(static_cast<uint8_t>(i), offset, sizeof(offset));
    JsonObject tz = list.add<JsonObject>();
    tz["name"] = table[i].name;
    tz["offset"] = offset;
  }
  sendJson(server, doc);
}

void handleDevice(WebServer& server) {
  JsonDocument doc;
  doc["battery"] = powerManager.getBatteryPercentage();
  doc["timezone"] = timezones::activeIndex();
  sendJson(server, doc);
}
}  // namespace

void registerWebAppApi(WebServer& server) {
  WebServer* s = &server;
  server.on("/api/income", HTTP_GET, [s] { handleIncomeGet(*s); });
  server.on("/api/income/key", HTTP_POST, [s] { handleIncomeKey(*s); });
  server.on("/api/income/key/delete", HTTP_POST, [s] { handleIncomeKeyDelete(*s); });
  server.on("/api/income/test", HTTP_POST, [s] { handleIncomeTest(*s); });
  server.on("/api/trains", HTTP_GET, [s] { handleTrainsGet(*s); });
  server.on("/api/trains", HTTP_POST, [s] { handleTrainsPost(*s); });
  server.on("/api/wallpaper", HTTP_GET, [s] { handleWallpaperGet(*s); });
  server.on("/api/wallpaper", HTTP_POST, [s] { handleWallpaperPost(*s); });
  server.on("/api/wifi/order", HTTP_POST, [s] { handleWifiOrder(*s); });
  server.on("/api/timezones", HTTP_GET, [s] { handleTimezones(*s); });
  server.on("/api/device", HTTP_GET, [s] { handleDevice(*s); });
}
