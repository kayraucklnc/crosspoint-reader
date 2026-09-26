#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// Line disruptions and strike notices from Trenord's line page feed
// (https://www.trenord.it/rest/render/line-details?code=S5). The response is a
// JSON string of HTML (~170 KB) listing "Stato della linea" and dated notices;
// it is scanned as a stream for text runs, so nothing large is ever held.
// The journey planner (hafas/v2) carries no notices, and its hafas_alerts list
// stays empty even on announced strike days.
namespace trenord_alerts {

struct Notice {
  time_t at = 0;
  bool critical = false;  // "Criticità" rather than "Regolare"
  char text[160] = {};
};

struct Strike {
  time_t start = 0;  // UTC; 0 = none known
  time_t end = 0;
  char text[160] = {};
};

struct Alerts {
  static constexpr int MAX_NOTICES = 3;
  bool lineCritical = false;       // current line status
  Notice notices[MAX_NOTICES];     // recent "Criticità" notices, newest first
  int noticeCount = 0;
  Strike strike;                   // next strike announced for the line
  time_t fetchedAt = 0;

  // Most recent critical notice mentioning train `number`, or nullptr.
  const Notice* forTrain(const char* number) const;
};

constexpr time_t REFRESH_SECONDS = 30 * 60;
constexpr time_t NOTICE_MAX_AGE = 6 * 3600;  // older "Criticità" notices are history

// Blocking; requires WiFi. Merges the notices of each line code (e.g. "S5").
bool fetch(const char* const* lineCodes, int lineCount, time_t now, Alerts& out);

// Streaming extractor, exposed for host tests: feed the raw response body.
// About 800 bytes of state; allocate it on the heap on the device.
class Extractor {
 public:
  Extractor(time_t now, Alerts& out) : now(now), out(out) {}
  void feed(const uint8_t* data, size_t len);
  void finish();

 private:
  time_t now;
  Alerts& out;
  // JSON string unescaping + HTML tag stripping.
  bool inString = false, escape = false, inTag = false, inEntity = false;
  int unicodeDigits = -1;
  uint32_t unicode = 0;
  char entity[10] = {};
  int entityLen = 0;
  char seg[200] = {};
  size_t segLen = 0;
  // Notice parsing: ISO time, status, ISO time, body...
  enum class Phase { SCAN, STATUS, SECOND_TIME, BODY } phase = Phase::SCAN;
  bool expectLineStatus = false;
  bool lineStatusSeen = false;
  Notice current;
  char body[400] = {};  // full notice text for strike parsing; Notice keeps a shorter copy
  size_t bodyLen = 0;

  void putChar(char c);
  void putUtf8(uint32_t cp);
  void endSegment();
  void onSegment(const char* text);
  void finishNotice();
};

}  // namespace trenord_alerts
