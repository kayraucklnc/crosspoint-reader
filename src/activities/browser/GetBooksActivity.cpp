#include "GetBooksActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <LegacyOpdsStream.h>
#include <WiFi.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GetBooksCatalog.h"
#include "MappedInputManager.h"
#include "OpdsBookBrowserActivity.h"
#include "OpdsServerStore.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "network/HttpDownloader.h"
#include "network/WifiAutoConnect.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/UrlUtils.h"

using Button = MappedInputManager::Button;

namespace {
constexpr char BOOKS_FOLDER[] = "/books";
constexpr int PROGRESS_STEP_PERCENT = 10;
constexpr unsigned long PROGRESS_MIN_INTERVAL_MS = 4000;
constexpr size_t MAX_RESULTS = 60;
// Description budget per entry: list rows only need a byline or a blurb; the
// details screen shows a full paragraph.
constexpr size_t LIST_SUMMARY_CHARS = 480;
constexpr size_t DETAIL_SUMMARY_CHARS = 900;
}  // namespace

void GetBooksActivity::onEnter() {
  Activity::onEnter();
  OPDS_STORE.loadFromFile();
  requestUpdate();
}

void GetBooksActivity::onExit() {
  Activity::onExit();
  results.clear();
  // Like every Wi-Fi activity: reboot to reclaim the heap TLS fragmented.
  if (wifiUsed && !openingBook) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

int GetBooksActivity::sourceCount() const { return getbooks::SOURCE_COUNT + static_cast<int>(OPDS_STORE.getCount()); }

const char* GetBooksActivity::originName() const {
  if (sourceIndex < getbooks::SOURCE_COUNT) return I18N.get(getbooks::SOURCES[sourceIndex].origin);
  const OpdsServer* server = OPDS_STORE.getServer(sourceIndex - getbooks::SOURCE_COUNT);
  return server ? server->name.c_str() : "";
}

// --- flow ------------------------------------------------------------------------

void GetBooksActivity::openSource(const int index) {
  if (index >= getbooks::SOURCE_COUNT) {
    const OpdsServer* server = OPDS_STORE.getServer(index - getbooks::SOURCE_COUNT);
    if (!server) return;
    auto browser = makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, *server);
    if (!browser) {
      LOG_ERR("GETBOOKS", "OOM: OPDS browser");
      return;
    }
    startActivityForResult(std::move(browser), [this](const ActivityResult&) { requestUpdate(); });
    return;
  }

  const getbooks::Source& source = getbooks::SOURCES[index];
  sourceIndex = index;
  if (!getbooks::isSearch(source)) {
    query.clear();
    listUrl = source.url;
    previousUrls.clear();
    startJob(Job::List);
    return;
  }
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GB_SEARCH_PROMPT), query, 64);
  if (!keyboard) {
    LOG_ERR("GETBOOKS", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, &source](const ActivityResult& result) {
    if (result.isCancelled) return;
    query = std::get<KeyboardResult>(result.data).text;
    if (query.empty()) return;
    listUrl = getbooks::searchUrl(source, query);
    previousUrls.clear();
    startJob(Job::List);
  });
}

void GetBooksActivity::startJob(const Job next) {
  job = next;
  requestUpdate();
}

// Draws the status popup first, then blocks on the network. The popup is the
// only refresh until the result screen lands.
void GetBooksActivity::runJob() {
  requestUpdateAndWait();
  retryJob = job;
  if (!ensureWifi()) return;

  switch (job) {
    case Job::List:
      if (fetchList()) screen = Screen::Results;
      break;
    case Job::Detail:
      if (fetchDetail()) screen = Screen::Detail;
      break;
    case Job::Download:
      screen = Screen::Download;
      job = Job::None;
      requestUpdateAndWait();
      download();
      break;
    case Job::None:
      return;
  }
  job = Job::None;
  requestUpdate();
}

