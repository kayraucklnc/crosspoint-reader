#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "WifiCredentialStore.h"
#include "XkcdActivity.h"
#include "XkcdCache.h"
#include "activities/network/WifiSelectionActivity.h"
#include "network/HttpDownloader.h"
#include "network/WifiAutoConnect.h"

namespace {
constexpr size_t MAX_CACHED = 30;
// One check fetches at most this many comics (two TLS requests each); a
// later check continues with the older ones still missing.
constexpr int MAX_NEW_PER_CHECK = 5;
// xkcd.com has never published #404.
constexpr int MISSING_COMIC = 404;
// Old comics carry kilobytes of transcript; anything larger is not info.0.json.
constexpr size_t MAX_JSON_BYTES = 16 * 1024;

void logHeap(const char* when) {
  LOG_INF("XKCD", "Heap %s: %u free, %u max block", when, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

void removeCachedFiles(const int num) {
  const std::string meta = xkcd_cache::metaPath(num);
  xkcd_cache::Meta info;
  if (xkcd_cache::parseMeta(Storage.readFile(meta.c_str()).c_str(), info) && !info.imagePath.empty()) {
    Storage.remove(info.imagePath.c_str());
  }
  Storage.remove(meta.c_str());
}
}  // namespace

// --- check for new comics ----------------------------------------------------------

void XkcdActivity::startCheck() {
  checking = true;
  fetchingNum = 0;
  netError = NetError::None;
  netStatus = 0;
  checkResult = -1;
  requestUpdate();
}

// Draws the status popup, then blocks on the network. User-initiated only.
void XkcdActivity::runCheck() {
  requestUpdateAndWait();
  const unsigned long started = millis();
  const bool hadComic = haveComic;
  const bool wasNewest = atNewest();
  int added = -1;
  if (ensureWifi()) {
    logHeap("with Wi-Fi up");
    added = downloadNew();
  } else if (!hadComic) {
    // Nothing to show offline: let the user pick a network, then check again.
    auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
    if (picker) {
      waitingForWifi = true;
      startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
        waitingForWifi = false;
        if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
          checking = false;
          netError = NetError::NoWifi;
        }
        requestUpdate();
      });
      return;
    }
    LOG_ERR("XKCD", "OOM: Wi-Fi picker");
    netError = NetError::NoWifi;
  } else {
    netError = NetError::NoWifi;
  }
  // The radio's buffers go back to the heap before any PNG is decoded.
  stopWifi();
  logHeap("after Wi-Fi off");

  const NetError error = netError;
  const int status = netStatus;
  if (added > 0 && (!hadComic || wasNewest)) show(newestAvailable());
  if (!haveComic && added < 0 && newestAvailable() > 0) show(newestAvailable());
  netError = error;
  netStatus = status;
  checkResult = error == NetError::None ? added : -1;
  LOG_INF("XKCD", "Check: %d new, error %d (HTTP %d), latest #%d, %lu ms", added, static_cast<int>(error), status,
          latest, millis() - started);
  checking = false;
  fetchingNum = 0;
  saveState();
  requestUpdate();
}

// Fetches comics newer than the pack that the cache lacks, newest first.
// Returns how many were added, or -1 when the first request failed.
int XkcdActivity::downloadNew() {
  std::string meta;
  if (!fetchJson(0, meta)) return -1;
  xkcd_cache::Meta newest;
  if (!xkcd_cache::parseMeta(meta.c_str(), newest)) {
    LOG_ERR("XKCD", "Unparsable info.0.json");
    netError = NetError::BadData;
    return -1;
  }
  latest = newest.num;
  int added = 0;
  for (int num = latest; num > pack.maxNum() && added < MAX_NEW_PER_CHECK; num--) {
    if (num == MISSING_COMIC || std::binary_search(cached.begin(), cached.end(), static_cast<uint16_t>(num))) {
      continue;
    }
    fetchingNum = num;
    requestUpdateAndWait();
    std::string older;
    if (num != latest && !fetchJson(num, older)) break;
    if (!fetchComic(num, num == latest ? meta : older)) break;
    added++;
  }
  return added;
}

