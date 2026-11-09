#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <ResumableFetch.h>
#include <SecureHttpClient.h>
#include <base64.h>

#include <algorithm>
#include <functional>
#include <string>

#include "NetLog.h"
#include "WifiPowerSaveGuard.h"

#ifdef SIMULATOR
#include <netdb.h>
#else
#include <WiFi.h>
#endif

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room.
constexpr int HTTP_TIMEOUT_MS = 60000;

constexpr int MAX_REDIRECTS = 5;

// postForm response cap: token responses are small JSON; a misbehaving server
// must not balloon the heap.
constexpr size_t MAX_RESPONSE_BYTES = 4096;

void applyCommonClientSetup(freeink::SecureHttpClient& http) {
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setInsecure();
  // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
  // append a second User-Agent header, which strict servers reject (aiohttp
  // answers 400 "Duplicate 'User-Agent' header found").
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

// Written after every request, and read by the UI right after a failure.
int g_lastStatus = 0;
// Whether the last answer came from another origin than the request started at.
bool g_lastRedirected = false;

// All HTTP(S) fetches go through wolfSSL (the firmware's only TLS stack: it
// speaks TLS 1.3 and reads large bodies reliably). Plain-http URLs still use a
// WiFiClient here, so this is safe for non-TLS targets too. A body cut short
// mid-transfer resumes with a Range request (see ResumableFetch.h).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers,
                                           const freeink::FetchSink& sink, const bool* cancelFlag = nullptr,
                                           size_t* bytesOut = nullptr, const bool downgradeRedirectsToHttp = false) {
  WifiPowerSaveGuard psGuard;
  freeink::FetchOptions options;
  options.redirectToHttp = downgradeRedirectsToHttp;
  bool answeredBySameOrigin = true;
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        answeredBySameOrigin = sameOrigin;
        applyCommonClientSetup(http);
        // Credentials and caller headers stay with the starting origin; a
        // redirect elsewhere (or to plain http) gets neither.
        if (sameOrigin) {
          if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
          for (const auto& h : headers) http.addHeader(h.first, h.second);
        }
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
      sink, [cancelFlag] { return cancelFlag && *cancelFlag; });
  if (bytesOut) *bytesOut = result.bytes;
  // FetchResult::status is -1 when no response arrived (bad URL, DNS, connect, TLS).
  g_lastStatus = result.status < 0 ? 0 : result.status;
  g_lastRedirected = !answeredBySameOrigin;

  if (result.aborted) return HttpDownloader::ABORTED;
  if (result.stopped) return HttpDownloader::FILE_ERROR;
  if (result.status == 401 || result.status == 403) {
    LOG_ERR("HTTP", "wolfSSL request unauthorized: status %d: %s", result.status, url.c_str());
    return HttpDownloader::UNAUTHORIZED;
  }
  if (result.status < 200 || result.status >= 300) {
    LOG_ERR("HTTP", "wolfSSL request failed: status %d: %s", result.status, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!result.complete) {
    LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", result.bytes, result.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}


// --- Single-request path for the dashboards (Stripe, Trenord, xkcd, web app):
// per-request timeout and stats, the netlog, an optional kept-alive connection
// and the response Date header, none of which fetchResumable exposes.

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  const char* authorization = nullptr;        // full Authorization header value, e.g. "Bearer ..."
  int status = 0;                             // final HTTP status, 0 if no response
  const char* const* extraHeaders = nullptr;  // name, value pairs
  std::string* dateHeader = nullptr;          // receives the response Date header when set
  int extraHeaderCount = 0;
  uint32_t timeoutMs = HTTP_TIMEOUT_MS;
  size_t total = 0;
  size_t downloaded = 0;
  unsigned long startedAt = 0;
  unsigned long lastDataAt = 0;  // millis() of the last body chunk, 0 before the first
  bool stalled = false;          // the last socket operation ran into the timeout
};

// True when the request ended because nothing arrived for a whole timeout.
bool ranIntoTimeout(const Sink& sink) {
  const unsigned long since = sink.lastDataAt ? sink.lastDataAt : sink.startedAt;
  return millis() - since + 500 >= sink.timeoutMs;
}

// Shared connection while a HttpDownloader::KeepAlive is alive.
freeink::SecureHttpClient* sharedClient = nullptr;
int keepAliveCount = 0;

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// One request on `http`; a redirect leaves its status for the caller to follow.
int wolfHop(freeink::SecureHttpClient& http, const std::string& url, const std::string& username,
            const std::string& password, Sink& sink) {
  http.setTimeout(sink.timeoutMs);
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
    return -2;
  }
  // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
  // append a second User-Agent header, which strict servers reject (aiohttp
  // answers 400 "Duplicate 'User-Agent' header found").
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    const String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
  }
  if (sink.authorization) http.addHeader("Authorization", sink.authorization);
  for (int h = 0; h < sink.extraHeaderCount; h++) {
    http.addHeader(sink.extraHeaders[h * 2], sink.extraHeaders[h * 2 + 1]);
  }

  LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
  return http.GET(
      [&http, &sink](const uint8_t* data, size_t len) {
        if (http.getStatus() != 200) return true;
        if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
        sink.lastDataAt = millis();
        if (!sink.write(data, len)) return false;
        sink.downloaded += len;
        if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
        return true;
      },
      [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });
}

HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink, bool downgradeRedirectsToHttp) {
  WifiPowerSaveGuard psGuard;
  std::string url = startUrl;
  if (keepAliveCount > 0 && !sharedClient) sharedClient = new (std::nothrow) freeink::SecureHttpClient();

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    bool redirected = false;
    // Sets `redirected` to follow a redirect, else returns the request's outcome.
    const auto request = [&](freeink::SecureHttpClient& http) {
      const int status = wolfHop(http, url, username, password, sink);
      if (status == -2) return HttpDownloader::HTTP_ERROR;
      sink.status = status;
      if (sink.dateHeader) *sink.dateHeader = http.getHeader("date");
      if (http.aborted()) return HttpDownloader::ABORTED;
      if (status < 0) {
        sink.stalled = ranIntoTimeout(sink);
        LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
        return HttpDownloader::HTTP_ERROR;
      }
      if (isRedirect(status)) {
        const std::string location = http.getHeader("location");
        if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
          LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
          return HttpDownloader::HTTP_ERROR;
        }
        if (downgradeRedirectsToHttp && url.rfind("https://", 0) == 0) {
          // Fetch the redirect target over plain HTTP. GitHub's release-asset
          // CDN serves its signed URLs on both schemes, and skipping the second
          // TLS session removes its ~17KB record buffer — the MEMORY_E /
          // OOM-abort site on C3 heaps that sit near 45KB free.
          url.replace(0, 8, "http://");
        }
        redirected = true;
        return HttpDownloader::OK;
      }
      if (status != 200) {
        LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
        return HttpDownloader::HTTP_ERROR;
      }
      if (http.callbackAborted()) return HttpDownloader::FILE_ERROR;
      if (!http.responseComplete()) {
        sink.stalled = ranIntoTimeout(sink);
        LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
        return HttpDownloader::HTTP_ERROR;
      }
      return HttpDownloader::OK;
    };
    HttpDownloader::DownloadError outcome;
    if (sharedClient) {
      outcome = request(*sharedClient);
    } else {
      // Its own connection, closed when this hop ends.
      freeink::SecureHttpClient http;
      outcome = request(http);
    }
    if (!redirected) return outcome;
  }
  LOG_ERR("HTTP", "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}

// Only after a failure with no response: lets the netlog tell DNS from TLS/TCP.
bool hostResolves(const std::string& url) {
  const size_t hostStart = url.find("://");
  if (hostStart == std::string::npos) return true;
  const size_t from = hostStart + 3;
  const std::string host = url.substr(from, url.find_first_of(":/?", from) - from);
  if (host.empty()) return true;
#ifdef SIMULATOR
  addrinfo* found = nullptr;
  const bool ok = getaddrinfo(host.c_str(), nullptr, nullptr, &found) == 0;
  if (found) freeaddrinfo(found);
  return ok;
#else
  IPAddress ip;
  return WiFi.hostByName(host.c_str(), ip) == 1;
#endif
}

net_log::Error classify(const HttpDownloader::RequestStats& stats) {
  if (stats.status <= 0) {
    if (stats.dnsFailed) return net_log::Error::DNS;
    return stats.stalled ? net_log::Error::TIMEOUT : net_log::Error::CONNECT;
  }
  if (stats.status != 200) return net_log::Error::HTTP_STATUS;
  return stats.stalled ? net_log::Error::TIMEOUT : net_log::Error::DROPPED;
}

// runGetSecure plus the request's stats; failed and slow requests go to the netlog.
HttpDownloader::DownloadError runMeasured(const std::string& url, const std::string& username,
                                          const std::string& password, Sink& sink, const char* logTag = nullptr,
                                          const uint8_t attempt = 1, HttpDownloader::RequestStats* statsOut = nullptr,
                                          const bool downgradeRedirectsToHttp = false) {
  HttpDownloader::RequestStats stats;
  stats.freeBefore = ESP.getFreeHeap();
  stats.blockBefore = ESP.getMaxAllocHeap();
  sink.startedAt = millis();
  const HttpDownloader::DownloadError result = runGetWolf(url, username, password, sink, downgradeRedirectsToHttp);
  stats.elapsedMs = millis() - sink.startedAt;
  stats.freeAfter = ESP.getFreeHeap();
  stats.blockAfter = ESP.getMaxAllocHeap();
  stats.status = sink.status > 0 ? sink.status : -1;
  stats.bytes = sink.downloaded;
  stats.contentLength = sink.total;
  stats.complete = result == HttpDownloader::OK;
  stats.stalled = sink.stalled;

  const bool failed = result == HttpDownloader::HTTP_ERROR;
  if (failed && stats.status <= 0) stats.dnsFailed = !hostResolves(url);
  if (failed || stats.elapsedMs >= net_log::SLOW_MS) {
    net_log::Entry entry;
    entry.url = url.c_str();
    entry.tag = logTag;
    entry.error = failed ? classify(stats) : net_log::Error::NONE;
    entry.status = stats.status;
    entry.bytes = static_cast<uint32_t>(stats.bytes);
    entry.contentLength = static_cast<uint32_t>(stats.contentLength);
    entry.elapsedMs = stats.elapsedMs;
    entry.freeBefore = stats.freeBefore;
    entry.blockBefore = stats.blockBefore;
    entry.freeAfter = stats.freeAfter;
    entry.blockAfter = stats.blockAfter;
    entry.attempt = attempt;
    net_log::append(entry);
  }
  if (statsOut) *statsOut = stats;
  return result;
}
}  // namespace

