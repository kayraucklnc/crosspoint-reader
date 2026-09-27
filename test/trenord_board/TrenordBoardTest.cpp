#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "TrenordClient.h"

using trenord::Board;
using trenord::BoardDecoder;
using trenord::BoardParser;
using trenord::Departure;
using trenord::Disruption;

namespace {

std::string readFixture(const char* name) {
  std::ifstream in(std::string(TRENORD_FIXTURES_DIR) + "/" + name, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
  return text;
}

// Feeds odd-sized chunks, as the network does.
Board parse(const std::string& json, const time_t now) {
  Board board;
  BoardParser parser(now, board);
  constexpr size_t CHUNK = 97;
  for (size_t at = 0; at < json.size(); at += CHUNK) {
    parser.feed(reinterpret_cast<const uint8_t*>(json.data()) + at, std::min(CHUNK, json.size() - at));
  }
  return board;
}

const Departure* findTrain(const Board& board, const char* number) {
  for (int i = 0; i < board.count; i++) {
    if (strcmp(board.departures[i].number, number) == 0) return &board.departures[i];
  }
  return nullptr;
}

time_t rome(const int hour, const int minute) { return trenord::romeCivilToUtc(2026, 9, 27, hour, minute); }

// --- Recorded responses (2026-09-27): S5 24560 lost Treviglio-Pioltello to "Forza Maggiore". ---

TEST(TrenordBoard, PartialSuppressionOutsideTheTripIsLimited) {
  const Board board = parse(readFixture("pv_certosa.json"), rome(17, 40));
  ASSERT_EQ(board.count, 5);
  const Departure* train = findTrain(board, "24560");
  ASSERT_NE(train, nullptr);
  EXPECT_STREQ(train->category, "S5");
  EXPECT_STREQ(train->direction, "VARESE");
  EXPECT_EQ(train->scheduled, rome(17, 58));
  EXPECT_EQ(train->disruption, Disruption::PARTIAL);
  EXPECT_FALSE(train->cancelled());
  EXPECT_STREQ(train->note, "TREVIGLIO-PIOLTELLO LIMITO (Forza Maggiore)");
  for (int i = 0; i < board.count; i++) {
    if (&board.departures[i] == train) continue;
    EXPECT_EQ(board.departures[i].disruption, Disruption::NONE) << board.departures[i].number;
    EXPECT_STREQ(board.departures[i].note, "");
  }
}

TEST(TrenordBoard, EncryptedResponseDecodesTheSameAlert) {
  const std::string body = readFixture("pv_certosa.bin");
  ASSERT_EQ(body.size() % 16, 0u);
  Board board;
  BoardDecoder decoder(rome(17, 40), board);
  decoder.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size());
  ASSERT_EQ(board.count, 5);
  const Departure* train = findTrain(board, "24560");
  ASSERT_NE(train, nullptr);
  EXPECT_EQ(train->disruption, Disruption::PARTIAL);
  EXPECT_STREQ(train->note, "TREVIGLIO-PIOLTELLO LIMITO (Forza Maggiore)");
  // The live pass_list starts at Pioltello (17:23, platform 2); the delay and platform are Porta Venezia's.
  const Departure* first = findTrain(board, "24660");
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->delayMin, 0);
  EXPECT_STREQ(first->platform, "");
}

TEST(TrenordBoard, BoardingWhereTheShortenedTrainNowStartsIsLimited) {
  // Live pass_list: the whole run from Treviglio, its first six stops cancelled; the trip starts at Pioltello.
  const Board board = parse(readFixture("pioltello_certosa.json"), rome(17, 25));
  const Departure* train = findTrain(board, "24560");
  ASSERT_NE(train, nullptr);
  EXPECT_EQ(train->disruption, Disruption::PARTIAL);
  EXPECT_EQ(train->scheduled, rome(17, 38));
  EXPECT_EQ(train->delayMin, 0);
}

