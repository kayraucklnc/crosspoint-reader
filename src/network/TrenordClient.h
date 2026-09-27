#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

// Trenord departures via the public HAFAS BFF journey planner
// (https://www.trenord.it/mia/bff/hafas/v2). It needs an origin and a
// destination, so "trains towards X" is a direct (0-change) journey query to X:
// every solution is a train that calls at both stations in that order.
// Responses are AES-256-ECB encrypted with a static key; they are decrypted and
// parsed as a stream, so only the handful of fields below are ever kept.
namespace trenord {

struct Station {
  const char* apiName;  // name the BFF resolves, e.g. "MILANO CERTOSA"
  const char* label;    // short on-screen name
};

// Known stations; the defaults assume home is Milano Certosa (index 0) and the rest have
// direct trains to it, in line order. Route choices live in TrainsConfig.
constexpr Station STATIONS[] = {
    {"MILANO CERTOSA", "Certosa"},       {"MILANO VILLAPIZZONE", "Villapizzone"},
    {"MILANO LANCETTI", "Lancetti"},     {"MILANO PORTA GARIBALDI", "Garibaldi"},
    {"MILANO REPUBBLICA", "Repubblica"}, {"MILANO PORTA VENEZIA", "P. Venezia"},
    {"MILANO DATEO", "Dateo"},           {"MILANO PORTA VITTORIA", "P. Vittoria"},
    {"MILANO FORLANINI", "Forlanini"},   {"PIOLTELLO LIMITO", "Pioltello"},
    {"RHO FIERA", "Rho Fiera"},          {"RHO", "Rho"},
};
constexpr int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
// The same stations in the order the S5/S6 trains call at them, west to east.
constexpr int8_t LINE_ORDER[] = {11, 10, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
static_assert(sizeof(LINE_ORDER) == STATION_COUNT, "every station needs a place on the line");
constexpr int HOME = 0;
constexpr int DEFAULT_TOWARDS = 1;  // Villapizzone: the city-centre direction from Certosa

constexpr int16_t DELAY_UNKNOWN = -1000;

// What the train's own alerts (and per-stop cancellations) mean for this trip,
// in rising severity; a departure keeps the most severe one.
enum class Disruption : uint8_t {
  NONE,
  NOTICE,     // an informational alert on this train
  DELAY,      // an alert about delays
  BUS,        // replaced by bus on (part of) its run
  PARTIAL,    // cancelled elsewhere on its run; still runs between your stations
  CANCELLED,  // does not run between your stations
};

struct Departure {
  time_t scheduled = 0;             // UTC
  int16_t delayMin = DELAY_UNKNOWN;  // live delay; DELAY_UNKNOWN before the live window
  Disruption disruption = Disruption::NONE;
  char category[8] = {};   // "S5", "RE", "MXP"
  char number[8] = {};     // "24539"
  char direction[28] = {};  // final destination of the train
  char platform[6] = {};
  // Alert detail as Trenord words it: the cancelled stretch and reason
  // ("TREVIGLIO-PIOLTELLO LIMITO (Forza Maggiore)") or the alert message.
  char note[48] = {};

  bool cancelled() const { return disruption == Disruption::CANCELLED; }
};

struct Board {
  static constexpr int MAX_DEPARTURES = 6;
  int8_t origin = HOME;
  int8_t towards = DEFAULT_TOWARDS;
  int count = 0;
  Departure departures[MAX_DEPARTURES];
  time_t fetchedAt = 0;
};

enum class Result { OK, NO_TIME, LOW_MEMORY, HTTP_ERROR, BAD_RESPONSE };

// Parses a decrypted BFF response (JSON) fed in arbitrary chunks into `board`.
// Departures that left before `now` are skipped. About 1.9 KB of state.
class BoardParser {
 public:
  BoardParser(time_t now, Board& board);
  ~BoardParser();
  BoardParser(const BoardParser&) = delete;
  BoardParser& operator=(const BoardParser&) = delete;

  void feed(const uint8_t* data, size_t len);

 private:
  struct Impl;
  Impl* impl;
};

// Decrypts and parses a BFF response fed in arbitrary chunks into `board`.
// About 2.2 KB of state; allocate it on the heap.
class BoardDecoder {
 public:
  BoardDecoder(time_t now, Board& board);
  ~BoardDecoder();
  BoardDecoder(const BoardDecoder&) = delete;
  BoardDecoder& operator=(const BoardDecoder&) = delete;

  void feed(const uint8_t* data, size_t len);
  size_t bytesIn() const;

 private:
  struct Impl;
  Impl* impl;
};

// Blocking; requires WiFi. One HTTPS request of roughly 15-35 KB.
Result fetchBoard(int origin, int towards, Board& out);

// Europe/Rome wall clock for a UTC instant, independent of the device timezone.
void romeTime(time_t utc, struct tm& out);
// Rome civil date/time -> UTC.
time_t romeCivilToUtc(int year, int month, int day, int hour, int minute);

}  // namespace trenord
