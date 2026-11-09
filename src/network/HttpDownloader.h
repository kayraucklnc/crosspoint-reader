#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

/**
 * HTTP client utility for fetching content and downloading files, over
 * wolfSSL (freeink::SecureHttpClient); plain http is used for local servers.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    UNAUTHORIZED,  // 401/403: callers holding a refreshable credential can retry
  };

  // Pre-flight floor for starting a TLS transfer. Below this the session or
  // its ~17KB record buffer fails mid-stream (wolfSSL MEMORY_E) -- or an
  // interior allocation abort()s the device. Callers should check before
  // downloadToFile() and fail into their error UI instead.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;

  // What one request did. Failed and slow requests are also appended to
  // /.crosspoint/netlog.txt (net_log).
  struct RequestStats {
    int status = 0;  // final HTTP status; -1 when no status line arrived
    size_t bytes = 0;
    size_t contentLength = 0;  // 0 when the response had none
    uint32_t elapsedMs = 0;
    uint32_t freeBefore = 0;
    uint32_t blockBefore = 0;
    uint32_t freeAfter = 0;
    uint32_t blockAfter = 0;
    bool complete = false;  // a 200 whose body arrived in full
    bool stalled = false;   // ended by the socket timeout, not by the peer
    bool dnsFailed = false;
  };

  struct RequestOptions {
    const char* authorization = nullptr;  // sent verbatim as the Authorization header
    uint32_t timeoutMs = 0;               // per socket operation; 0 = the 60 s default
    const char* logTag = nullptr;         // short context for the netlog line
    uint8_t attempt = 1;                  // shown in the netlog line when > 1
    RequestStats* stats = nullptr;
  };

  /**
   * Stream the response body of a GET to onData with the given options.
   * Returns true for a 200 whose body arrived in full.
   */
  static bool fetch(const std::string& url, const DataCallback& onData, const RequestOptions& options);

  /**
   * While one exists, requests share one kept-alive connection (wolfSSL only),
   * so a run of API pages pays for one TLS handshake instead of one per page.
   * The connection and its TLS buffers are freed when the last one goes away.
   */
  class KeepAlive {
   public:
    KeepAlive();
    ~KeepAlive();
    KeepAlive(const KeepAlive&) = delete;
    KeepAlive& operator=(const KeepAlive&) = delete;
    // Closes the shared connection now (frees its TLS memory); the next request reconnects.
    static void reset();
  };

  using Header = std::pair<std::string, std::string>;

  /**
   * HTTP status of the last request, or 0 when it never got a response (DNS,
   * connect, TLS, timeout). fetchUrl() returns only success or failure; this
   * lets a caller tell a dead server from a catalog that wants a password.
   * Not thread-safe by design: one fetch runs at a time.
   */
  static int lastStatus();

  /**
   * True when the last answer came from a different origin (scheme, host or
   * port) than the URL asked for, after a redirect. Credentials are withheld
   * from such a hop, so a 401 there is not a wrong password.
   */
  static bool lastAnswerRedirected();

  /**
   * Fetch text content from a URL with optional credentials. `headers` are
   * added to the request, as for downloadToFile().
   */
  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", const std::vector<Header>& headers = {});

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "", const std::vector<Header>& headers = {});

  /**
   * Stream the response body to onData, sending `authorization` verbatim as
   * the Authorization header (e.g. "Bearer <token>"). statusOut receives the
   * final HTTP status (0 when no response arrived).
   */
  static bool fetchUrlWithAuthorization(const std::string& url, const DataCallback& onData,
                                        const std::string& authorization, int* statusOut = nullptr);

  /**
   * Stream the response body to onData with extra request headers, given as
   * headerCount (name, value) pairs in `headers`.
   */
  static bool fetchUrlWithHeaders(const std::string& url, const DataCallback& onData, const char* const* headers,
                                  int headerCount, int* statusOut = nullptr);

  /**
   * Requests `url` only for the server's Date response header (clock fallback
   * when NTP is blocked). Only the wolfSSL transport exposes response headers.
   */
  static bool fetchDateHeader(const std::string& url, std::string& outDate);

  /**
   * Authentication and response inspection options for fetchUrl/postForm.
   * `bearer`, when set, sends "Authorization: Bearer <token>" and overrides
   * Basic credentials. `statusOut` receives the final HTTP status (0 when the
   * request never reached a response). `captureErrorBody` also streams a 401
   * response body to the callback — OPDS authentication documents are served
   * as 401 bodies.
   */
  struct FetchOptions {
    std::string username;
    std::string password;
    std::string bearer;
    // Accept header for content negotiation (e.g. preferring OPDS 2.0 JSON
    // from servers that also speak Atom); empty sends none.
    std::string accept;
    // Accept-Language header, so servers that localize the catalog return it
    // in the reader's UI language; empty sends none.
    std::string acceptLanguage;
    // Extra headers (per-server custom headers), sent to the starting origin only.
    std::vector<Header> headers;
    int* statusOut = nullptr;
    bool captureErrorBody = false;
  };

  static bool fetchUrl(const std::string& url, const DataCallback& onData, const FetchOptions& options);

  /**
   * POST an application/x-www-form-urlencoded body (e.g. an OAuth token
   * request) and collect the response body (any status) into outResponse,
   * capped at a few KB. Returns true when the response status is 2xx.
   */
  static bool postForm(const std::string& url, const std::string& formBody, std::string& outResponse,
                       int* statusOut = nullptr);

  /**
   * Download a file to the SD card with optional credentials. `headers` are
   * added to the request (e.g. a Bearer Authorization), alongside any Basic
   * auth derived from username/password.
   *
   * downgradeRedirectsToHttp rewrites followed redirect targets from https to
   * http so the bulk transfer skips a second TLS session (and its ~17KB record
   * buffer — the OOM site on low-heap C3 boards).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<Header>& headers = {}, bool downgradeRedirectsToHttp = false);

  /**
   * POST a body (e.g. an LCP license to the fulfillment service) and stream
   * the response to the SD card, staged through "<dest>.part" like
   * downloadToFile. Single attempt, no redirects or Range resume: the
   * endpoint is ours and answers directly.
   */
  static DownloadError postToFile(const std::string& url, const std::string& body, const std::string& contentType,
                                  const std::string& destPath, ProgressCallback progress = nullptr,
                                  const bool* cancelFlag = nullptr);
};