int HttpDownloader::lastStatus() { return g_lastStatus; }

bool HttpDownloader::lastAnswerRedirected() { return g_lastRedirected; }

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, const std::vector<Header>& headers) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password, headers);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password, const std::vector<Header>& headers) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, headers, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const FetchOptions& options) {
  // OPDS fetches need what fetchResumable withholds: the final HTTP status
  // and a 401 body (auth documents are served as 401 bodies). Direct GET with
  // a manual redirect loop; catalog feeds are small enough that Range resume
  // isn't worth carrying here (books go through downloadToFile, which has it).
  WifiPowerSaveGuard psGuard;
  std::string current = url;
  const std::string startOrigin = freeink::fetchOrigin(url);
  int status = 0;
  bool sinkStopped = false;
  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    freeink::SecureHttpClient http;
    if (!http.begin(current)) {
      LOG_ERR("HTTP", "bad URL: %s", current.c_str());
      status = 0;
      break;
    }
    applyCommonClientSetup(http);
    // Credentials belong to the starting origin only: a redirect to another
    // origin (a CDN) must not receive them.
    if (freeink::fetchOrigin(current) == startOrigin) {
      if (!options.bearer.empty()) {
        http.addHeader("Authorization", "Bearer " + options.bearer);
      } else if (!options.username.empty() && !options.password.empty()) {
        http.setBasicAuth(options.username, options.password);
      }
      for (const auto& h : options.headers) http.addHeader(h.first, h.second);
    }
    if (!options.accept.empty()) http.addHeader("Accept", options.accept);
    if (!options.acceptLanguage.empty()) http.addHeader("Accept-Language", options.acceptLanguage);

    LOG_DBG("HTTP", "wolfSSL GET: %s", current.c_str());
    status = http.GET([&](const uint8_t* data, size_t len) {
      const int st = http.getStatus();
      const bool wanted = (st >= 200 && st < 300) || (options.captureErrorBody && st == 401);
      if (!wanted) return true;  // redirect or error body: drain
      if (!onData(data, len)) {
        sinkStopped = true;
        return false;
      }
      return true;
    });
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(current, location, current)) break;
      continue;
    }
    break;
  }
  g_lastStatus = status > 0 ? status : 0;
  g_lastRedirected = freeink::fetchOrigin(current) != startOrigin;
  if (options.statusOut) *options.statusOut = g_lastStatus;
  return status >= 200 && status < 300 && !sinkStopped;
}

bool HttpDownloader::postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                              int* statusOut) {
  WifiPowerSaveGuard psGuard;
  outResponse.clear();
  freeink::SecureHttpClient http;
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "bad URL: %s", url.c_str());
    if (statusOut) *statusOut = 0;
    return false;
  }
  applyCommonClientSetup(http);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  LOG_DBG("HTTP", "wolfSSL POST: %s", url.c_str());
  const int status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(formBody.data()), formBody.size(),
                                      [&outResponse](const uint8_t* data, size_t len) {
                                        const size_t room =
                                            MAX_RESPONSE_BYTES - std::min(outResponse.size(), MAX_RESPONSE_BYTES);
                                        outResponse.append(reinterpret_cast<const char*>(data), std::min(len, room));
                                        return true;
                                      });
  if (statusOut) *statusOut = status > 0 ? status : 0;
  return status >= 200 && status < 300;
}

