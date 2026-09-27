#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Aes256Ecb.h"
#include "JsonPathScanner.h"
#include "TrenordClient.h"

// Response parsing, kept free of device APIs so host tests can feed recorded responses.
namespace trenord {
namespace {
// SHA-256("8hI&WK=1NQ55*f^yyZkdEGWYyN{S"), the passphrase in trenord-web-components.js.
constexpr uint8_t RESPONSE_KEY[32] = {0xaf, 0x04, 0x2a, 0x53, 0x95, 0x5d, 0xb3, 0x9c, 0xe3, 0x88, 0x69,
                                      0x2e, 0xdf, 0x84, 0xab, 0x99, 0x6a, 0x51, 0xeb, 0x01, 0x05, 0x30,
                                      0xea, 0x2a, 0xa5, 0xac, 0xa3, 0xc2, 0xd9, 0x47, 0x58, 0x2a};

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

// Drops a UTF-8 sequence cut short at the end of `s` (tokens and fields truncate by bytes).
void trimUtf8(char* s) {
  const size_t len = strlen(s);
  size_t i = len;
  while (i > 0 && (static_cast<uint8_t>(s[i - 1]) & 0xC0) == 0x80) i--;
  if (i == 0) return;
  const auto lead = static_cast<uint8_t>(s[i - 1]);
  const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (len - (i - 1) < need) s[i - 1] = '\0';
}

// "MILANO PORTA VENEZIA" -> "PORTA VENEZIA": the note has little room.
const char* shortStation(const char* name) {
  return strncmp(name, "MILANO ", 7) == 0 && name[7] ? name + 7 : name;
}

bool isNull(const char* value) { return value[0] == '\0' || strcmp(value, "null") == 0; }

// One element of train.alerts[], or the train's suppression_* fields.
struct Alert {
  char type[28];
  int8_t operation;  // suppressed: 1 = whole train, 2+ = part of its run; -1 = not given
  char fromMir[8];   // cancelled stretch, as station ids ("S01708")
  char toMir[8];
  char from[36];
  char to[36];
  char reason[28];
  char message[JsonPathScanner::MAX_TOKEN + 1];
  bool secondary;  // message holds secondary_message, which live news prefers

  void reset() {
    type[0] = fromMir[0] = toMir[0] = from[0] = to[0] = reason[0] = message[0] = '\0';
    operation = -1;
    secondary = false;
  }
};

// The solution leg being streamed. Only the first vehicle leg becomes the
// departure; walks (e.g. Porta Garibaldi to its Passante platforms) are skipped.
struct Leg {
  Departure dep;  // train fields, disruption and boarding-stop platform
  bool walk;
  bool suspended;  // train.status "S" (soppresso)
  Alert alert;     // current alerts[] element
  Alert suppression;
  bool sawSuppressedAlert;
  // Cancelled stretch of a partial suppression, located in pass_list.
  char rangeFrom[8];
  char rangeTo[8];
  int rangeFromIdx;
  int rangeToIdx;
  // pass_list covers your stretch (is_journey) and, with live data, the stops before it.
  int boardIdx;
  int alightIdx;
  bool boardCancelled;
  bool alightCancelled;
  bool anyCancelled;
  char boardTime[12];
  char boardEstimate[12];
  // Current stop.
  char stopId[8];
  char stopPlatform[6];
  char stopEstimate[12];
  char stopTime[12];
  bool stopJourney;
  bool stopCancelled;

  void reset() {
    dep = Departure{};
    walk = suspended = sawSuppressedAlert = false;
    alert.reset();
    suppression.reset();
    rangeFrom[0] = rangeTo[0] = boardTime[0] = boardEstimate[0] = '\0';
    rangeFromIdx = rangeToIdx = boardIdx = alightIdx = -1;
    boardCancelled = alightCancelled = anyCancelled = false;
    resetStop();
  }
  void resetStop() {
    stopId[0] = stopPlatform[0] = stopEstimate[0] = stopTime[0] = '\0';
    stopJourney = stopCancelled = false;
  }

  void raise(const Disruption kind, const char* note) {
    if (kind <= dep.disruption) return;
    dep.disruption = kind;
    copyField(dep.note, sizeof(dep.note), note);
    trimUtf8(dep.note);
  }

