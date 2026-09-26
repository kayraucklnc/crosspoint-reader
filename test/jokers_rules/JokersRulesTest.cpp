#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

#include "activities/play/JokersRules.h"

using namespace jokers;

namespace {
// Rank 0..12 = 2..A; suits: 0 spades, 1 hearts, 2 clubs, 3 diamonds.
constexpr uint8_t card(const int rank, const int suit) { return static_cast<uint8_t>(suit * 13 + rank); }
constexpr int TWO = 0, THREE_ = 1, FOUR_ = 2, FIVE = 3, SIX = 4, SEVEN = 5, EIGHT = 6, NINE = 7, TEN = 8, JACK = 9,
              QUEEN = 10, KING = 11, ACE = 12;
constexpr int S = 0, H = 1, C = 2, D = 3;

HandType eval(std::initializer_list<uint8_t> cards, uint8_t* maskOut = nullptr) {
  uint8_t buf[5];
  int n = 0;
  for (const uint8_t c : cards) buf[n++] = c;
  uint8_t mask = 0;
  const HandType t = evaluate(buf, n, mask);
  if (maskOut) *maskOut = mask;
  return t;
}

// A run in the middle of a small blind with a chosen hand.
Run runWithHand(std::initializer_list<uint8_t> cards) {
  jokers::Run run;
  newRun(run, 12345);
  startBlind(run);
  run.handCount = 0;
  for (const uint8_t c : cards) run.hand[run.handCount++] = c;
  return run;
}
}  // namespace

TEST(JokersEvaluate, HighCardScoresOnlyTheTopCard) {
  uint8_t mask = 0;
  EXPECT_EQ(eval({card(TWO, S), card(KING, H), card(SEVEN, C)}, &mask), HIGH_CARD);
  EXPECT_EQ(mask, 0b010);
}

TEST(JokersEvaluate, PairsAndTrips) {
  uint8_t mask = 0;
  EXPECT_EQ(eval({card(NINE, S), card(NINE, H), card(TWO, C)}, &mask), PAIR);
  EXPECT_EQ(mask, 0b011);
  EXPECT_EQ(eval({card(NINE, S), card(TWO, H), card(NINE, C), card(TWO, D), card(ACE, D)}, &mask), TWO_PAIR);
  EXPECT_EQ(mask, 0b01111);
  EXPECT_EQ(eval({card(FIVE, S), card(FIVE, H), card(FIVE, C), card(ACE, D)}, &mask), THREE);
  EXPECT_EQ(mask, 0b0111);
  EXPECT_EQ(eval({card(FIVE, S), card(FIVE, H), card(FIVE, C), card(ACE, D), card(ACE, S)}, &mask), FULL_HOUSE);
  EXPECT_EQ(mask, 0b11111);
  EXPECT_EQ(eval({card(JACK, S), card(JACK, H), card(JACK, C), card(JACK, D), card(TWO, S)}, &mask), FOUR);
  EXPECT_EQ(mask, 0b01111);
}

TEST(JokersEvaluate, StraightsIncludingTheWheel) {
  uint8_t mask = 0;
  EXPECT_EQ(eval({card(SIX, S), card(SEVEN, H), card(EIGHT, C), card(NINE, D), card(TEN, S)}, &mask), STRAIGHT);
  EXPECT_EQ(mask, 0b11111);
  EXPECT_EQ(eval({card(ACE, S), card(TWO, H), card(THREE_, C), card(FOUR_, D), card(FIVE, S)}), STRAIGHT);
  EXPECT_EQ(eval({card(TEN, S), card(JACK, H), card(QUEEN, C), card(KING, D), card(ACE, S)}), STRAIGHT);
  // No wrap-around past the ace.
  EXPECT_EQ(eval({card(QUEEN, S), card(KING, H), card(ACE, C), card(TWO, D), card(THREE_, S)}), HIGH_CARD);
  // Four cards are never a straight.
  EXPECT_EQ(eval({card(SIX, S), card(SEVEN, H), card(EIGHT, C), card(NINE, D)}), HIGH_CARD);
}

TEST(JokersEvaluate, FlushesNeedFiveCards) {
  EXPECT_EQ(eval({card(TWO, H), card(SEVEN, H), card(NINE, H), card(JACK, H), card(KING, H)}), FLUSH);
  EXPECT_EQ(eval({card(TWO, H), card(SEVEN, H), card(NINE, H), card(JACK, H)}), HIGH_CARD);
  EXPECT_EQ(eval({card(NINE, C), card(TEN, C), card(JACK, C), card(QUEEN, C), card(KING, C)}), STRAIGHT_FLUSH);
  EXPECT_EQ(eval({card(ACE, D), card(TWO, D), card(THREE_, D), card(FOUR_, D), card(FIVE, D)}), STRAIGHT_FLUSH);
}