TEST(TrenordBoard, LeadingWalkIsSkippedForTheTrain) {
  // From Porta Garibaldi the S-lines leave from the Passante platforms after a 5-minute walk.
  const Board board = parse(readFixture("garibaldi_certosa.json"), rome(17, 35));
  ASSERT_EQ(board.count, 5);
  EXPECT_STREQ(board.departures[0].category, "S6");
  EXPECT_STREQ(board.departures[0].number, "24660");
  EXPECT_EQ(board.departures[0].scheduled, rome(17, 49));
  const Departure* train = findTrain(board, "24560");
  ASSERT_NE(train, nullptr);
  EXPECT_EQ(train->scheduled, rome(18, 4));
  EXPECT_EQ(train->disruption, Disruption::PARTIAL);
}

// --- The recorded alert moved onto the trip (PV S01649 ... Villapizzone S01639, Certosa S01640). ---

std::string withStretch(const char* fromMir, const char* toMir) {
  std::string json = readFixture("pv_certosa.json");
  json = replaceAll(json, "\"S01708\"", std::string("\"") + fromMir + "\"");
  return replaceAll(json, "\"S01703\"", std::string("\"") + toMir + "\"");
}

TEST(TrenordBoard, SuppressionCoveringTheTripCancelsIt) {
  const Board board = parse(withStretch("S01649", "S01639"), rome(17, 40));
  const Departure* train = findTrain(board, "24560");
  ASSERT_NE(train, nullptr);
  EXPECT_TRUE(train->cancelled());
  EXPECT_STREQ(train->note, "TREVIGLIO-PIOLTELLO LIMITO (Forza Maggiore)");
  EXPECT_EQ(findTrain(board, "24660")->disruption, Disruption::NONE);
}

TEST(TrenordBoard, SuppressionStartingMidTripCancelsIt) {
  // From Lancetti onwards to Rho, which is past the listed stops.
  const Board board = parse(withStretch("S01643", "S01039"), rome(17, 40));
  EXPECT_TRUE(findTrain(board, "24560")->cancelled());
}

TEST(TrenordBoard, SuppressionEndingAtBoardingLeavesTheTrip) {
  // Pioltello (unlisted) up to Porta Venezia: the train now starts where you board.
  const Board board = parse(withStretch("S01703", "S01649"), rome(17, 40));
  EXPECT_EQ(findTrain(board, "24560")->disruption, Disruption::PARTIAL);
}

TEST(TrenordBoard, SuppressionStartingAtAlightingLeavesTheTrip) {
  // Certosa to Rho: the train now terminates where you get off.
  const Board board = parse(withStretch("S01640", "S01039"), rome(17, 40));
  EXPECT_EQ(findTrain(board, "24560")->disruption, Disruption::PARTIAL);
}

TEST(TrenordBoard, WholeTrainSuppressionCancels) {
  const std::string json = replaceAll(readFixture("pv_certosa.json"), "\"operation_type\":2", "\"operation_type\":1");
  const Departure* train = findTrain(parse(json, rome(17, 40)), "24560");
  EXPECT_TRUE(train->cancelled());
  EXPECT_STREQ(train->note, "Forza Maggiore");
}

TEST(TrenordBoard, SuppressionFieldsWithoutAlertsList) {
  const std::string json = replaceAll(readFixture("pv_certosa.json"), "\"alerts\":", "\"ignored_alerts\":");
  const Departure* train = findTrain(parse(json, rome(17, 40)), "24560");
  EXPECT_EQ(train->disruption, Disruption::PARTIAL);
  EXPECT_STREQ(train->note, "TREVIGLIO-PIOLTELLO LIMITO (Forza Maggiore)");
}

// --- Minimal synthetic solutions: Treviglio (before the trip), Porta Venezia -> Repubblica -> Certosa. ---