  // Alert types as the Trenord store app renders them (suppressed, replaced_by_bus,
  // guaranteed, custom_*_severity, and communication/warning/news live news).
  void applyAlert(const Alert& a) {
    if (strcmp(a.type, "suppressed") == 0) {
      sawSuppressedAlert = true;
      if (a.operation < 0 || a.operation == 1) {
        raise(Disruption::CANCELLED, a.reason[0] ? a.reason : a.message);
        return;
      }
      char note[sizeof(dep.note)];
      if (a.from[0] || a.to[0]) {
        snprintf(note, sizeof(note), a.reason[0] ? "%s-%s (%s)" : "%s-%s", shortStation(a.from), shortStation(a.to),
                 a.reason);
      } else {
        copyField(note, sizeof(note), a.message);
      }
      if (!rangeFrom[0] && !rangeTo[0]) {
        copyField(rangeFrom, sizeof(rangeFrom), a.fromMir);
        copyField(rangeTo, sizeof(rangeTo), a.toMir);
      }
      raise(Disruption::PARTIAL, note);
    } else if (strcmp(a.type, "replaced_by_bus") == 0) {
      raise(Disruption::BUS, a.message);
    } else if (a.message[0]) {
      raise(strstr(a.message, "ritard") || strstr(a.message, "Ritard") ? Disruption::DELAY : Disruption::NOTICE,
            a.message);
    }
  }

  // Whether the cancelled stretch overlaps your stretch by more than a shared
  // end stop: its last stop is where the shortened train starts again, its
  // first where it now terminates. A bound missing from pass_list lies past
  // the listed stops on that side.
  bool rangeCoversTrip() const {
    if (boardIdx < 0) return false;
    if (rangeFromIdx >= 0 && rangeToIdx >= 0) {
      const int lo = rangeFromIdx < rangeToIdx ? rangeFromIdx : rangeToIdx;
      const int hi = rangeFromIdx < rangeToIdx ? rangeToIdx : rangeFromIdx;
      return lo < alightIdx && hi > boardIdx;
    }
    if (rangeFromIdx >= 0) return rangeFromIdx < alightIdx;
    if (rangeToIdx >= 0) return rangeToIdx > boardIdx;
    return false;  // both bounds unlisted: the stretch lies outside your trip
  }

  void finishDisruption() {
    if (dep.disruption == Disruption::PARTIAL && rangeCoversTrip()) dep.disruption = Disruption::CANCELLED;
    if (suspended || boardCancelled || alightCancelled) {
      dep.disruption = Disruption::CANCELLED;
    } else if (anyCancelled && dep.disruption < Disruption::PARTIAL) {
      dep.disruption = Disruption::PARTIAL;  // skips a stop on your stretch
    }
  }

