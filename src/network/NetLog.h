#pragma once

#include <cstddef>
#include <cstdint>

// One-line records of failed or slow network requests on the SD card, so a
// failure can be diagnosed on a device without a serial console:
//   2026-09-29 14:05:12 FAIL api.stripe.com/v1/charges/search [g 0901-0924] st=200 drop rx=84512/0 ms=12034
//   heap=112/52>98/40
// The file rotates to netlog.old.txt past MAX_BYTES.
namespace net_log {

constexpr const char* PATH = "/.crosspoint/netlog.txt";
constexpr const char* OLD_PATH = "/.crosspoint/netlog.old.txt";
constexpr size_t MAX_BYTES = 16 * 1024;

// Requests slower than this are logged even when they succeed.
constexpr uint32_t SLOW_MS = 8000;

// Why a request failed; NONE for a slow success.
enum class Error : uint8_t { NONE, LOW_MEMORY, DNS, CONNECT, TIMEOUT, DROPPED, HTTP_STATUS };

struct Entry {
  const char* url = "";  // only scheme-less host and path are written, never the query
  const char* tag = nullptr;
  Error error = Error::NONE;
  int status = 0;  // HTTP status, or -1 when no status line arrived
  uint32_t bytes = 0;
  uint32_t contentLength = 0;
  uint32_t elapsedMs = 0;
  uint32_t freeBefore = 0;
  uint32_t blockBefore = 0;
  uint32_t freeAfter = 0;
  uint32_t blockAfter = 0;
  uint8_t attempt = 1;
};

const char* errorName(Error error);

// Formats one line (with trailing newline) into out. Exposed for tests.
size_t format(const Entry& entry, long long unixTime, unsigned long uptimeMs, char* out, size_t size);

// Appends one line to PATH.
void append(const Entry& entry);

// Appends a free-form line (e.g. the outcome of a multi-request refresh).
void note(const char* text);

}  // namespace net_log