bool GetBooksActivity::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  {
    // Credentials load lazily; SD shares the SPI bus with the display.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  wifiUsed = true;
  if (wifi_auto_connect::connect() == wifi_auto_connect::Result::CONNECTED) return true;

  waitingForWifi = true;
  auto picker = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, false);
  if (!picker) {
    LOG_ERR("GETBOOKS", "OOM: Wi-Fi picker");
    waitingForWifi = false;
    showError(StrId::STR_WIFI_CONN_FAILED, screen);
    return false;
  }
  startActivityForResult(std::move(picker),
                         [this](const ActivityResult& result) { onWifiResult(!result.isCancelled); });
  return false;
}

void GetBooksActivity::onWifiResult(const bool connected) {
  waitingForWifi = false;
  if (!connected || WiFi.status() != WL_CONNECTED) showError(StrId::STR_WIFI_CONN_FAILED, screen);
  // Otherwise the pending job runs on the next loop().
}

void GetBooksActivity::showError(const StrId text, const Screen returnTo) {
  job = Job::None;
  errorText = text;
  errorReturn = returnTo == Screen::Error ? Screen::Sources : returnTo;
  screen = Screen::Error;
  requestUpdate();
}

bool GetBooksActivity::fetchList() {
  const std::string url = UrlUtils::buildUrl(listUrl, "");
  LOG_DBG("GETBOOKS", "List: %s", url.c_str());
  legacy_opds::OpdsParser parser;
  parser.setSummaryLimit(LIST_SUMMARY_CHARS);
  {
    legacy_opds::OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream)) {
      showError(StrId::STR_FETCH_FEED_FAILED, screen);
      return false;
    }
  }
  if (!parser) {
    showError(StrId::STR_PARSE_FEED_FAILED, screen);
    return false;
  }

  std::vector<legacy_opds::OpdsEntry> fresh;
  fresh.reserve(std::min(parser.getEntries().size(), MAX_RESULTS));
  for (auto& entry : std::move(parser).getEntries()) {
    if (fresh.size() >= MAX_RESULTS) break;
    if (!getbooks::isBookRow(entry)) continue;
    // Relative links resolve against the feed they came from.
    entry.href = UrlUtils::buildUrl(url, entry.href);
    fresh.push_back(std::move(entry));
  }
  if (fresh.empty()) {
    showError(StrId::STR_GB_NO_RESULTS, screen);
    return false;
  }
  RenderLock lock(*this);
  nextUrl = parser.getNextPageUrl().empty() ? std::string() : UrlUtils::buildUrl(url, parser.getNextPageUrl());
  results = std::move(fresh);
  focus = 0;
  return true;
}

// Rows that are already books carry everything the screen needs. Gutenberg
// rows link to a one-book feed listing each edition; the smallest EPUB wins
// (the image-free edition is typically a twentieth of the size).
bool GetBooksActivity::fetchDetail() {
  if (focus < 0 || focus >= static_cast<int>(results.size())) return false;
  const legacy_opds::OpdsEntry& row = results[focus];
  legacy_opds::OpdsEntry chosen = row;
  if (row.type != legacy_opds::OpdsEntryType::BOOK) {
    legacy_opds::OpdsParser parser;
    parser.setSummaryLimit(DETAIL_SUMMARY_CHARS);
    {
      legacy_opds::OpdsParserStream stream{parser};
      if (!HttpDownloader::fetchUrl(row.href, stream)) {
        showError(StrId::STR_FETCH_FEED_FAILED, Screen::Results);
        return false;
      }
    }
    const legacy_opds::OpdsEntry* best = nullptr;
    const std::string* summary = nullptr;
    for (const auto& edition : parser.getEntries()) {
      if (!summary && !edition.summary.empty()) summary = &edition.summary;
      if (edition.type != legacy_opds::OpdsEntryType::BOOK) continue;
      if (!best || (edition.size > 0 && (best->size == 0 || edition.size < best->size))) best = &edition;
    }
    if (!best) {
      showError(StrId::STR_GB_NO_EPUB, Screen::Results);
      return false;
    }
    chosen.type = legacy_opds::OpdsEntryType::BOOK;
    chosen.href = UrlUtils::buildUrl(row.href, best->href);
    chosen.size = best->size;
    if (!best->author.empty()) chosen.author = best->author;
    chosen.summary = summary ? *summary : std::string();
  }
  chosen.author = getbooks::displayAuthor(chosen.author);

  const bool coverOk = fetchCover(getbooks::coverUrlFor(chosen.href));

  RenderLock lock(*this);
  hasCover = coverOk;
  detailUrl = chosen.href;
  detail = std::move(chosen);
  savedPath = destinationFor(detail);
  detailOnCard = Storage.exists(savedPath.c_str());
  return true;
}