  void closeStop(const int index) {
    if (rangeFrom[0] && strcmp(stopId, rangeFrom) == 0) rangeFromIdx = index;
    if (rangeTo[0] && strcmp(stopId, rangeTo) == 0) rangeToIdx = index;
    if (stopJourney) {
      if (boardIdx < 0) {
        boardIdx = index;
        boardCancelled = stopCancelled;
        copyField(dep.platform, sizeof(dep.platform), stopPlatform);
        copyField(boardTime, sizeof(boardTime), stopTime);
        copyField(boardEstimate, sizeof(boardEstimate), stopEstimate);
      }
      alightIdx = index;
      alightCancelled = stopCancelled;
      anyCancelled = anyCancelled || stopCancelled;
    }
    resetStop();
  }
};

void onAlertValue(Alert& a, const char* key, const char* value) {
  if (strcmp(key, "type") == 0) copyField(a.type, sizeof(a.type), value);
  else if (strcmp(key, "operation_type") == 0) a.operation = isNull(value) ? -1 : static_cast<int8_t>(atoi(value));
  else if (strcmp(key, "from_mir_code") == 0) copyField(a.fromMir, sizeof(a.fromMir), value);
  else if (strcmp(key, "to_mir_code") == 0) copyField(a.toMir, sizeof(a.toMir), value);
  else if (strcmp(key, "from_station") == 0) copyField(a.from, sizeof(a.from), value);
  else if (strcmp(key, "to_station") == 0) copyField(a.to, sizeof(a.to), value);
  else if (strcmp(key, "reason") == 0 && !isNull(value)) copyField(a.reason, sizeof(a.reason), value);
  else if (strcmp(key, "message") == 0 && !a.secondary) copyField(a.message, sizeof(a.message), value);
  else if (strcmp(key, "secondary_message") == 0 && !isNull(value)) {
    copyField(a.message, sizeof(a.message), value);
    a.secondary = true;
  }
}

// The train object's own suppression_* fields, kept for trains without an alerts list.
void onSuppressionValue(Alert& a, const char* key, const char* value) {
  if (isNull(value)) return;
  if (strcmp(key, "suppression_start") == 0) {
    copyField(a.from, sizeof(a.from), value);
    copyField(a.type, sizeof(a.type), "suppressed");
  } else if (strcmp(key, "suppression_end") == 0) {
    copyField(a.to, sizeof(a.to), value);
  } else if (strcmp(key, "suppression_start_mir") == 0) {
    copyField(a.fromMir, sizeof(a.fromMir), value);
  } else if (strcmp(key, "suppression_end_mir") == 0) {
    copyField(a.toMir, sizeof(a.toMir), value);
  } else if (strcmp(key, "suppression_reason") == 0) {
    copyField(a.reason, sizeof(a.reason), value);
  } else if (strcmp(key, "suppression_type") == 0) {
    a.operation = static_cast<int8_t>(atoi(value));
    copyField(a.type, sizeof(a.type), "suppressed");
  }
}

// Accumulates one solution (journey option) and its first vehicle leg while streaming.
struct Parser {
  Board* board;
  time_t now;
  // Current solution.
  char date[12];
  char depTime[12];
  char change[4];
  char delay[8];
  bool cancelled;
  bool legTaken;
  Departure dep;
  char boardTime[12];
  char boardEstimate[12];
  Leg leg;

  void resetSolution() {
    date[0] = depTime[0] = change[0] = delay[0] = boardTime[0] = boardEstimate[0] = '\0';
    cancelled = legTaken = false;
    dep = Departure{};
    leg.reset();
  }

  void closeLeg() {
    if (!legTaken && !leg.walk) {
      leg.finishDisruption();
      dep = leg.dep;
      copyField(boardTime, sizeof(boardTime), leg.boardTime);
      copyField(boardEstimate, sizeof(boardEstimate), leg.boardEstimate);
      legTaken = true;
    }
    leg.reset();
  }

  void commitSolution() {
    if (!legTaken || strcmp(change, "0") != 0 || board->count >= Board::MAX_DEPARTURES) return;
    // After a leading walk the train leaves later than the solution does.
    const char* leaves = boardTime[0] ? boardTime : depTime;
    dep.scheduled = romeToUtc(date, leaves);
    if (dep.scheduled == 0) return;
    const int sched = minutesOfDay(leaves);
    if (sched >= 0 && sched < minutesOfDay(depTime)) dep.scheduled += 86400;
    const int est = minutesOfDay(boardEstimate);
    if (est >= 0 && sched >= 0) {
      int diff = est - sched;
      if (diff < -720) diff += 1440;  // estimate after midnight
      dep.delayMin = static_cast<int16_t>(diff);
    } else if (delay[0] && strcmp(delay, "null") != 0) {
      dep.delayMin = static_cast<int16_t>(atoi(delay));
    }
    if (cancelled) dep.disruption = Disruption::CANCELLED;
    const int delaySeconds = dep.delayMin == DELAY_UNKNOWN ? 0 : dep.delayMin * 60;
    if (dep.scheduled + delaySeconds < now - 60) return;  // already gone
    board->departures[board->count++] = dep;
  }

