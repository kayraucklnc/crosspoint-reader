#include "XkcdActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "XkcdCache.h"
#include "components/UITheme.h"

using Button = MappedInputManager::Button;

namespace {
// Landscape only when it makes the comic clearly bigger.
constexpr float LANDSCAPE_GAIN = 1.15f;
}  // namespace

void XkcdActivity::onEnter() {
  Activity::onEnter();
  savedOrientation = renderer.getOrientation();
  drawnOrientation = savedOrientation;
  if (!Storage.exists(xkcd_cache::DIR)) Storage.mkdir(xkcd_cache::DIR);
  const unsigned long started = millis();
  pack.open();
  loadState();
  const int start = lastViewed > 0 && show(lastViewed) ? lastViewed : newestAvailable();
  if (!haveComic && start > 0) show(start);
  LOG_INF("XKCD", "Opened at #%u in %lu ms (heap %u free, %u max block)", comic.num, millis() - started,
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // Nothing on the card yet: fetch the latest comics rather than show an empty screen.
  if (!haveComic) startCheck();
  requestUpdate();
}

void XkcdActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(savedOrientation);
  saveState();
  // Like every Wi-Fi activity: reboot to reclaim the heap TLS fragmented.
  if (wifiUsed) {
    stopWifi();
    silentRestart();
  }
}

// --- browsing --------------------------------------------------------------------

bool XkcdActivity::show(const int num) {
  if (num <= 0) return false;
  const int32_t index = pack.find(static_cast<uint16_t>(num));
  const bool ok = index >= 0 ? loadFromPack(index) : loadCached(num);
  if (ok) {
    lastViewed = comic.num;
    checkResult = -1;
    netError = NetError::None;
  }
  return ok;
}

bool XkcdActivity::loadFromPack(const int32_t index) {
  const unsigned long started = millis();
  Comic next;
  if (!pack.entryAt(static_cast<uint32_t>(index), next.packEntry) ||
      !pack.readText(next.packEntry, next.title, next.alt)) {
    LOG_ERR("XKCD", "Pack record %d unreadable", static_cast<int>(index));
    return false;
  }
  const XkcdPack::Entry& entry = next.packEntry;
  next.num = entry.num;
  next.year = entry.year;
  next.month = entry.month;
  next.day = entry.day;
  next.fromPack = true;
  RenderLock lock(*this);
  comic = std::move(next);
  haveComic = true;
  showAlt = false;
  landscape = comic.packEntry.landscape;
  LOG_DBG("XKCD", "Pack #%u (%ux%u%s) looked up in %lu ms", comic.num, entry.width, entry.height,
          landscape ? ", landscape" : "", millis() - started);
  return true;
}

bool XkcdActivity::loadCached(const int num) {
  if (!std::binary_search(cached.begin(), cached.end(), static_cast<uint16_t>(num))) return false;
  const std::string path = xkcd_cache::metaPath(num);
  xkcd_cache::Meta meta;
  if (!xkcd_cache::parseMeta(Storage.readFile(path.c_str()).c_str(), meta)) return false;
  Comic next;
  next.num = meta.num;
  next.year = meta.year;
  next.month = meta.month;
  next.day = meta.day;
  next.title = std::move(meta.title);
  next.alt = std::move(meta.alt);
  next.imagePath = std::move(meta.imagePath);
  if (!next.imagePath.empty() && !xkcd_cache::imageSize(next.imagePath, next.width, next.height)) {
    LOG_ERR("XKCD", "Cached image unreadable: %s", next.imagePath.c_str());
    next.imagePath.clear();
  }
  RenderLock lock(*this);
  comic = std::move(next);
  haveComic = true;
  showAlt = false;
  chooseOrientation();
  return true;
}

// Nearest available comic after (direction > 0) or before `num`, 0 if none.
int XkcdActivity::neighbour(const int num, const int direction) const {
  int best = 0;
  if (direction > 0) {
    XkcdPack::Entry entry;
    const uint32_t index = pack.lowerBound(static_cast<uint16_t>(num + 1));
    if (pack.entryAt(index, entry)) best = entry.num;
    const auto it = std::upper_bound(cached.begin(), cached.end(), static_cast<uint16_t>(num));
    if (it != cached.end() && (best == 0 || *it < best)) best = *it;
  } else {
    XkcdPack::Entry entry;
    const uint32_t index = pack.lowerBound(static_cast<uint16_t>(num));
    if (index > 0 && pack.entryAt(index - 1, entry)) best = entry.num;
    const auto it = std::lower_bound(cached.begin(), cached.end(), static_cast<uint16_t>(num));
    if (it != cached.begin() && *(it - 1) > best) best = *(it - 1);
  }
  return best;
}

int XkcdActivity::newestAvailable() const {
  const int cachedTop = cached.empty() ? 0 : cached.back();
  return std::max<int>(pack.maxNum(), cachedTop);
}

bool XkcdActivity::atNewest() const { return haveComic && comic.num >= newestAvailable(); }

// Uniform over every comic on the card: the pack, then cached ones beyond it.
void XkcdActivity::showRandom() {
  const auto newer = std::upper_bound(cached.begin(), cached.end(), pack.maxNum());
  const uint32_t extra = static_cast<uint32_t>(cached.end() - newer);
  const uint32_t total = pack.count() + extra;
  if (total == 0) return;
  const uint32_t pick = static_cast<uint32_t>(random(static_cast<long>(total)));
  const bool ok =
      pick < pack.count() ? loadFromPack(static_cast<int32_t>(pick)) : loadCached(*(newer + (pick - pack.count())));
  if (ok) {
    lastViewed = comic.num;
    checkResult = -1;
    netError = NetError::None;
  }
}

// Picks the orientation that shows an online comic larger; the header strip
// and the button hints take the same room either way.
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

// --- input -----------------------------------------------------------------------

void XkcdActivity::loop() {
  if (waitingForWifi) return;
  if (checking) {
    runCheck();
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
    if (mappedInput.wasReleased(Button::Confirm)) startCheck();
    return;
  }
  bool moved = false;
  if (mappedInput.wasReleased(Button::Confirm)) {
    showAlt = !showAlt;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Left)) {
    const int prev = neighbour(comic.num, -1);
    moved = prev > 0 && show(prev);
  } else if (mappedInput.wasReleased(Button::Right)) {
    // At the newest comic there is no "next", so Right rolls a random one instead.
    const int next = neighbour(comic.num, +1);
    if (next > 0) {
      moved = show(next);
    } else {
      showRandom();
      moved = true;
    }
  } else if (mappedInput.wasReleased(Button::Up)) {
    showRandom();
    moved = true;
  } else if (mappedInput.wasReleased(Button::Down)) {
    if (atNewest()) {
      startCheck();
    } else {
      moved = show(newestAvailable());
    }
  }
  if (moved) requestUpdate();
}