// Best effort: without a cover the details screen keeps a text-only layout.
// Dithered to 1 bit once here, so every repaint is a plain bitmap blit.
bool GetBooksActivity::fetchCover(const std::string& url) {
  if (url.empty() || HttpDownloader::downloadToFile(url, getbooks::COVER_JPG_PATH) != HttpDownloader::OK) return false;
  bool ok = false;
  {
    HalFile jpg;
    HalFile bmp;
    if (Storage.openFileForRead("GETBOOKS", getbooks::COVER_JPG_PATH, jpg) &&
        Storage.openFileForWrite("GETBOOKS", getbooks::COVER_BMP_PATH, bmp)) {
      ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(jpg, bmp, getbooks::COVER_WIDTH,
                                                               getbooks::COVER_HEIGHT);
    }
  }
  Storage.remove(getbooks::COVER_JPG_PATH);
  if (!ok) LOG_ERR("GETBOOKS", "Cover conversion failed: %s", url.c_str());
  return ok;
}

std::string GetBooksActivity::destinationFor(const legacy_opds::OpdsEntry& entry) const {
  std::string path = BOOKS_FOLDER;
  path += '/';
  path += opdsBookFilename(entry.author, entry.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

bool GetBooksActivity::download() {
  if (!Storage.exists(BOOKS_FOLDER) && !Storage.mkdir(BOOKS_FOLDER)) {
    LOG_ERR("GETBOOKS", "mkdir %s failed", BOOKS_FOLDER);
    showError(StrId::STR_DOWNLOAD_FAILED, Screen::Detail);
    return false;
  }
  // SD-font caches repopulate on demand; TLS needs the room now.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("GETBOOKS", "Low heap for download (%u free, %u block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    showError(StrId::STR_DOWNLOAD_FAILED, Screen::Detail);
    return false;
  }

  downloaded = total = 0;
  cancelDownload = false;
  int lastPercent = -1;
  unsigned long lastUpdate = 0;
  LOG_DBG("GETBOOKS", "Download %s -> %s", detailUrl.c_str(), savedPath.c_str());
  const auto result = HttpDownloader::downloadToFile(
      detailUrl, savedPath,
      [this, &lastPercent, &lastUpdate](const size_t done, const size_t size) {
        downloaded = done;
        total = size > 0 ? size : detail.size;
        // loop() is blocked for the transfer; pump input here so Back cancels.
        mappedInput.update(true);
        if (mappedInput.wasReleased(Button::Back) || mappedInput.wasHomeGesture()) cancelDownload = true;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(done) * 100 / total) : 0;
        const unsigned long now = millis();
        if (lastPercent < 0 || percent >= lastPercent + PROGRESS_STEP_PERCENT ||
            now - lastUpdate >= PROGRESS_MIN_INTERVAL_MS) {
          lastPercent = percent;
          lastUpdate = now;
          requestUpdate(true);
        }
      },
      &cancelDownload);

  if (result == HttpDownloader::ABORTED) {
    screen = Screen::Detail;
    return false;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("GETBOOKS", "Download failed: %d", static_cast<int>(result));
    showError(StrId::STR_DOWNLOAD_FAILED, Screen::Detail);
    return false;
  }
  clearBookCache(savedPath);
  library::markLibraryIndexDirty();
  detailOnCard = true;
  screen = Screen::Done;
  return true;
}

void GetBooksActivity::openSavedBook() {
  if (savedPath.empty() || !Storage.exists(savedPath.c_str())) return;
  openingBook = true;
  APP_STATE.openEpubPath = savedPath;
  APP_STATE.saveToFile();
  if (wifiUsed) {
    WiFi.disconnect(false);
    delay(30);
    // Reboots into the reader on hardware, reclaiming the Wi-Fi heap.
    silentRestartToReader();
  }
  activityManager.goToReader(savedPath);
}

