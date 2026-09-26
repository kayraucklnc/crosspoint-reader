#include "TrenordClient.h"

#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Memory.h>

#include <new>

#include "Aes256Ecb.h"
#include "HttpDownloader.h"
#include "JsonPathScanner.h"
#include "NetworkTime.h"

namespace trenord {
namespace {
constexpr const char* BASE_URL = "https://www.trenord.it/mia/bff/hafas/v2";

// SHA-256("8hI&WK=1NQ55*f^yyZkdEGWYyN{S"), the passphrase in trenord-web-components.js.
constexpr uint8_t RESPONSE_KEY[32] = {0xaf, 0x04, 0x2a, 0x53, 0x95, 0x5d, 0xb3, 0x9c, 0xe3, 0x88, 0x69,
                                      0x2e, 0xdf, 0x84, 0xab, 0x99, 0x6a, 0x51, 0xeb, 0x01, 0x05, 0x30,
                                      0xea, 0x2a, 0xa5, 0xac, 0xa3, 0xc2, 0xd9, 0x47, 0x58, 0x2a};

constexpr const char* HEADERS[] = {
    "Accept",        "application/json, text/plain, */*",
    "Referer",       "https://www.trenord.it/store/",
    "X-3n-Language", "it",
};

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
}

// 01:00 UTC on the last Sunday of the month: when EU clocks change.
time_t euChangeover(const int year, const unsigned month) {
  const long lastDay = daysFromCivil(year, month + 1, 1) - 1;
  const long weekday = (lastDay + 4) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday
  return static_cast<time_t>((lastDay - weekday) * 86400 + 3600);
}

int romeOffsetSeconds(const time_t utc) {
  struct tm t;
  gmtime_r(&utc, &t);
  const int year = t.tm_year + 1900;
  const bool summer = utc >= euChangeover(year, 3) && utc < euChangeover(year, 10);
  return summer ? 7200 : 3600;
}

// "YYYYMMDD" + "HH:MM:SS" in Rome time -> UTC.
time_t romeToUtc(const char* date, const char* hms) {
  if (strlen(date) < 8 || strlen(hms) < 5) return 0;
  const int y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
  const int mo = (date[4] - '0') * 10 + (date[5] - '0');
  const int d = (date[6] - '0') * 10 + (date[7] - '0');
  const int h = (hms[0] - '0') * 10 + (hms[1] - '0');
  const int mi = (hms[3] - '0') * 10 + (hms[4] - '0');
  const time_t naive = static_cast<time_t>(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60);
  return naive - romeOffsetSeconds(naive - 3600);
}

int minutesOfDay(const char* hms) {
  if (strlen(hms) < 5) return -1;
  return ((hms[0] - '0') * 10 + (hms[1] - '0')) * 60 + (hms[3] - '0') * 10 + (hms[4] - '0');
}

void copyField(char* dst, const size_t size, const char* src) { snprintf(dst, size, "%s", src); }

// Accumulates one solution (journey option) and its origin stop while streaming.
struct Parser {
  Board* board;
  time_t now;
  // Current solution.
  char date[12];
  char depTime[12];
  char change[4];
  char delay[8];
  bool cancelled;
  char origin[40];
  Departure dep;
  // Current stop in the first leg's pass_list.
  char stopName[40];
  char stopPlatform[6];
  char stopEstimate[12];
  char originEstimate[12];

  void resetSolution() {
    date[0] = depTime[0] = change[0] = delay[0] = origin[0] = originEstimate[0] = '\0';
    cancelled = false;
    dep = Departure{};
    resetStop();
  }
  void resetStop() { stopName[0] = stopPlatform[0] = stopEstimate[0] = '\0'; }

  void commitSolution() {
    if (strcmp(change, "0") != 0 || board->count >= Board::MAX_DEPARTURES) return;
    dep.scheduled = romeToUtc(date, depTime);
    if (dep.scheduled == 0) return;
    const int sched = minutesOfDay(depTime);
    const int est = minutesOfDay(originEstimate);
    if (est >= 0 && sched >= 0) {
      int diff = est - sched;
      if (diff < -720) diff += 1440;  // estimate after midnight
      dep.delayMin = static_cast<int16_t>(diff);
    } else if (delay[0] && strcmp(delay, "null") != 0) {
      dep.delayMin = static_cast<int16_t>(atoi(delay));
    }
    dep.cancelled = dep.cancelled || cancelled;
    const int delaySeconds = dep.delayMin == DELAY_UNKNOWN ? 0 : dep.delayMin * 60;
    if (dep.scheduled + delaySeconds < now - 60) return;  // already gone
    board->departures[board->count++] = dep;
  }

