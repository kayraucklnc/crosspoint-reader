#include "TrenordAlerts.h"

#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

#include "HttpDownloader.h"
#include "TrenordClient.h"

namespace trenord_alerts {
namespace {
constexpr const char* URL = "https://www.trenord.it/rest/render/line-details?code=";

// "2026-09-26T14:22:42.000Z" -> UTC epoch, or 0.
time_t parseIso(const char* s) {
  int y, mo, d, h, mi, sec;
  if (strlen(s) < 19 || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  if (s[4] != '-' || s[10] != 'T') return 0;
  y -= mo <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
  return static_cast<time_t>(days) * 86400 + h * 3600 + mi * 60 + sec;
}

bool containsNoCase(const char* hay, const char* needle) {
  const size_t n = strlen(needle);
  for (const char* p = hay; *p; p++) {
    if (strncasecmp(p, needle, n) == 0) return true;
  }
  return false;
}

// "... dalle ore 11.00 alle ore 14:00 del 2 ottobre 2026 ..." -> start/end (Rome time).
// Without hours the strike covers the whole day.
bool parseStrike(const char* text, Strike& out) {
  static constexpr const char* MONTHS[] = {"gennaio", "febbraio", "marzo",     "aprile",  "maggio",   "giugno",
                                           "luglio",  "agosto",   "settembre", "ottobre", "novembre", "dicembre"};
  int day = 0, month = 0, year = 0;
  for (const char* p = text; *p && !month; p++) {
    if (!isdigit(static_cast<unsigned char>(*p)) || (p > text && isdigit(static_cast<unsigned char>(p[-1])))) continue;
    char word[16];
    int d, y;
    if (sscanf(p, "%d %15s %d", &d, word, &y) != 3 || d < 1 || d > 31 || y < 2020) continue;
    for (int m = 0; m < 12; m++) {
      if (strcasecmp(word, MONTHS[m]) == 0) {
        day = d;
        month = m + 1;
        year = y;
      }
    }
  }
  if (!month) return false;
  int h1 = 0, m1 = 0, h2 = 23, m2 = 59;
  const char* from = strstr(text, "dalle ore ");
  const char* to = from ? strstr(from + 10, "alle ore ") : nullptr;  // "dalle ore" also contains "alle ore"
  if (from && to && to > from) {
    char sep;
    if (sscanf(from + 10, "%d%c%d", &h1, &sep, &m1) != 3 || sscanf(to + 9, "%d%c%d", &h2, &sep, &m2) != 3) {
      h1 = m1 = 0;
      h2 = 23;
      m2 = 59;
    }
  }
  out.start = trenord::romeCivilToUtc(year, month, day, h1, m1);
  out.end = trenord::romeCivilToUtc(year, month, day, h2, m2);
  if (out.end <= out.start) out.end += 86400;  // e.g. 21:00 -> 21:00 next day
  snprintf(out.text, sizeof(out.text), "%s", text);
  return true;
}
}  // namespace

const Notice* Alerts::forTrain(const char* number) const {
  if (!number || !number[0]) return nullptr;
  for (int i = 0; i < noticeCount; i++) {
    const char* hit = strstr(notices[i].text, number);
    // Whole number only: "24556" must not match inside "124556".
    if (hit && (hit == notices[i].text || !isdigit(static_cast<unsigned char>(hit[-1]))) &&
        !isdigit(static_cast<unsigned char>(hit[strlen(number)]))) {
      return &notices[i];
    }
  }
  return nullptr;
}

void Extractor::putUtf8(const uint32_t cp) {
  if (cp < 0x80) {
    putChar(static_cast<char>(cp));
  } else if (cp < 0x800) {
    putChar(static_cast<char>(0xC0 | (cp >> 6)));
    putChar(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    putChar(static_cast<char>(0xE0 | (cp >> 12)));
    putChar(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    putChar(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// One character of decoded HTML: tags end a text run, entities are decoded.
void Extractor::putChar(const char c) {
  if (inTag) {
    if (c == '>') inTag = false;
    return;
  }
  if (inEntity) {
    if (c == ';' || entityLen >= static_cast<int>(sizeof(entity)) - 1) {
      entity[entityLen] = '\0';
      inEntity = false;
      if (strcmp(entity, "amp") == 0) putChar('&');
      else if (strcmp(entity, "quot") == 0) putChar('"');
      else if (strcmp(entity, "#39") == 0 || strcmp(entity, "rsquo") == 0) putChar('\'');
      else if (strcmp(entity, "nbsp") == 0) putChar(' ');
      else if (strcmp(entity, "agrave") == 0) putUtf8(0xE0);
      else if (strcmp(entity, "egrave") == 0) putUtf8(0xE8);
      else if (strcmp(entity, "ugrave") == 0) putUtf8(0xF9);
      return;
    }
    entity[entityLen++] = c;
    return;
  }
  if (c == '<') {
    endSegment();
    inTag = true;
  } else if (c == '&') {
    inEntity = true;
    entityLen = 0;
  } else {
    const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (space && (segLen == 0 || seg[segLen - 1] == ' ')) return;  // collapse whitespace
    if (segLen < sizeof(seg) - 1) seg[segLen++] = space ? ' ' : c;
  }
}

void Extractor::feed(const uint8_t* data, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = static_cast<char>(data[i]);
    if (!inString) {
      if (c == '"') inString = true;  // the only string of interest is the HTML value
      continue;
    }
    if (unicodeDigits >= 0) {
      unicode = unicode * 16 + (isdigit(static_cast<unsigned char>(c)) ? c - '0' : (tolower(c) - 'a' + 10));
      if (++unicodeDigits == 4) {
        putUtf8(unicode);
        unicodeDigits = -1;
      }
      continue;
    }
    if (escape) {
      escape = false;
      if (c == 'u') {
        unicodeDigits = 0;
        unicode = 0;
      } else {
        putChar(c == 'n' || c == 't' || c == 'r' ? ' ' : c);
      }
      continue;
    }
    if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      inString = false;
      endSegment();
    } else {
      putChar(c);
    }
  }
}

void Extractor::endSegment() {
  while (segLen > 0 && seg[segLen - 1] == ' ') segLen--;
  seg[segLen] = '\0';
  if (segLen > 0) onSegment(seg[0] == ' ' ? seg + 1 : seg);
  segLen = 0;
}

void Extractor::finishNotice() {
  if (phase == Phase::BODY && bodyLen > 0) {
    snprintf(current.text, sizeof(current.text), "%s", body);
    if (containsNoCase(body, "sciopero")) {
      Strike strike;
      // Keep the soonest strike that has not ended yet.
      if (parseStrike(body, strike) && strike.end > now &&
          (out.strike.start == 0 || strike.start < out.strike.start)) {
        out.strike = strike;
      }
    }
    if (current.critical && now - current.at < NOTICE_MAX_AGE && out.noticeCount < Alerts::MAX_NOTICES) {
      out.notices[out.noticeCount++] = current;
    }
  }
  phase = Phase::SCAN;
}

void Extractor::onSegment(const char* text) {
  if (strcmp(text, "Stato della linea") == 0) {
    expectLineStatus = !lineStatusSeen;
    return;
  }
  const time_t iso = parseIso(text);
  if (expectLineStatus && !iso) {
    out.lineCritical = out.lineCritical || strncmp(text, "Critic", 6) == 0;
    expectLineStatus = false;
    lineStatusSeen = true;
    return;
  }
  if (iso) {
    if (phase == Phase::STATUS || phase == Phase::SECOND_TIME) {
      phase = phase == Phase::SECOND_TIME ? Phase::BODY : Phase::STATUS;
      if (phase == Phase::BODY) {
        bodyLen = 0;
        body[0] = '\0';
      }
      return;
    }
    finishNotice();
    current = Notice{};
    current.at = iso;
    phase = Phase::STATUS;
    return;
  }
  if (phase == Phase::STATUS) {
    if (strncmp(text, "Critic", 6) == 0 || strcmp(text, "Regolare") == 0) {
      current.critical = text[0] == 'C';
      phase = Phase::SECOND_TIME;
    } else {
      phase = Phase::SCAN;
    }
    return;
  }
  if (phase == Phase::BODY && bodyLen < sizeof(body) - 2) {
    const int n = snprintf(body + bodyLen, sizeof(body) - bodyLen, "%s%s", bodyLen ? " " : "", text);
    bodyLen = std::min(sizeof(body) - 1, bodyLen + static_cast<size_t>(n > 0 ? n : 0));
  }
}

void Extractor::finish() {
  endSegment();
  finishNotice();
}

bool fetch(const char* const* lineCodes, const int lineCount, const time_t now, Alerts& out) {
  out = Alerts{};
  out.fetchedAt = now;
  bool any = false;
  for (int i = 0; i < lineCount; i++) {
    char url[96];
    snprintf(url, sizeof(url), "%s%s", URL, lineCodes[i]);
    auto extractor = makeUniqueNoThrow<Extractor>(now, out);
    if (!extractor) {
      LOG_ERR("TRAIN", "OOM: alerts extractor");
      return any;
    }
    int status = 0;
    const unsigned long start = millis();
    const bool ok = HttpDownloader::fetchUrlWithHeaders(
        url,
        [&extractor](const uint8_t* data, const size_t len) {
          extractor->feed(data, len);
          return true;
        },
        nullptr, 0, &status);
    extractor->finish();
    LOG_INF("TRAIN", "Alerts %s: HTTP %d, %d notices, strike %s in %lu ms", lineCodes[i], status, out.noticeCount,
            out.strike.start ? "yes" : "no", millis() - start);
    any = any || ok;
  }
  return any;
}

}  // namespace trenord_alerts
