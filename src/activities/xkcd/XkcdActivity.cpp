#include "XkcdActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "network/WifiAutoConnect.h"

using Button = MappedInputManager::Button;

namespace {
constexpr char CACHE_DIR[] = "/.crosspoint/xkcd";
constexpr char STATE_PATH[] = "/.crosspoint/xkcd/state.json";
constexpr size_t MAX_CACHED = 30;
// xkcd.com has never published #404.
constexpr int MISSING_COMIC = 404;
// Landscape only when it makes the comic clearly bigger.
constexpr float LANDSCAPE_GAIN = 1.15f;

std::string metaPath(const int num) {
  char path[48];
  snprintf(path, sizeof(path), "%s/%d.json", CACHE_DIR, num);
  return path;
}

// ".png" / ".jpg" from an image URL; empty for formats the decoders lack (gif).
const char* imageExtension(const char* url) {
  const char* dot = strrchr(url, '.');
  if (!dot) return "";
  if (strcasecmp(dot, ".png") == 0) return ".png";
  if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return ".jpg";
  return "";
}

std::string imagePath(const int num, const char* extension) {
  char path[48];
  snprintf(path, sizeof(path), "%s/%d%s", CACHE_DIR, num, extension);
  return path;
}

bool imageSize(const std::string& path, int16_t& width, int16_t& height) {
  ImageDimensions dims{};
  const bool ok = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0
                      ? PngToFramebufferConverter::getDimensionsStatic(path, dims)
                      : JpegToFramebufferConverter::getDimensionsStatic(path, dims);
  if (!ok || dims.width <= 0 || dims.height <= 0) return false;
  width = dims.width;
  height = dims.height;
  return true;
}
}  // namespace

void XkcdActivity::onEnter() {
  Activity::onEnter();
  savedOrientation = renderer.getOrientation();
  drawnOrientation = savedOrientation;
  if (!Storage.exists(CACHE_DIR)) Storage.mkdir(CACHE_DIR);
  loadState();
  request(0, 0);
}

void XkcdActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(savedOrientation);
  saveState();
  // Like every Wi-Fi activity: reboot to reclaim the heap TLS fragmented.
  if (wifiUsed) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

// --- loading ---------------------------------------------------------------------

void XkcdActivity::request(const int num, const int step) {
  target = num;
  direction = step;
  loading = true;
  requestUpdate();
}

bool XkcdActivity::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  {
    // Credentials load lazily; SD shares the SPI bus with the display.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  wifiUsed = true;
  return wifi_auto_connect::connect() == wifi_auto_connect::Result::CONNECTED;
}

// Draws the status popup, then blocks on the card or the network.
void XkcdActivity::runLoad() {
  requestUpdateAndWait();
  int num = target;
  if (num == MISSING_COMIC) num += direction < 0 ? -1 : 1;

  bool ok = num > 0 && loadCached(num);
  if (!ok) {
    if (ensureWifi()) {
      ok = fetch(num);
    } else if (!haveComic && recent.empty()) {
      // Nothing to show offline: let the user pick a network.
      waitingForWifi = true;
      auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
      if (picker) {
        startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
          waitingForWifi = false;
          if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
            loading = false;
            failed = true;
          }
        });
        return;
      }
      waitingForWifi = false;
      LOG_ERR("XKCD", "OOM: Wi-Fi picker");
    }
    if (!ok && !haveComic && !recent.empty()) {
      // Offline: fall back to the last comic on the card.
      ok = loadCached(recent.front());
      offline = ok;
    } else if (!ok && haveComic) {
      offline = true;
    }
  }
  failed = !ok && !haveComic;
  loading = false;
  requestUpdate();
}

bool XkcdActivity::readMeta(const std::string& json, Comic& out) const {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  out.num = doc["num"] | 0;
  out.title = doc["safe_title"] | "";
  out.alt = doc["alt"] | "";
  out.year = static_cast<uint16_t>(atoi(doc["year"] | "0"));
  out.month = static_cast<uint8_t>(atoi(doc["month"] | "0"));
  out.day = static_cast<uint8_t>(atoi(doc["day"] | "0"));
  const char* extension = imageExtension(doc["img"] | "");
  out.imagePath = extension[0] ? imagePath(out.num, extension) : std::string();
  return out.num > 0;
}

bool XkcdActivity::loadCached(const int num) {
  const std::string path = metaPath(num);
  if (!Storage.exists(path.c_str())) return false;
  Comic next;
  if (!readMeta(Storage.readFile(path.c_str()).c_str(), next)) return false;
  if (!next.imagePath.empty() &&
      (!Storage.exists(next.imagePath.c_str()) || !imageSize(next.imagePath, next.width, next.height))) {
    return false;
  }
  {
    RenderLock lock(*this);
    comic = std::move(next);
    haveComic = true;
    showAlt = false;
    offline = false;
    chooseOrientation();
  }
  remember(comic.num);
  return true;
}