TEST(JokersEvaluate, ContainsFollowsPokerLogic) {
  EXPECT_TRUE(handContains(FULL_HOUSE, PAIR));
  EXPECT_TRUE(handContains(FULL_HOUSE, THREE));
  EXPECT_TRUE(handContains(FULL_HOUSE, TWO_PAIR));
  EXPECT_TRUE(handContains(FOUR, PAIR));
  EXPECT_FALSE(handContains(FOUR, TWO_PAIR));
  EXPECT_TRUE(handContains(STRAIGHT_FLUSH, FLUSH));
  EXPECT_TRUE(handContains(STRAIGHT_FLUSH, STRAIGHT));
  EXPECT_FALSE(handContains(FLUSH, STRAIGHT));
  EXPECT_FALSE(handContains(PAIR, THREE));
}

TEST(JokersScore, PairWithoutJokers) {
  jokers::Run run = runWithHand({card(KING, S), card(KING, H), card(TWO, C)});
  Breakdown b;
  scoreHand(run, 0b011, b);
  EXPECT_EQ(b.type, PAIR);
  EXPECT_EQ(b.chips, 10 + 10 + 10);
  EXPECT_EQ(b.mult10, 20);
  EXPECT_EQ(b.score, 60);
}

TEST(JokersScore, LevelsRaiseBaseValues) {
  const Level l = handLevelValues(FLUSH, 3);
  EXPECT_EQ(l.chips, 35 + 2 * 15);
  EXPECT_EQ(l.mult, 4 + 2 * 2);
}

TEST(JokersScore, JokersApplyLeftToRight) {
  jokers::Run run = runWithHand({card(KING, H), card(KING, S), card(TWO, C)});
  run.jokers[0] = HEARTBREAKER;     // +3 per scored heart
  run.jokers[1] = TWIN_DETECTIVES;  // +8 with a pair
  run.jokers[2] = CLEAN_HANDS;      // x2 without discards
  run.jokerCount = 3;
  Breakdown b;
  scoreHand(run, 0b011, b);
  EXPECT_EQ(b.chips, 30);
  EXPECT_EQ(b.mult10, (2 + 3 + 8) * 10 * 2);
  EXPECT_EQ(b.score, 30 * 26);
  EXPECT_EQ(b.fx[0].kind, Fx::MULT);
  EXPECT_EQ(b.fx[0].value, 3);
  EXPECT_EQ(b.fx[1].value, 8);
  EXPECT_EQ(b.fx[2].kind, Fx::XMULT);
  EXPECT_EQ(b.fx[2].value, 20);

  // Per-card triggers fire first; then x2 before +8 is worth less.
  std::swap(run.jokers[0], run.jokers[2]);
  scoreHand(run, 0b011, b);
  EXPECT_EQ(b.mult10, ((2 + 3) * 2 + 8) * 10);
}

TEST(JokersScore, LuckySevenStacksExactly) {
  jokers::Run run = runWithHand({card(SEVEN, H), card(SEVEN, S), card(TWO, C)});
  run.jokers[0] = LUCKY_SEVEN;
  run.jokerCount = 1;
  Breakdown b;
  scoreHand(run, 0b011, b);
  EXPECT_EQ(b.mult10, 45);  // 2 x1.5 x1.5
  EXPECT_EQ(b.fx[0].value, 22);
  EXPECT_EQ(b.score, (10 + 7 + 7) * 45 / 10);
}

TEST(JokersScore, DebuffedCardsScoreNothing) {
  jokers::Run run = runWithHand({card(KING, H), card(KING, S), card(TWO, C)});
  run.blind = 2;
  run.boss = WIDOW;
  run.jokers[0] = HEARTBREAKER;
  run.jokerCount = 1;
  Breakdown b;
  scoreHand(run, 0b011, b);
  EXPECT_EQ(b.type, PAIR);
  EXPECT_EQ(b.debuffedMask, 0b001);
  EXPECT_EQ(b.chips, 10 + 10);
  EXPECT_EQ(b.fx[0].kind, Fx::NONE);
}

TEST(JokersScore, PalaceGuardCountsHeldFaces) {
  jokers::Run run = runWithHand({card(TWO, H), card(QUEEN, S), card(JACK, C), card(FIVE, D)});
  run.jokers[0] = PALACE_GUARD;
  run.jokerCount = 1;
  Breakdown b;
  scoreHand(run, 0b0001, b);
  EXPECT_EQ(b.fx[0].value, 8);
}

TEST(JokersRound, PlayDrawsAndCountsDown) {
  jokers::Run run;
  newRun(run, 99);
  startBlind(run);
  EXPECT_EQ(run.handCount, MAX_HAND);
  EXPECT_EQ(run.handsLeft, START_HANDS);
  Breakdown b;
  EXPECT_FALSE(playHand(run, 0, b));
  EXPECT_FALSE(playHand(run, 0b111111, b));  // six cards
  ASSERT_TRUE(playHand(run, 0b11, b));
  EXPECT_EQ(run.handCount, MAX_HAND);
  EXPECT_EQ(run.deckPos, MAX_HAND + 2);
  EXPECT_EQ(run.handsLeft, START_HANDS - 1);
  EXPECT_EQ(run.roundScore, b.score);
  ASSERT_TRUE(discardCards(run, 0b11111));
  EXPECT_EQ(run.discardsLeft, START_DISCARDS - 1);
  EXPECT_EQ(run.deckPos, MAX_HAND + 7);
}

