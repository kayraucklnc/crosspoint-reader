#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Noir Jokers rules: a small poker roguelike. Pure logic with no Arduino or
// renderer dependencies so the host unit tests can drive it directly.
namespace jokers {

constexpr int DECK_SIZE = 52;
constexpr int MAX_HAND = 8;
constexpr int MAX_PLAY = 5;
constexpr int MAX_JOKERS = 5;
constexpr int HAND_TYPES = 9;
constexpr int JOKER_KINDS = 25;
constexpr int BOSS_KINDS = 10;
constexpr int SHOP_JOKERS = 2;
constexpr int SHOP_SLOTS = 3;  // two jokers, then one planet
constexpr int FINAL_ANTE = 8;
constexpr int START_HANDS = 4;
constexpr int START_DISCARDS = 3;
constexpr int START_MONEY = 4;
constexpr int PLANET_PRICE = 3;
constexpr int REROLL_PRICE = 4;
constexpr uint8_t EMPTY = 0xFF;

// A card is suit * 13 + rank; rank 0..12 is 2..A, suit 0..3 is spades, hearts, clubs, diamonds.
constexpr int rankOf(const uint8_t card) { return card % 13; }
constexpr int suitOf(const uint8_t card) { return card / 13; }
constexpr bool isFace(const uint8_t card) { return rankOf(card) >= 9 && rankOf(card) <= 11; }
constexpr int cardChips(const uint8_t card) {
  return rankOf(card) == 12 ? 11 : (rankOf(card) >= 8 ? 10 : rankOf(card) + 2);
}

enum HandType : uint8_t { HIGH_CARD, PAIR, TWO_PAIR, THREE, STRAIGHT, FLUSH, FULL_HOUSE, FOUR, STRAIGHT_FLUSH };

enum Joker : uint8_t {
  WISECRACK,        // +4 Mult
  HEARTBREAKER,     // scored hearts +3 Mult each
  GRAVEDIGGER,      // scored spades +3 Mult each
  JEWELER,          // scored diamonds +3 Mult each
  BRAWLER,          // scored clubs +3 Mult each
  TWIN_DETECTIVES,  // +8 Mult if the hand contains a Pair
  TRIPLE_CROSS,     // +12 Mult if it contains Three of a Kind
  GETAWAY_DRIVER,   // +12 Mult if it contains a Straight
  FEMME_FATALE,     // +10 Mult if it contains a Flush
  DOUBLE_AGENT,     // x2 Mult if it contains Two Pair
  CRIME_FAMILY,     // x3 Mult if it contains a Full House
  SHORT_FUSE,       // +20 Mult if 3 or fewer cards are played
  MUGSHOT,          // scored face cards +30 Chips each
  LUCKY_SEVEN,      // scored 7s x1.5 Mult each
  ACE_IN_THE_HOLE,  // x2 Mult if an Ace scores
  LAST_CALL,        // x3 Mult on the last hand of the round
  CLEAN_HANDS,      // x2 Mult if no discards were used this round
  HIGH_ROLLER,      // +1 Mult per $5 held
  THE_GANG,         // +3 Mult per Joker owned
  RAP_SHEET,        // +1 Mult per hand played since bought
  PAPER_SHREDDER,   // +2 Chips per card discarded since bought
  PALACE_GUARD,     // +4 Mult per face card held in hand
  NEST_EGG,         // $3 at the end of each round
  SMUGGLER,         // $1 per face card discarded
  MUSCLE,           // +60 Chips
};

enum Boss : uint8_t {
  WIDOW,       // hearts debuffed
  UNDERTAKER,  // spades debuffed
  FENCE,       // diamonds debuffed
  BOOTLEGGER,  // clubs debuffed
  MISER,       // one hand fewer
  CENSOR,      // face cards debuffed
  MUTE,        // no discards
  SYNDICATE,   // double target
  SQUEEZE,     // one card fewer in hand
  TAXMAN,      // each hand played costs $1
};

enum class Phase : uint8_t { BLIND_SELECT, ROUND, SHOP, OVER };

int jokerPrice(uint8_t joker);
int jokerSellValue(uint8_t joker);
// 0 common, 1 uncommon, 2 rare.
int jokerRarity(uint8_t joker);

struct Level {
  int32_t chips;
  int32_t mult;
};
Level handLevelValues(HandType type, int level);

// Evaluates up to five played cards. scoringMask gets a bit per played index
// whose card counts toward the hand.
HandType evaluate(const uint8_t* cards, int count, uint8_t& scoringMask);
// True when a hand of type `played` contains a `wanted` (a Full House contains a Pair).
bool handContains(HandType played, HandType wanted);

// Per-joker outcome for the score reveal.
enum class Fx : uint8_t { NONE, CHIPS, MULT, XMULT, MONEY };
struct JokerFx {
  Fx kind = Fx::NONE;
  int32_t value = 0;  // XMULT in tenths
};

struct Breakdown {
  HandType type = HIGH_CARD;
  uint8_t played[MAX_PLAY] = {};
  uint8_t count = 0;
  uint8_t scoringMask = 0;
  uint8_t debuffedMask = 0;
  int16_t cardChips[MAX_PLAY] = {};
  int32_t baseChips = 0;
  int32_t baseMult = 0;
  int32_t cardsChips = 0;  // base plus the played cards' chips
  int32_t cardsMult10 = 0;
  JokerFx fx[MAX_JOKERS] = {};
  int32_t chips = 0;
  int32_t mult10 = 0;  // Mult in tenths so x1.5 stays exact
  int32_t score = 0;   // saturates at INT32_MAX
};

struct Payout {
  int32_t blind = 0;
  int32_t hands = 0;
  int32_t interest = 0;
  int32_t jokers = 0;
  int32_t total() const { return blind + hands + interest + jokers; }
};

struct Run {
  uint32_t rng = 1;
  Phase phase = Phase::BLIND_SELECT;
  uint8_t ante = 1;
  uint8_t blind = 0;  // 0 small, 1 big, 2 boss
  uint8_t boss = 0;
  uint16_t bossesSeen = 0;
  int32_t money = START_MONEY;
  uint8_t levels[HAND_TYPES] = {};
  uint8_t jokers[MAX_JOKERS] = {};
  int16_t jokerCounter[MAX_JOKERS] = {};
  uint8_t jokerCount = 0;
  // Round
  uint8_t deck[DECK_SIZE] = {};
  uint8_t deckPos = 0;
  uint8_t hand[MAX_HAND] = {};
  uint8_t handCount = 0;
  uint8_t handsLeft = 0;
  uint8_t discardsLeft = 0;
  uint8_t discardsUsed = 0;
  bool sortBySuit = false;
  int32_t roundScore = 0;
  // Shop
  uint8_t shop[SHOP_SLOTS] = {};
  uint8_t rerollCost = REROLL_PRICE;
  // Run stats
  int32_t bestHand = 0;
  uint16_t handsPlayed = 0;
  bool won = false;
};

uint32_t nextRandom(uint32_t& state);

void newRun(Run& run, uint32_t seed);
int32_t blindTarget(int ante, int blind, bool syndicate);
int32_t currentTarget(const Run& run);
bool bossActive(const Run& run, Boss boss);
int handSize(const Run& run);
bool isDebuffed(const Run& run, uint8_t card);
int blindReward(int blind);

// Blind flow.
void startBlind(Run& run);
void sortHand(Run& run);

// Scores `mask` (bits over run.hand) without changing the run.
void scoreHand(const Run& run, uint8_t mask, Breakdown& out);
// Plays the selected cards, draws replacements. Returns false for an invalid selection.
bool playHand(Run& run, uint8_t mask, Breakdown& out);
bool discardCards(Run& run, uint8_t mask);
bool roundWon(const Run& run);
bool roundLost(const Run& run);

// Pays out the cleared blind and opens the shop.
Payout cashOut(Run& run);
void rollShop(Run& run);
bool reroll(Run& run);
bool buy(Run& run, int slot);
bool sellJoker(Run& run, int index);
void moveJoker(Run& run, int from, int to);
int shopPrice(const Run& run, int slot);
// Leaves the shop for the next blind; the boss of the final ante ends the run.
void advanceBlind(Run& run);

// Binary save of a run in progress: a version byte, then the Run struct. A
// build with a different layout rejects old saves instead of misreading them.
constexpr size_t SAVE_BYTES = 1 + sizeof(Run);
size_t serialize(const Run& run, uint8_t* out, size_t size);
bool deserialize(Run& run, const uint8_t* in, size_t size);

}  // namespace jokers