bool XkcdActivity::fetch(const int num) {
  char url[48];
  if (num > 0) {
    snprintf(url, sizeof(url), "https://xkcd.com/%d/info.0.json", num);
  } else {
    snprintf(url, sizeof(url), "https://xkcd.com/info.0.json");
  }
  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("XKCD", "Fetch failed: %s", url);
    return false;
  }

  // Keep only what the reader shows; old comics carry kilobytes of transcript.
  JsonDocument doc;
  {
    JsonDocument filter;
    for (const char* key : {"num", "safe_title", "alt", "img", "year", "month", "day"}) filter[key] = true;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      LOG_ERR("XKCD", "Bad JSON for %s", url);
      return false;
    }
  }
  std::string meta;
  serializeJson(doc, meta);
  std::string().swap(body);

  Comic next;
  if (!readMeta(meta, next)) return false;
  if (num == 0) latest = next.num;
  if (next.num > latest) latest = next.num;

  if (!next.imagePath.empty()) {
    const char* img = doc["img"] | "";
    if (HttpDownloader::downloadToFile(img, next.imagePath) != HttpDownloader::OK ||
        !imageSize(next.imagePath, next.width, next.height)) {
      LOG_ERR("XKCD", "Image failed: %s", img);
      Storage.remove(next.imagePath.c_str());
      return false;
    }
  }
  Storage.writeFile(metaPath(next.num).c_str(), meta.c_str());

  {
    RenderLock lock(*this);
    comic = std::move(next);
    haveComic = true;
    showAlt = false;
    offline = false;
    chooseOrientation();
  }
  remember(comic.num);
  return true;
}

// Picks the orientation that shows the comic larger; the header strip and the
// button hints take the same room either way.
void XkcdActivity::chooseOrientation() {
  if (comic.width <= 0 || comic.height <= 0) {
    landscape = false;
    return;
  }
  const int hints = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int shortSide = std::min(renderer.getScreenWidth(), renderer.getScreenHeight());
  const int longSide = std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  const float portrait = std::min(static_cast<float>(shortSide - 16) / comic.width,
                                  static_cast<float>(longSide - HEADER_HEIGHT - hints - 8) / comic.height);
  const float wide = std::min(static_cast<float>(longSide - hints - 16) / comic.width,
                              static_cast<float>(shortSide - HEADER_HEIGHT - 8) / comic.height);
  landscape = std::min(wide, MAX_UPSCALE) > std::min(portrait, MAX_UPSCALE) * LANDSCAPE_GAIN;
}

// Most recently viewed first; the oldest beyond MAX_CACHED leave the card.
void XkcdActivity::remember(const uint16_t num) {
  recent.erase(std::remove(recent.begin(), recent.end(), num), recent.end());
  recent.insert(recent.begin(), num);
  while (recent.size() > MAX_CACHED) {
    const int old = recent.back();
    recent.pop_back();
    Comic stale;
    const std::string meta = metaPath(old);
    if (readMeta(Storage.readFile(meta.c_str()).c_str(), stale) && !stale.imagePath.empty()) {
      Storage.remove(stale.imagePath.c_str());
    }
    Storage.remove(meta.c_str());
  }
  saveState();
}

void XkcdActivity::loadState() {
  recent.clear();
  recent.reserve(MAX_CACHED + 1);
  if (!Storage.exists(STATE_PATH)) return;
  JsonDocument doc;
  if (deserializeJson(doc, Storage.readFile(STATE_PATH).c_str())) return;
  latest = doc["latest"] | 0;
  for (const JsonVariantConst num : doc["recent"].as<JsonArrayConst>()) {
    if (recent.size() >= MAX_CACHED) break;
    const int value = num | 0;
    if (value > 0) recent.push_back(static_cast<uint16_t>(value));
  }
}

void XkcdActivity::saveState() const {
  JsonDocument doc;
  doc["latest"] = latest;
  JsonArray list = doc["recent"].to<JsonArray>();
  for (const uint16_t num : recent) list.add(num);
  std::string out;
  serializeJson(doc, out);
  Storage.writeFile(STATE_PATH, out.c_str());
}

// --- input -----------------------------------------------------------------------

void XkcdActivity::requestRandom() {
  const int top = std::max<int>(latest, comic.num);
  if (top > 0) request(1 + static_cast<int>(random(top)), +1);
}

void XkcdActivity::loop() {
  if (waitingForWifi) return;
  if (loading) {
    runLoad();
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    if (showAlt) {
      showAlt = false;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (!haveComic) {
    if (mappedInput.wasReleased(Button::Confirm)) request(0, 0);
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    showAlt = !showAlt;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Left)) {
    if (comic.num > 1) request(comic.num - 1, -1);
  } else if (mappedInput.wasReleased(Button::Right)) {
    // At the newest comic there is no "next", so Right rolls a random one instead.
    if (latest == 0 || comic.num < latest) {
      request(comic.num + 1, +1);
    } else {
      requestRandom();
    }
  } else if (mappedInput.wasReleased(Button::Up)) {
    requestRandom();
  } else if (mappedInput.wasReleased(Button::Down)) {
    request(0, 0);
  }
}