bool XkcdActivity::tlsHeapOk() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap >= HttpDownloader::MIN_TLS_FREE_HEAP && maxBlock >= HttpDownloader::MIN_TLS_MAX_ALLOC) return true;
  LOG_ERR("XKCD", "Low memory for TLS: %u free, %u max block", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(maxBlock));
  netError = NetError::LowMemory;
  return false;
}

// info.0.json of comic num (0 = latest), trimmed to the fields the reader keeps.
bool XkcdActivity::fetchJson(const int num, std::string& meta) {
  if (!tlsHeapOk()) return false;
  char url[48];
  if (num > 0) {
    snprintf(url, sizeof(url), "https://xkcd.com/%d/info.0.json", num);
  } else {
    snprintf(url, sizeof(url), "https://xkcd.com/info.0.json");
  }
  std::string body;
  bool tooBig = false;
  int status = 0;
  const unsigned long started = millis();
  const bool ok = HttpDownloader::fetchUrlWithHeaders(
      url,
      [&body, &tooBig](const uint8_t* data, const size_t len) {
        if (body.size() + len > MAX_JSON_BYTES) {
          tooBig = true;
          return false;
        }
        body.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      nullptr, 0, &status);
  if (!ok) {
    netStatus = status;
    netError = tooBig ? NetError::BadData : status > 0 && status != 200 ? NetError::Server : NetError::Network;
    LOG_ERR("XKCD", "GET %s failed: HTTP %d%s", url, status, tooBig ? ", body too large" : "");
    return false;
  }
  JsonDocument doc;
  JsonDocument filter;
  for (const char* key : {"num", "safe_title", "alt", "img", "year", "month", "day"}) filter[key] = true;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    LOG_ERR("XKCD", "Bad JSON from %s", url);
    netError = NetError::BadData;
    return false;
  }
  meta.clear();
  serializeJson(doc, meta);
  LOG_DBG("XKCD", "%s: %u bytes in %lu ms", url, static_cast<unsigned>(body.size()), millis() - started);
  return true;
}

bool XkcdActivity::fetchComic(const int num, const std::string& meta) {
  xkcd_cache::Meta info;
  if (!xkcd_cache::parseMeta(meta.c_str(), info) || info.num != num) {
    netError = NetError::BadData;
    return false;
  }
  if (!info.imagePath.empty()) {
    if (!downloadImage(info.imageUrl.c_str(), info.imagePath)) return false;
    int16_t width = 0;
    int16_t height = 0;
    if (!xkcd_cache::imageSize(info.imagePath, width, height)) {
      LOG_ERR("XKCD", "#%d: image header unreadable", num);
      Storage.remove(info.imagePath.c_str());
      netError = NetError::BadData;
      return false;
    }
  }
  if (!Storage.writeFile(xkcd_cache::metaPath(num).c_str(), meta.c_str())) {
    LOG_ERR("XKCD", "#%d: meta write failed", num);
    if (!info.imagePath.empty()) Storage.remove(info.imagePath.c_str());
    netError = NetError::SdCard;
    return false;
  }
  addCached(static_cast<uint16_t>(num));
  return true;
}

