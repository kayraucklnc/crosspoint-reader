#include "NetLog.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#ifndef NET_LOG_HOST_TEST
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include "NetworkTime.h"
#endif

namespace net_log {
namespace {
constexpr size_t PLACE_BYTES = 72;

// "api.stripe.com/v1/charges": the scheme and the whole query are dropped, so
// no key, token or cursor ever reaches the card.
void hostAndPath(const char* url, char* out, const size_t size) {
  const char* start = strstr(url, "://");
  start = start ? start + 3 : url;
  size_t n = strcspn(start, "?#");
  if (n >= size) n = size - 1;
  memcpy(out, start, n);
  out[n] = '\0';
}

unsigned kb(const uint32_t bytes) { return static_cast<unsigned>((bytes + 512) / 1024); }
}  // namespace

const char* errorName(const Error error) {
  switch (error) {
    case Error::LOW_MEMORY:
      return "lowmem";
    case Error::DNS:
      return "dns";
    case Error::CONNECT:
      return "connect";
    case Error::TIMEOUT:
      return "timeout";
    case Error::DROPPED:
      return "drop";
    case Error::HTTP_STATUS:
      return "http";
    case Error::NONE:
    default:
      return "slow";
  }
}

size_t format(const Entry& entry, const long long unixTime, const unsigned long uptimeMs, char* out,
              const size_t size) {
  char when[24];
  if (unixTime > 0) {
    const time_t t = static_cast<time_t>(unixTime);
    struct tm local;
    localtime_r(&t, &local);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
  } else {
    snprintf(when, sizeof(when), "+%lus", uptimeMs / 1000);
  }
  char place[PLACE_BYTES];
  hostAndPath(entry.url, place, sizeof(place));
  char tag[32] = "";
  if (entry.tag && entry.tag[0]) snprintf(tag, sizeof(tag), " [%s]", entry.tag);
  char attempt[8] = "";
  if (entry.attempt > 1) snprintf(attempt, sizeof(attempt), " try%u", entry.attempt);
  const int n = snprintf(out, size, "%s %s %s%s st=%d %s rx=%lu/%lu ms=%lu heap=%u/%u>%u/%u%s\n", when,
                         entry.error == Error::NONE ? "SLOW" : "FAIL", place, tag, entry.status, errorName(entry.error),
                         static_cast<unsigned long>(entry.bytes), static_cast<unsigned long>(entry.contentLength),
                         static_cast<unsigned long>(entry.elapsedMs), kb(entry.freeBefore), kb(entry.blockBefore),
                         kb(entry.freeAfter), kb(entry.blockAfter), attempt);
  if (n < 0) return 0;
  return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

#ifndef NET_LOG_HOST_TEST
namespace {
constexpr size_t LINE_BYTES = 200;

void appendLine(const char* line, const size_t len) {
  if (Storage.exists(PATH)) {
    HalFile current = Storage.open(PATH, O_RDONLY);
    const size_t existing = current ? current.fileSize() : 0;
    current.close();
    if (existing + len > MAX_BYTES) {
      Storage.remove(OLD_PATH);
      if (!Storage.rename(PATH, OLD_PATH)) Storage.remove(PATH);
    }
  }
  HalFile file = Storage.open(PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("NETLOG", "Cannot open %s", PATH);
    return;
  }
  file.write(reinterpret_cast<const uint8_t*>(line), len);
}
}  // namespace

void append(const Entry& entry) {
  char line[LINE_BYTES];
  const size_t len = format(entry, static_cast<long long>(network_time::current()), millis(), line, sizeof(line));
  if (len == 0) return;
  LOG_INF("NETLOG", "%.*s", static_cast<int>(len - 1), line);
  appendLine(line, len);
}

void note(const char* text) {
  char line[LINE_BYTES];
  char when[24];
  const time_t now = network_time::current();
  if (now > 0) {
    struct tm local;
    localtime_r(&now, &local);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
  } else {
    snprintf(when, sizeof(when), "+%lus", millis() / 1000);
  }
  const int n = snprintf(line, sizeof(line), "%s %s\n", when, text);
  if (n <= 0) return;
  const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line) - 1;
  LOG_INF("NETLOG", "%.*s", static_cast<int>(len - 1), line);
  appendLine(line, len);
}
#endif

}  // namespace net_log
