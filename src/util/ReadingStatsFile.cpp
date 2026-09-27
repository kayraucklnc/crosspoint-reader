#include "ReadingStatsFile.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "network/NetworkTime.h"

namespace reading_stats {

namespace file {

namespace {

// --- Calendar ---------------------------------------------------------------

// Howard Hinnant's civil_from_days, the inverse of daysFromCivil().
void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
}

}  // namespace

void formatDayKey(const int32_t day, char* buf, const size_t len) {
  int y;
  unsigned m, d;
  civilFromDays(day, y, m, d);
  snprintf(buf, len, "%04d-%02u-%02u", y, m, d);
}

bool parseDayKey(const char* s, int32_t& out) {
  if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  const int y = atoi(s);
  const int m = atoi(s + 5);
  const int d = atoi(s + 8);
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
  out = daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
  return true;
}

namespace {

// SAX pass over the stats file. Heap-allocated (the parser alone carries a
// 512-byte token buffer), never on the stack.
class Scanner {
 public:
  Scanner(BookFn onBook, DayFn onDay, void* ctx)
      : onBook(onBook),
        onDay(onDay),
        ctx(ctx),
        parser(JsonCallbacks{this, sOnKey, nullptr, sOnNumber, sOnBool, nullptr, sOnObjectStart, sOnObjectEnd, nullptr,
                             nullptr}) {}

  void feed(const char* data, const size_t len) { parser.feed(data, len); }
  bool hasError() const { return parser.hasError(); }

 private:
  enum class Section : uint8_t { None, Books, Days, Other };

  BookFn onBook;
  DayFn onDay;
  void* ctx;
  StreamingJsonParser parser;
  Section section = Section::None;
  uint8_t depth = 0;
  bool inBook = false;
  char key[256] = {};
  char field[12] = {};
  BookStats cur;

  static Scanner& self(void* p) { return *static_cast<Scanner*>(p); }