std::string stop(const char* id, const char* time, const bool journey, const bool cancelled) {
  return std::string(R"({"dep_time":")") + time + R"(","station":{"station_id":")" + id +
         R"(","station_ori_name":"X"},"type":"F","is_journey":)" + (journey ? "true" : "false") +
         R"(,"actual_data":{},"cancelled":)" + (cancelled ? "true" : "false") + "}";
}

std::string solution(const std::string& trainExtra, const bool boardCancelled = false,
                     const bool solutionCancelled = false) {
  const std::string stops = stop("S01708", "17:20:00", false, false) + "," +
                            stop("S01649", "18:00:00", true, boardCancelled) + "," +
                            stop("S01648", "18:02:00", true, false) + "," + stop("S01640", "18:15:00", true, false);
  return std::string(R"({"hafas_alerts":[],"solutions":[{"date":"20260927","dep_time":"18:00:00","change":"0",)") +
         R"("journey_list":[{"train":{"train_category":"S5","train_name":"24562","direction":"VARESE")" + trainExtra +
         R"(},"pass_list":[)" + stops + R"(],"journey_type":"train"}],"delay":null,"cancelled":)" +
         (solutionCancelled ? "true" : "false") + "}]}";
}

Departure only(const std::string& json) {
  const Board board = parse(json, rome(17, 30));
  EXPECT_EQ(board.count, 1);
  return board.departures[0];
}

TEST(TrenordBoard, CleanTrainHasNoDisruption) {
  const Departure d = only(solution(R"(,"has_live_info":false)"));
  EXPECT_EQ(d.disruption, Disruption::NONE);
  EXPECT_EQ(d.scheduled, rome(18, 0));
}

TEST(TrenordBoard, CancelledBoardingStopCancels) {
  EXPECT_TRUE(only(solution("", true)).cancelled());
}

TEST(TrenordBoard, SolutionAndStatusCancellationStillCancel) {
  EXPECT_TRUE(only(solution("", false, true)).cancelled());
  EXPECT_TRUE(only(solution(R"(,"status":"S")")).cancelled());
}

TEST(TrenordBoard, ReplacementBusKeepsItsMessage) {
  const Departure d = only(solution(
      R"(,"alerts":[{"type":"replaced_by_bus","message":"Bus sostitutivo tra Rho e Certosa","operation_type":null}])"));
  EXPECT_EQ(d.disruption, Disruption::BUS);
  EXPECT_STREQ(d.note, "Bus sostitutivo tra Rho e Certosa");
}

TEST(TrenordBoard, LiveNewsPrefersSecondaryMessageAndSpotsDelays) {
  const Departure d = only(solution(
      R"(,"alerts":[{"type":"communication","message":"Comunicazione",)"
      R"("secondary_message":"Il treno viaggia in ritardo"}])"));
  EXPECT_EQ(d.disruption, Disruption::DELAY);
  EXPECT_STREQ(d.note, "Il treno viaggia in ritardo");
}

TEST(TrenordBoard, MostSevereAlertWins) {
  const Departure d = only(solution(
      R"(,"alerts":[{"type":"custom_low_severity","message":"Info"},)"
      R"({"type":"suppressed","operation_type":2,"from_mir_code":"S01648","to_mir_code":"S01039",)"
      R"("from_station":"MILANO REPUBBLICA","to_station":"RHO","reason":"Guasto"},)"
      R"({"type":"news","message":"Later news"}])"));
  EXPECT_TRUE(d.cancelled());
  EXPECT_STREQ(d.note, "REPUBBLICA-RHO (Guasto)");
}

TEST(TrenordBoard, LongMessageIsCutOnACharacterBoundary) {
  // 46 ASCII bytes, then "è": the note (47 bytes max) must not end inside it.
  const std::string text = std::string(46, 'a') + "\xc3\xa8 fine";
  const Departure d = only(solution(R"(,"alerts":[{"type":"custom_high_severity","message":")" + text + "\"}]"));
  EXPECT_EQ(d.disruption, Disruption::NOTICE);
  EXPECT_EQ(std::string(d.note), std::string(46, 'a'));
}

}  // namespace
