#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
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
  };

  // Pre-flight floor for starting a TLS transfer. Below this the session or
  // its ~17KB record buffer fails mid-stream (wolfSSL MEMORY_E) — or an
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

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

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
   * Download a file to the SD card with optional credentials.
   *
   * downgradeRedirectsToHttp rewrites followed redirect targets from https to
   * http so the bulk transfer skips a second TLS session (and its ~17KB record
   * buffer — the OOM site on low-heap C3 boards).
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      bool downgradeRedirectsToHttp = false);
};