  static void copyToken(char* dst, const size_t cap, const char* src, const size_t len) {
    const size_t n = std::min(len, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
  }

  static void sOnKey(void* p, const char* k, const size_t len) {
    Scanner& s = self(p);
    if (s.depth == 1) {
      s.section = (len == 5 && memcmp(k, "books", 5) == 0)  ? Section::Books
                  : (len == 4 && memcmp(k, "days", 4) == 0) ? Section::Days
                                                            : Section::Other;
    } else if (s.depth == 2) {
      copyToken(s.key, sizeof(s.key), k, len);
    } else if (s.depth == 3) {
      copyToken(s.field, sizeof(s.field), k, len);
    }
  }

  static void sOnNumber(void* p, const char* v, size_t) {
    Scanner& s = self(p);
    const uint32_t n = static_cast<uint32_t>(strtoul(v, nullptr, 10));
    if (s.depth == 2 && s.section == Section::Days) {
      if (s.onDay) s.onDay(s.ctx, s.key, n);
      return;
    }
    if (s.depth != 3 || !s.inBook) return;
    if (strcmp(s.field, "sec") == 0) {
      s.cur.sec = n;
    } else if (strcmp(s.field, "pages") == 0) {
      s.cur.pages = n;
    } else if (strcmp(s.field, "sessions") == 0) {
      s.cur.sessions = n;
    } else if (strcmp(s.field, "first") == 0) {
      s.cur.first = n;
    } else if (strcmp(s.field, "last") == 0) {
      s.cur.last = n;
    } else if (strcmp(s.field, "wpm") == 0) {
      s.cur.wpm = static_cast<uint16_t>(std::min<uint32_t>(n, UINT16_MAX));
    }
  }

  static void sOnBool(void* p, const bool v) {
    Scanner& s = self(p);
    if (s.depth == 3 && s.inBook && strcmp(s.field, "finished") == 0) s.cur.finished = v;
  }

  static void sOnObjectStart(void* p) {
    Scanner& s = self(p);
    s.depth++;
    if (s.depth == 3 && s.section == Section::Books) {
      s.inBook = true;
      s.cur = BookStats{};
    }
  }

  static void sOnObjectEnd(void* p) {
    Scanner& s = self(p);
    if (s.depth == 3 && s.inBook) {
      s.inBook = false;
      if (s.onBook) s.onBook(s.ctx, s.key, s.cur);
    } else if (s.depth == 2) {
      s.section = Section::None;
    }
    if (s.depth > 0) s.depth--;
  }
};

}  // namespace

void recoverTempFile() {
  if (!Storage.exists(FILE_PATH) && Storage.exists(TMP_PATH)) {
    LOG_INF("RST", "Recovering stats from temp file");
    Storage.rename(TMP_PATH, FILE_PATH);
  }
}

bool scanFile(BookFn onBook, DayFn onDay, void* ctx, const bool recover) {
  if (recover) recoverTempFile();
  HalFile f;
  if (!Storage.exists(FILE_PATH) || !Storage.openFileForRead("RST", FILE_PATH, f)) return false;
  auto scanner = makeUniqueNoThrow<Scanner>(onBook, onDay, ctx);
  if (!scanner) {
    LOG_ERR("RST", "OOM: stats scanner");
    return false;
  }
  char chunk[128];
  int n;
  while ((n = f.read(chunk, sizeof(chunk))) > 0) {
    scanner->feed(chunk, static_cast<size_t>(n));
    if (scanner->hasError()) {
      LOG_ERR("RST", "Malformed stats file");
      return false;
    }
  }
  return true;
}

// --- Writer ------------------------------------------------------------------

void JsonOut::raw(const char* s) { raw(s, strlen(s)); }

void JsonOut::raw(const char* s, size_t len) {
  while (len > 0) {
    if (used == sizeof(buf)) drain();
    const size_t n = std::min(len, sizeof(buf) - used);
    memcpy(buf + used, s, n);
    used += n;
    s += n;
    len -= n;
  }
}

void JsonOut::quoted(const char* s) {
  raw("\"", 1);
  for (const char* p = s; *p; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      const char esc[2] = {'\\', static_cast<char>(c)};
      raw(esc, 2);
    } else if (c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      raw(esc);
    } else {
      raw(p, 1);
    }
  }
  raw("\"", 1);
}

void JsonOut::book(const char* path, const BookStats& b, const bool first) {
  if (!first) raw(",\n");
  quoted(path);
  char tail[160];
  snprintf(tail, sizeof(tail),
           ":{\"sec\":%lu,\"pages\":%lu,\"sessions\":%lu,\"first\":%lu,\"last\":%lu,\"finished\":%s,\"wpm\":%u}",
           static_cast<unsigned long>(b.sec), static_cast<unsigned long>(b.pages),
           static_cast<unsigned long>(b.sessions), static_cast<unsigned long>(b.first),
           static_cast<unsigned long>(b.last), b.finished ? "true" : "false", static_cast<unsigned>(b.wpm));
  raw(tail);
}

void JsonOut::day(const int32_t dayNumber, const uint32_t sec, const bool first) {
  char key[12];
  formatDayKey(dayNumber, key, sizeof(key));
  char line[32];
  snprintf(line, sizeof(line), "%s\"%s\":%lu", first ? "" : ",\n", key, static_cast<unsigned long>(sec));
  raw(line);
}

bool JsonOut::finish() {
  drain();
  return ok;
}

void JsonOut::drain() {
  if (used == 0) return;
  if (file.write(buf, used) != used) ok = false;
  used = 0;
}

}  // namespace file

// --- Public: calendar ---------------------------------------------------------

int32_t daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

int weekdayMondayFirst(const int32_t dayNumber) {
  // 1970-01-01 was a Thursday (Monday-first index 3).
  return static_cast<int>(((dayNumber + 3) % 7 + 7) % 7);
}

int32_t today() {
  const time_t now = network_time::current();
  if (now < network_time::MIN_VALID_EPOCH) return INT32_MIN;
  struct tm local;
  localtime_r(&now, &local);
  return daysFromCivil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                       static_cast<unsigned>(local.tm_mday));
}

}  // namespace reading_stats