TEST(JokersRound, BossesChangeTheRound) {
  jokers::Run run;
  newRun(run, 7);
  run.blind = 2;
  run.boss = MISER;
  startBlind(run);
  EXPECT_EQ(run.handsLeft, START_HANDS - 1);
  run.boss = MUTE;
  startBlind(run);
  EXPECT_EQ(run.discardsLeft, 0);
  EXPECT_FALSE(discardCards(run, 1));
  run.boss = SQUEEZE;
  startBlind(run);
  EXPECT_EQ(run.handCount, MAX_HAND - 1);
  EXPECT_EQ(blindTarget(1, 2, true), 2 * blindTarget(1, 2, false));
}

TEST(JokersShop, CashOutBuySellReroll) {
  jokers::Run run;
  newRun(run, 5);
  startBlind(run);
  run.roundScore = currentTarget(run);
  ASSERT_TRUE(roundWon(run));
  run.money = 12;
  const Payout pay = cashOut(run);
  EXPECT_EQ(pay.blind, 3);
  EXPECT_EQ(pay.hands, START_HANDS);
  EXPECT_EQ(pay.interest, 2);
  EXPECT_EQ(run.money, 12 + 3 + 4 + 2);
  ASSERT_EQ(run.phase, Phase::SHOP);
  ASSERT_NE(run.shop[0], EMPTY);
  EXPECT_NE(run.shop[0], run.shop[1]);

  const int price = shopPrice(run, 0);
  const uint8_t bought = run.shop[0];
  const int money = run.money;
  ASSERT_TRUE(buy(run, 0));
  EXPECT_EQ(run.jokerCount, 1);
  EXPECT_EQ(run.jokers[0], bought);
  EXPECT_EQ(run.money, money - price);
  EXPECT_FALSE(buy(run, 0));  // sold out

  const HandType planet = static_cast<HandType>(run.shop[2]);
  ASSERT_TRUE(buy(run, 2));
  EXPECT_EQ(run.levels[planet], 2);

  const int before = run.money;
  ASSERT_TRUE(sellJoker(run, 0));
  EXPECT_EQ(run.jokerCount, 0);
  EXPECT_EQ(run.money, before + jokerSellValue(bought));

  run.money = REROLL_PRICE;
  ASSERT_TRUE(reroll(run));
  EXPECT_EQ(run.money, 0);
  EXPECT_EQ(run.rerollCost, REROLL_PRICE + 1);
  EXPECT_FALSE(reroll(run));
}

TEST(JokersShop, FinalBossWinsTheRun) {
  jokers::Run run;
  newRun(run, 3);
  run.ante = FINAL_ANTE;
  run.blind = 2;
  startBlind(run);
  run.roundScore = currentTarget(run);
  cashOut(run);
  EXPECT_TRUE(run.won);
  EXPECT_EQ(run.phase, Phase::OVER);
}

TEST(JokersShop, AdvanceWalksBlindsAndAntes) {
  jokers::Run run;
  newRun(run, 11);
  advanceBlind(run);
  EXPECT_EQ(run.blind, 1);
  advanceBlind(run);
  EXPECT_EQ(run.blind, 2);
  const uint8_t firstBoss = run.boss;
  advanceBlind(run);
  EXPECT_EQ(run.ante, 2);
  EXPECT_EQ(run.blind, 0);
  EXPECT_NE(run.boss, firstBoss);  // bosses do not repeat until all were seen
}

TEST(JokersSave, RoundTripsAndRejectsGarbage) {
  jokers::Run run;
  newRun(run, 4242);
  startBlind(run);
  run.jokers[0] = RAP_SHEET;
  run.jokerCounter[0] = 7;
  run.jokerCount = 1;
  run.money = 37;
  run.roundScore = 1234567890;
  uint8_t buf[SAVE_BYTES];
  const size_t n = serialize(run, buf, sizeof(buf));
  ASSERT_GT(n, 0u);
  jokers::Run back;
  ASSERT_TRUE(deserialize(back, buf, n));
  EXPECT_EQ(back.rng, run.rng);
  EXPECT_EQ(back.money, 37);
  EXPECT_EQ(back.jokerCounter[0], 7);
  EXPECT_EQ(back.roundScore, 1234567890);
  EXPECT_EQ(0, memcmp(back.deck, run.deck, DECK_SIZE));
  EXPECT_EQ(0, memcmp(back.hand, run.hand, MAX_HAND));

  EXPECT_FALSE(deserialize(back, buf, n - 1));
  const uint8_t version = buf[0];
  buf[0] = static_cast<uint8_t>(version ^ 0xFF);
  EXPECT_FALSE(deserialize(back, buf, n));
  buf[0] = version;
  const size_t deck = 1 + offsetof(jokers::Run, deck);
  std::swap(buf[deck], buf[deck + 1]);  // still a permutation: accepted
  EXPECT_TRUE(deserialize(back, buf, n));
  buf[deck] = buf[deck + 1];  // a duplicate card: rejected
  EXPECT_FALSE(deserialize(back, buf, n));
}
