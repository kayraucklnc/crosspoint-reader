#include "TrenordClient.h"

#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include <Memory.h>

#include "Aes256Ecb.h"
#include "HttpDownloader.h"
#include "NetworkTime.h"

namespace trenord {
namespace {
constexpr const char* BASE_URL = "https://www.trenord.it/mia/bff/hafas/v2";

constexpr const char* HEADERS[] = {
    "Accept",        "application/json, text/plain, */*",
    "Referer",       "https://www.trenord.it/store/",
    "X-3n-Language", "it",
};

void urlEncode(const char* in, char* out, const size_t size) {
  size_t o = 0;
  for (const char* p = in; *p && o + 4 < size; p++) {
    const char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.') {
      out[o++] = c;
    } else {
      o += snprintf(out + o, size - o, "%%%02X", static_cast<unsigned char>(c));
    }
  }
  out[o] = '\0';
}
}  // namespace

Result fetchBoard(const int origin, const int towards, Board& out) {
  time_t now = 0;
  if (!network_time::ensure(now)) return Result::NO_TIME;
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("TRAIN", "Not enough heap for TLS: free=%u", static_cast<unsigned>(ESP.getFreeHeap()));
    return Result::LOW_MEMORY;
  }

  struct tm rome;
  romeTime(now, rome);
  char orig[48];
  char dest[48];
  urlEncode(STATIONS[origin].apiName, orig, sizeof(orig));
  urlEncode(STATIONS[towards].apiName, dest, sizeof(dest));
  char url[256];
  snprintf(url, sizeof(url),
           "%s?orig=%s&dest=%s&departure_date=%04d%02d%02d&departure_hour=%02d%%3A%02d&transfers=0&live_data=true"
           "&language=it",
           BASE_URL, orig, dest, rome.tm_year + 1900, rome.tm_mon + 1, rome.tm_mday, rome.tm_hour, rome.tm_min);

  auto board = makeUniqueNoThrow<Board>();
  if (!board) {
    LOG_ERR("TRAIN", "OOM: board");
    return Result::LOW_MEMORY;
  }
  board->origin = static_cast<int8_t>(origin);
  board->towards = static_cast<int8_t>(towards);
  board->fetchedAt = now;
  auto decoder = makeUniqueNoThrow<BoardDecoder>(now, *board);
  if (!decoder) {
    LOG_ERR("TRAIN", "OOM: decoder");
    return Result::LOW_MEMORY;
  }

  int status = 0;
  const unsigned long start = millis();
  const bool ok = HttpDownloader::fetchUrlWithHeaders(
      url,
      [&decoder](const uint8_t* data, const size_t len) {
        decoder->feed(data, len);
        return true;
      },
      HEADERS, 3, &status);
  if (!ok) {
    LOG_ERR("TRAIN", "Board request failed (HTTP %d)", status);
    return Result::HTTP_ERROR;
  }
  LOG_INF("TRAIN", "%s -> %s: %u bytes, %d departures in %lu ms", STATIONS[origin].label, STATIONS[towards].label,
          static_cast<unsigned>(decoder->bytesIn()), board->count, millis() - start);
  if (decoder->bytesIn() == 0 || decoder->bytesIn() % Aes256EcbDecryptor::BLOCK != 0) return Result::BAD_RESPONSE;
  out = *board;
  return Result::OK;
}

}  // namespace trenord