  void onLegValue(const JsonPathScanner::Path& p, const char* value) {
    if (p.depth == 5) {
      if (p.keyIs(4, "journey_type")) leg.walk = strcmp(value, "walk") == 0;
      return;
    }
    if (p.keyIs(4, "train")) {
      if (p.depth == 6) {
        const char* key = p.key(5);
        if (strcmp(key, "train_category") == 0) copyField(leg.dep.category, sizeof(leg.dep.category), value);
        else if (strcmp(key, "train_name") == 0) copyField(leg.dep.number, sizeof(leg.dep.number), value);
        else if (strcmp(key, "direction") == 0) copyField(leg.dep.direction, sizeof(leg.dep.direction), value);
        else if (strcmp(key, "status") == 0) leg.suspended = strcmp(value, "S") == 0;
        else if (strncmp(key, "suppression_", 12) == 0) onSuppressionValue(leg.suppression, key, value);
      } else if (p.depth == 8 && p.keyIs(5, "alerts")) {
        onAlertValue(leg.alert, p.key(7), value);
      }
      return;
    }
    if (!p.keyIs(4, "pass_list")) return;
    if (p.depth == 7) {
      const char* key = p.key(6);
      if (strcmp(key, "platform") == 0) copyField(leg.stopPlatform, sizeof(leg.stopPlatform), value);
      else if (strcmp(key, "dep_time") == 0) copyField(leg.stopTime, sizeof(leg.stopTime), value);
      else if (strcmp(key, "is_journey") == 0) leg.stopJourney = strcmp(value, "true") == 0;
      else if (strcmp(key, "cancelled") == 0) leg.stopCancelled = strcmp(value, "true") == 0;
    } else if (p.depth == 8 && p.keyIs(6, "station") && p.keyIs(7, "station_id")) {
      copyField(leg.stopId, sizeof(leg.stopId), value);
    } else if (p.depth == 8 && p.keyIs(6, "actual_data") && p.keyIs(7, "dep_estimated_time")) {
      copyField(leg.stopEstimate, sizeof(leg.stopEstimate), value);
    }
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
    if (p.depth >= 5 && p.keyIs(2, "journey_list")) self.onLegValue(p, value);
  }

  static void onClose(void* ctx, const JsonPathScanner::Path& p) {
    auto& self = *static_cast<Parser*>(ctx);
    if (!p.keyIs(0, "solutions")) return;
    if (p.depth == 3) {  // a solution object closed
      self.commitSolution();
      self.resetSolution();
      return;
    }
    if (p.depth < 5 || !p.keyIs(2, "journey_list")) return;
    if (p.depth == 5) {  // a leg closed
      self.closeLeg();
    } else if (p.depth == 6 && p.keyIs(4, "train")) {
      // The train object precedes pass_list, so the cancelled stretch is known before the stops stream.
      if (!self.leg.sawSuppressedAlert && self.leg.suppression.type[0]) self.leg.applyAlert(self.leg.suppression);
    } else if (p.depth == 8 && p.keyIs(4, "train") && p.keyIs(5, "alerts")) {
      self.leg.applyAlert(self.leg.alert);
      self.leg.alert.reset();
    } else if (p.depth == 7 && p.keyIs(4, "pass_list")) {
      self.leg.closeStop(p.index(5));
    }
  }
};

struct ParserCore {
  Parser parser;
  JsonPathScanner scanner;

  ParserCore(const time_t now, Board& board) : parser{}, scanner({&parser, &Parser::onValue, &Parser::onClose}) {
    parser.board = &board;
    parser.now = now;
    parser.resetSolution();
  }
};
}  // namespace

struct BoardParser::Impl : ParserCore {
  using ParserCore::ParserCore;
};

BoardParser::BoardParser(const time_t now, Board& board) : impl(new (std::nothrow) Impl(now, board)) {}
BoardParser::~BoardParser() { delete impl; }
void BoardParser::feed(const uint8_t* data, const size_t len) {
  if (impl) impl->scanner.feed(data, len);
}

struct BoardDecoder::Impl {
  ParserCore core;
  Aes256EcbDecryptor aes;
  AesEcbStream stream;

  Impl(const time_t now, Board& board)
      : core(now, board),
        aes(RESPONSE_KEY),
        stream(
            aes,
            [](const uint8_t* data, const size_t len, void* ctx) {
              static_cast<JsonPathScanner*>(ctx)->feed(data, len);
            },
            &core.scanner) {}
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

}  // namespace trenord