// --- input -----------------------------------------------------------------------

void GetBooksActivity::loop() {
  if (waitingForWifi) return;
  if (job != Job::None) {
    runJob();
    return;
  }
  switch (screen) {
    case Screen::Sources:
      loopSources();
      break;
    case Screen::Results:
      loopResults();
      break;
    case Screen::Detail:
      loopDetail();
      break;
    case Screen::Done:
      if (mappedInput.wasReleased(Button::Confirm)) {
        openSavedBook();
      } else if (mappedInput.wasReleased(Button::Back)) {
        screen = Screen::Detail;
        requestUpdate();
      }
      break;
    case Screen::Error:
      if (mappedInput.wasReleased(Button::Back)) {
        screen = errorReturn;
        requestUpdate();
      } else if (mappedInput.wasReleased(Button::Confirm)) {
        // Retry whatever failed.
        screen = errorReturn;
        startJob(retryJob);
      }
      break;
    case Screen::Download:
      break;
  }
}

void GetBooksActivity::goBack() {
  switch (screen) {
    case Screen::Sources:
      if (wifiUsed) {
        onGoHome();  // onExit reboots home to reclaim the Wi-Fi heap
      } else {
        activityManager.goToLibrary();
      }
      return;
    case Screen::Results:
      screen = Screen::Sources;
      break;
    default:
      screen = Screen::Results;
      break;
  }
  requestUpdate();
}

void GetBooksActivity::loopSources() {
  if (mappedInput.wasReleased(Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    openSource(sourceFocus);
    return;
  }
  const int count = sourceCount();
  const auto step = [this, count](const int direction) {
    sourceFocus = (sourceFocus + count + direction) % count;
    requestUpdate();
  };
  navigator.onPressAndContinuous({Button::NavPrevious}, [&step] { step(-1); });
  navigator.onPressAndContinuous({Button::NavNext}, [&step] { step(+1); });
}

void GetBooksActivity::stepFocus(const int delta) {
  const int count = static_cast<int>(results.size());
  if (count == 0) return;
  focus = std::clamp(focus + delta, 0, count - 1);
  requestUpdate();
}

void GetBooksActivity::loopResults() {
  if (mappedInput.wasReleased(Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    startJob(Job::Detail);
    return;
  }
  const int perPage = rowsPerPage();
  if (mappedInput.wasPressed(Button::Up)) {
    if (focus < perPage && !previousUrls.empty()) {
      // Before the first page of this feed: back to the previous one.
      listUrl = previousUrls.back();
      previousUrls.pop_back();
      startJob(Job::List);
      return;
    }
    stepFocus(-perPage);
    return;
  }
  if (mappedInput.wasPressed(Button::Down)) {
    const int last = static_cast<int>(results.size()) - 1;
    if (focus / perPage == last / perPage && !nextUrl.empty()) {
      // Past the last page of this feed: fetch the next one.
      previousUrls.push_back(listUrl);
      listUrl = nextUrl;
      startJob(Job::List);
      return;
    }
    stepFocus(perPage);
    return;
  }
  navigator.onPressAndContinuous({Button::Left}, [this] { stepFocus(-1); });
  navigator.onPressAndContinuous({Button::Right}, [this] { stepFocus(+1); });
}

void GetBooksActivity::loopDetail() {
  if (mappedInput.wasReleased(Button::Back)) {
    goBack();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    if (detailOnCard) {
      openSavedBook();
    } else {
      startJob(Job::Download);
    }
    return;
  }
  const auto neighbour = [this](const int delta) {
    const int next = focus + delta;
    if (next < 0 || next >= static_cast<int>(results.size())) return;
    focus = next;
    startJob(Job::Detail);
  };
  if (mappedInput.wasReleased(Button::Left)) neighbour(-1);
  if (mappedInput.wasReleased(Button::Right)) neighbour(+1);
}