  static void onValue(void* ctx, const JsonPathScanner::Path& p, const char* value) {
    auto& self = *static_cast<Parser*>(ctx);
    if (!p.keyIs(0, "solutions") || p.depth < 3) return;
    if (p.depth == 3) {
      const char* key = p.key(2);
      if (!key) return;
      if (strcmp(key, "date") == 0) copyField(self.date, sizeof(self.date), value);
      else if (strcmp(key, "dep_time") == 0) copyField(self.depTime, sizeof(self.depTime), value);
      else if (strcmp(key, "change") == 0) copyField(self.change, sizeof(self.change), value);
      else if (strcmp(key, "delay") == 0) copyField(self.delay, sizeof(self.delay), value);
      else if (strcmp(key, "cancelled") == 0) self.cancelled = strcmp(value, "true") == 0;
      return;
    }
    if (p.depth == 4 && p.keyIs(2, "dep_station") && p.keyIs(3, "station_ori_name")) {
      copyField(self.origin, sizeof(self.origin), value);
      return;
    }
    if (!p.keyIs(2, "journey_list") || p.index(3) != 0) return;
    if (p.depth == 6 && p.keyIs(4, "train")) {
      const char* key = p.key(5);
      if (strcmp(key, "train_category") == 0) copyField(self.dep.category, sizeof(self.dep.category), value);
      else if (strcmp(key, "train_name") == 0) copyField(self.dep.number, sizeof(self.dep.number), value);
      else if (strcmp(key, "direction") == 0) copyField(self.dep.direction, sizeof(self.dep.direction), value);
      else if (strcmp(key, "status") == 0 && strcmp(value, "S") == 0) self.dep.cancelled = true;
      return;
    }
    if (!p.keyIs(4, "pass_list")) return;
    if (p.depth == 7 && p.keyIs(6, "platform")) {
      copyField(self.stopPlatform, sizeof(self.stopPlatform), value);
    } else if (p.depth == 8 && p.keyIs(6, "station") && p.keyIs(7, "station_ori_name")) {
      copyField(self.stopName, sizeof(self.stopName), value);
    } else if (p.depth == 8 && p.keyIs(6, "actual_data") && p.keyIs(7, "dep_estimated_time")) {
      copyField(self.stopEstimate, sizeof(self.stopEstimate), value);
    }
  }

  static void onClose(void* ctx, const JsonPathScanner::Path& p) {
    auto& self = *static_cast<Parser*>(ctx);
    if (!p.keyIs(0, "solutions")) return;
    if (p.depth == 3) {  // a solution object closed
      self.commitSolution();
      self.resetSolution();
    } else if (p.depth == 7 && p.keyIs(2, "journey_list") && p.index(3) == 0 && p.keyIs(4, "pass_list")) {
      // The first leg lists the train's whole run; only our origin stop matters.
      if (self.origin[0] && strcmp(self.stopName, self.origin) == 0) {
        copyField(self.dep.platform, sizeof(self.dep.platform), self.stopPlatform);
        copyField(self.originEstimate, sizeof(self.originEstimate), self.stopEstimate);
      }
      self.resetStop();
    }
  }
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

struct BoardDecoder::Impl {
  Parser parser;
  JsonPathScanner scanner;
  Aes256EcbDecryptor aes;
  AesEcbStream stream;

  Impl(const time_t now, Board& board)
      : parser{},
        scanner({&parser, &Parser::onValue, &Parser::onClose}),
        aes(RESPONSE_KEY),
        stream(
            aes,
            [](const uint8_t* data, const size_t len, void* ctx) {
              static_cast<JsonPathScanner*>(ctx)->feed(data, len);
            },
            &scanner) {
    parser.board = &board;
    parser.now = now;
    parser.resetSolution();
  }
};

BoardDecoder::BoardDecoder(const time_t now, Board& board) : impl(new (std::nothrow) Impl(now, board)) {}
BoardDecoder::~BoardDecoder() { delete impl; }
void BoardDecoder::feed(const uint8_t* data, const size_t len) {
  if (impl) impl->stream.feed(data, len);
}
size_t BoardDecoder::bytesIn() const { return impl ? impl->stream.bytesIn() : 0; }

time_t romeCivilToUtc(const int year, const int month, const int day, const int hour, const int minute) {
  const time_t naive = static_cast<time_t>(daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60);
  return naive - romeOffsetSeconds(naive - 3600);
}

void romeTime(const time_t utc, struct tm& out) {
  const time_t local = utc + romeOffsetSeconds(utc);
  gmtime_r(&local, &out);
}

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