// Streams the image to the card; nothing larger than a TLS record is buffered.
bool XkcdActivity::downloadImage(const char* url, const std::string& path) {
  if (!tlsHeapOk()) return false;
  const std::string part = path + ".part";
  if (Storage.exists(part.c_str())) Storage.remove(part.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("XKCD", part, file)) {
    netError = NetError::SdCard;
    return false;
  }
  bool writeFailed = false;
  size_t bytes = 0;
  int status = 0;
  const unsigned long started = millis();
  const bool ok = HttpDownloader::fetchUrlWithHeaders(
      url,
      [&file, &writeFailed, &bytes](const uint8_t* data, const size_t len) {
        if (file.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        bytes += len;
        return true;
      },
      nullptr, 0, &status);
  // Close before remove()/rename() on the same path.
  file.close();
  if (!ok || bytes == 0) {
    Storage.remove(part.c_str());
    netStatus = status;
    netError = writeFailed ? NetError::SdCard : status > 0 && status != 200 ? NetError::Server : NetError::Network;
    LOG_ERR("XKCD", "GET %s failed: HTTP %d after %u bytes", url, status, static_cast<unsigned>(bytes));
    return false;
  }
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  if (!Storage.rename(part.c_str(), path.c_str())) {
    Storage.remove(part.c_str());
    netError = NetError::SdCard;
    return false;
  }
  LOG_INF("XKCD", "%s: %u bytes in %lu ms", url, static_cast<unsigned>(bytes), millis() - started);
  return true;
}

bool XkcdActivity::ensureWifi() {
  wifiUsed = true;
  if (WiFi.status() == WL_CONNECTED) return true;
  {
    // Credentials load lazily; SD shares the SPI bus with the display.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  const auto result = wifi_auto_connect::connect();
  if (result != wifi_auto_connect::Result::CONNECTED) {
    LOG_ERR("XKCD", "Wi-Fi join failed (%s)",
            result == wifi_auto_connect::Result::NO_SAVED_NETWORKS ? "no saved networks" : "no network answered");
    return false;
  }
  return true;
}

void XkcdActivity::stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

// --- cache bookkeeping -------------------------------------------------------------

// Keeps `cached` sorted; past MAX_CACHED, comics the pack also has go first,
// then the oldest (never the one on screen).
void XkcdActivity::addCached(const uint16_t num) {
  const auto at = std::lower_bound(cached.begin(), cached.end(), num);
  if (at == cached.end() || *at != num) cached.insert(at, num);
  while (cached.size() > MAX_CACHED) {
    auto victim = std::find_if(cached.begin(), cached.end(),
                               [this](const uint16_t n) { return n <= pack.maxNum() && n != comic.num; });
    if (victim == cached.end()) victim = cached.front() != comic.num ? cached.begin() : cached.begin() + 1;
    removeCachedFiles(*victim);
    cached.erase(victim);
  }
}

void XkcdActivity::loadState() {
  cached.clear();
  cached.reserve(MAX_CACHED + 1);
  if (!Storage.exists(xkcd_cache::STATE_PATH)) return;
  JsonDocument doc;
  if (deserializeJson(doc, Storage.readFile(xkcd_cache::STATE_PATH).c_str())) return;
  latest = doc["latest"] | 0;
  lastViewed = doc["last"] | 0;
  // "recent" is the pre-pack name of the cache list.
  JsonArrayConst list = doc["cached"].is<JsonArrayConst>() ? doc["cached"].as<JsonArrayConst>() : doc["recent"];
  for (const JsonVariantConst item : list) {
    const int num = item | 0;
    if (num <= 0 || num > UINT16_MAX || cached.size() >= MAX_CACHED) continue;
    if (num <= pack.maxNum()) {
      removeCachedFiles(num);  // the pack has it now
    } else if (Storage.exists(xkcd_cache::metaPath(num).c_str())) {
      cached.push_back(static_cast<uint16_t>(num));
    }
  }
  std::sort(cached.begin(), cached.end());
  cached.erase(std::unique(cached.begin(), cached.end()), cached.end());
  if (lastViewed == 0) lastViewed = doc["recent"][0] | 0;
}

void XkcdActivity::saveState() const {
  JsonDocument doc;
  doc["latest"] = latest;
  doc["last"] = lastViewed;
  JsonArray list = doc["cached"].to<JsonArray>();
  for (const uint16_t num : cached) list.add(num);
  std::string out;
  serializeJson(doc, out);
  Storage.writeFile(xkcd_cache::STATE_PATH, out.c_str());
}
