#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "HttpDownloader.h"

// Requests to the Stripe API: a heap check before each one, a short socket
// timeout, one retry, and a record of the last failure for the Income page.
namespace stripe_http {

enum class Failure : uint8_t { NONE, LOW_MEMORY, DNS, CONNECT, TIMEOUT, DROPPED, HTTP_STATUS, BAD_REPLY };

struct Report {
  Failure failure = Failure::NONE;
  int status = 0;         // HTTP status of the failed request (0 when none arrived)
  uint32_t bytes = 0;     // body bytes the failed request received
  uint32_t freeHeap = 0;  // at the failure
  uint32_t maxBlock = 0;
};

// Per socket operation; a stalled page fails fast enough to be retried within a timer wake.
constexpr uint32_t TIMEOUT_MS = 20000;
constexpr uint32_t RETRY_DELAY_MS = 1500;

// GETs `url` with the key's Authorization header into onData. A transport
// failure, 429 or 5xx is retried once after the shared connection is closed
// (freeing its TLS memory); beforeAttempt runs before every attempt so the
// caller can drop a partly read page. Without beforeAttempt the caller cannot
// rewind what onData consumed, so there is no retry. Records the failure on false.
bool get(const std::string& url, const std::string& authorization, const HttpDownloader::DataCallback& onData,
         const char* tag, const std::function<void()>& beforeAttempt, int& statusOut);

// For callers that find a 200 reply unusable.
void fail(Failure failure, int status = 200);

const Report& lastFailure();
const char* failureName(Failure failure);

// No retry is started after this millis() value (the caller's time budget).
void setRetryDeadline(unsigned long deadline);
bool retryAllowed();

// Counters since the last resetCounters(): requests (with retries) and body bytes.
// Also clears the retry deadline.
void resetCounters();
int requestCount();
uint32_t bytesReceived();

}  // namespace stripe_http
