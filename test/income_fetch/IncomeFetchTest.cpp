#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ChargeDayTally.h"
#include "NetLog.h"
#include "StripeChargeScanner.h"

namespace {

void feed(StripeChargeScanner& scanner, const std::string& body) {
  scanner.feed(reinterpret_cast<const uint8_t*>(body.data()), body.size());
}

}  // namespace

TEST(ChargeDayTally, PageCompletesOnlyDaysAfterItsOldestRecord) {
  ChargeDayTally tally(1, 10);
  tally.add(10, 500, false);
  tally.add(9, 300, false);
  tally.add(7, 100, false);
  EXPECT_EQ(tally.commitPage(true), 8);  // days 8..10 are final; day 7 may continue on the next page
  EXPECT_EQ(tally.top(), 7);
  EXPECT_EQ(tally.cents(10), 500);
  EXPECT_EQ(tally.cents(9), 300);
  EXPECT_EQ(tally.cents(8), 0);

  tally.add(7, 50, false);
  tally.add(3, 20, true);
  EXPECT_EQ(tally.commitPage(false), 1);  // last page: everything left is final
  EXPECT_EQ(tally.top(), 0);
  EXPECT_EQ(tally.cents(7), 150);
  EXPECT_EQ(tally.nonUsd(3), 1);
  EXPECT_EQ(tally.cents(3), 0);
}

TEST(ChargeDayTally, DiscardedPageIsNotCounted) {
  ChargeDayTally tally(1, 5);
  tally.add(5, 700, false);
  tally.discardPage();  // the page dropped mid-body and is fetched again
  tally.add(5, 700, false);
  tally.add(4, 100, false);
  EXPECT_EQ(tally.commitPage(true), 5);
  EXPECT_EQ(tally.cents(5), 700);
}

TEST(ChargeDayTally, PageOfOneDayCompletesNothingUntilItMovesOn) {
  ChargeDayTally tally(1, 5);
  tally.add(5, 100, false);
  EXPECT_EQ(tally.commitPage(true), 6);  // none
  EXPECT_EQ(tally.top(), 5);
  tally.add(5, 100, false);
  tally.add(2, 10, false);
  EXPECT_EQ(tally.commitPage(true), 3);
  EXPECT_EQ(tally.cents(5), 200);
}

TEST(ChargeDayTally, IgnoresDaysOutsideTheRange) {
  ChargeDayTally tally(3, 6);
  tally.add(7, 100, false);
  tally.add(2, 100, false);
  tally.add(4, 5, false);
  EXPECT_EQ(tally.commitPage(false), 3);
  EXPECT_EQ(tally.cents(4), 5);
  EXPECT_EQ(tally.cents(2), 0);
}

TEST(StripeChargeScanner, ReadsSearchCursorLongerThanItemFields) {
  std::vector<StripeCharge> seen;
  StripeChargeScanner scanner([&seen](const StripeCharge& c) { seen.push_back(c); });
  const std::string cursor(120, 'W');
  EXPECT_FALSE(scanner.finished());
  feed(scanner, R"({"object":"search_result","data":[{"id":"ch_1","amount":699,"amount_refunded":0,)"
                R"("currency":"usd","status":"succeeded","created":1790000000,"payment_intent":"pi_9",)"
                R"("outcome":{"type":"authorized","reason":null}}],"has_more":true,"next_page":")" +
                    cursor + R"(","url":"/v1/charges/search"})");
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].paymentIntent, "pi_9");
  EXPECT_EQ(seen[0].amount, 699);
  EXPECT_TRUE(scanner.hasMore());
  EXPECT_EQ(scanner.nextPage(), cursor);
  EXPECT_TRUE(scanner.finished());
}

TEST(StripeChargeScanner, TruncatedReplyIsNotFinished) {
  StripeChargeScanner scanner([](const StripeCharge&) {});
  feed(scanner, R"({"object":"list","data":[{"id":"ch_1","amount":699},{"id":"ch_2")");
  EXPECT_FALSE(scanner.finished());
  EXPECT_FALSE(scanner.hasMore());
}

TEST(StripeChargeScanner, NullNextPageIsEmpty) {
  StripeChargeScanner scanner([](const StripeCharge&) {});
  feed(scanner, R"({"data":[],"has_more":false,"next_page":null})");
  EXPECT_TRUE(scanner.finished());
  EXPECT_TRUE(scanner.nextPage().empty());
}

TEST(NetLog, LineHasNoQueryAndCarriesTheFacts) {
  net_log::Entry entry;
  entry.url = "https://api.stripe.com/v1/charges/search?query=secret&page=token";
  entry.tag = "g 0901-0924 p2";
  entry.error = net_log::Error::TIMEOUT;
  entry.status = 200;
  entry.bytes = 84512;
  entry.contentLength = 90000;
  entry.elapsedMs = 20034;
  entry.freeBefore = 112 * 1024;
  entry.blockBefore = 52 * 1024;
  entry.freeAfter = 98 * 1024;
  entry.blockAfter = 40 * 1024;
  entry.attempt = 2;
  char line[200];
  const size_t len = net_log::format(entry, 0, 61000, line, sizeof(line));
  ASSERT_GT(len, 0u);
  const std::string text(line, len);
  EXPECT_EQ(text.find("secret"), std::string::npos);
  EXPECT_EQ(text.find("token"), std::string::npos);
  EXPECT_NE(text.find("+61s FAIL api.stripe.com/v1/charges/search [g 0901-0924 p2]"), std::string::npos);
  EXPECT_NE(text.find("st=200 timeout rx=84512/90000 ms=20034 heap=112/52>98/40 try2"), std::string::npos);
  EXPECT_EQ(text.back(), '\n');
}

TEST(NetLog, SlowSuccessIsMarkedSlow) {
  net_log::Entry entry;
  entry.url = "https://example.com/a";
  entry.status = 200;
  char line[200];
  const size_t len = net_log::format(entry, 0, 0, line, sizeof(line));
  EXPECT_NE(std::string(line, len).find("SLOW example.com/a st=200 slow"), std::string::npos);
}
