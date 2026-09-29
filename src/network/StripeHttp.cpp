#include "StripeHttp.h"

#include <Arduino.h>
#include <Logging.h>

#include "NetLog.h"

namespace stripe_http {
namespace {
Report last;
int requests = 0;
uint32_t received = 0;
unsigned long retryDeadline = 0;
bool hasRetryDeadline = false;

bool heapReady() {
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP &&
         ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
}

void record(const Failure failure, const int status, const uint32_t bytes) {
  last.failure = failure;
  last.status = status;
  last.bytes = bytes;
  last.freeHeap = ESP.getFreeHeap();
  last.maxBlock = ESP.getMaxAllocHeap();
}

Failure failureOf(const HttpDownloader::RequestStats& stats) {
  if (stats.status <= 0) {
    if (stats.dnsFailed) return Failure::DNS;
    return stats.stalled ? Failure::TIMEOUT : Failure::CONNECT;
  }
  if (stats.status != 200) return Failure::HTTP_STATUS;
  return stats.stalled ? Failure::TIMEOUT : Failure::DROPPED;
}

// Worth a second attempt: the network or Stripe's edge, not the request itself.
bool retryable(const HttpDownloader::RequestStats& stats) {
  if (stats.status <= 0) return !stats.dnsFailed;
  return stats.status == 200 || stats.status == 429 || stats.status >= 500;
}
}  // namespace

bool get(const std::string& url, const std::string& authorization, const HttpDownloader::DataCallback& onData,
         const char* tag, const std::function<void()>& beforeAttempt, int& statusOut) {
  statusOut = 0;
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    if (!heapReady()) {
      LOG_ERR("STRIPE", "Not enough heap for TLS: free=%u block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      record(Failure::LOW_MEMORY, 0, 0);
      net_log::Entry entry;
      entry.url = url.c_str();
      entry.tag = tag;
      entry.error = net_log::Error::LOW_MEMORY;
      entry.freeBefore = entry.freeAfter = last.freeHeap;
      entry.blockBefore = entry.blockAfter = last.maxBlock;
      entry.attempt = attempt;
      net_log::append(entry);
      return false;
    }
    if (beforeAttempt) beforeAttempt();
    HttpDownloader::RequestStats stats;
    HttpDownloader::RequestOptions options;
    options.authorization = authorization.c_str();
    options.timeoutMs = TIMEOUT_MS;
    options.logTag = tag;
    options.attempt = attempt;
    options.stats = &stats;
    requests++;
    const bool ok = HttpDownloader::fetch(url, onData, options);
    received += static_cast<uint32_t>(stats.bytes);
    statusOut = stats.status > 0 ? stats.status : 0;
    if (ok) return true;
    record(failureOf(stats), statusOut, static_cast<uint32_t>(stats.bytes));
    if (attempt == 2 || !beforeAttempt || !retryable(stats)) return false;
    if (!retryAllowed()) return false;
    // Close the kept-alive connection so the retry starts with its TLS memory back on the heap.
    HttpDownloader::KeepAlive::reset();
    delay(stats.status == 429 ? RETRY_DELAY_MS * 2 : RETRY_DELAY_MS);
    LOG_INF("STRIPE", "Retrying after %s (HTTP %d)", failureName(last.failure), statusOut);
  }
  return false;
}

void fail(const Failure failure, const int status) { record(failure, status, 0); }

const Report& lastFailure() { return last; }

const char* failureName(const Failure failure) {
  switch (failure) {
    case Failure::LOW_MEMORY:
      return "lowmem";
    case Failure::DNS:
      return "dns";
    case Failure::CONNECT:
      return "connect";
    case Failure::TIMEOUT:
      return "timeout";
    case Failure::DROPPED:
      return "drop";
    case Failure::HTTP_STATUS:
      return "http";
    case Failure::BAD_REPLY:
      return "badreply";
    case Failure::NONE:
    default:
      return "none";
  }
}

void setRetryDeadline(const unsigned long deadline) {
  retryDeadline = deadline;
  hasRetryDeadline = true;
}

bool retryAllowed() { return !hasRetryDeadline || static_cast<long>(millis() - retryDeadline) < 0; }

void resetCounters() {
  requests = 0;
  received = 0;
  last = Report{};
  hasRetryDeadline = false;
}

int requestCount() { return requests; }

uint32_t bytesReceived() { return received; }

}  // namespace stripe_http