HttpDownloader::DownloadError HttpDownloader::postToFile(const std::string& url, const std::string& body,
                                                         const std::string& contentType, const std::string& destPath,
                                                         ProgressCallback progress, const bool* cancelFlag) {
  LOG_DBG("HTTP", "POST download: %s -> %s", url.c_str(), destPath.c_str());
  WifiPowerSaveGuard psGuard;

  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::SecureHttpClient http;
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "bad URL: %s", url.c_str());
    return HTTP_ERROR;
  }
  applyCommonClientSetup(http);
  http.addHeader("Content-Type", contentType);

  size_t downloaded = 0;
  bool writeFailed = false;
  const int status = http.sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, size_t len) {
        const int st = http.getStatus();
        if (st < 200 || st >= 300) return true;  // drain the error body
        if (file.write(data, len) != len) {
          writeFailed = true;
          return false;
        }
        downloaded += len;
        if (progress && http.hasContentLength()) progress(downloaded, http.getContentLength());
        return true;
      },
      [cancelFlag] { return cancelFlag && *cancelFlag; });
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  if (file.isOpen()) file.close();

  DownloadError result = OK;
  if (http.aborted()) {
    result = ABORTED;
  } else if (writeFailed) {
    result = FILE_ERROR;
  } else if (status < 200 || status >= 300) {
    LOG_ERR("HTTP", "POST failed: status %d: %s", status, url.c_str());
    result = HTTP_ERROR;
  } else if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    result = HTTP_ERROR;
  } else if (http.hasContentLength() && downloaded != http.getContentLength()) {
    LOG_ERR("HTTP", "POST response incomplete: %zu of %zu bytes", downloaded, (size_t)http.getContentLength());
    result = HTTP_ERROR;
  }
  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             bool downgradeRedirectsToHttp) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  // Stage in <dest>.part: a failed or cancelled download never replaces an
  // existing copy, and a partial file never sits under the real name.
  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::FetchSink sink;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the server restarted the body from byte 0.
  sink.rewind = [&file, &partPath] {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };
  sink.progress = progress;

  size_t downloaded = 0;
  const DownloadError result =
      runGetSecure(url, username, password, headers, sink, cancelFlag, &downloaded, downgradeRedirectsToHttp);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove. A failed rewind leaves no open handle.
  if (file.isOpen()) file.close();

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return OK;
}

bool HttpDownloader::fetchUrlWithAuthorization(const std::string& url, const DataCallback& onData,
                                               const std::string& authorization, int* statusOut) {
  LOG_DBG("HTTP", "Fetching with auth header: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.authorization = authorization.c_str();
  const bool ok = runMeasured(url, "", "", sink) == OK;
  if (statusOut) *statusOut = sink.status;
  return ok;
}

bool HttpDownloader::fetchUrlWithHeaders(const std::string& url, const DataCallback& onData, const char* const* headers,
                                         const int headerCount, int* statusOut) {
  LOG_DBG("HTTP", "Fetching with headers: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.extraHeaders = headers;
  sink.extraHeaderCount = headerCount;
  const bool ok = runMeasured(url, "", "", sink) == OK;
  if (statusOut) *statusOut = sink.status;
  return ok;
}

bool HttpDownloader::fetch(const std::string& url, const DataCallback& onData, const RequestOptions& options) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  sink.authorization = options.authorization;
  if (options.timeoutMs > 0) sink.timeoutMs = options.timeoutMs;
  return runMeasured(url, "", "", sink, options.logTag, options.attempt, options.stats) == OK;
}

HttpDownloader::KeepAlive::KeepAlive() {
  keepAliveCount++;
}

HttpDownloader::KeepAlive::~KeepAlive() {
  if (--keepAliveCount > 0) return;
  delete sharedClient;  // closes the connection and frees its TLS session
  sharedClient = nullptr;
}

void HttpDownloader::KeepAlive::reset() {
  if (sharedClient) sharedClient->end();
}

bool HttpDownloader::fetchDateHeader(const std::string& url, std::string& outDate) {
  Sink sink;
  sink.write = [](const uint8_t*, size_t) { return false; };  // headers are all we need
  sink.dateHeader = &outDate;
  outDate.clear();
  runGetWolf(url, "", "", sink, false);
  return !outDate.empty();
}
