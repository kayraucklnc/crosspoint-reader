#include "JokersRules.h"

#include <algorithm>
#include <cstring>

namespace jokers {
namespace {
constexpr int32_t BASE_CHIPS[HAND_TYPES] = {5, 10, 20, 30, 30, 35, 40, 60, 100};
constexpr int32_t BASE_MULT[HAND_TYPES] = {1, 2, 2, 3, 4, 4, 4, 7, 8};
constexpr int32_t LEVEL_CHIPS[HAND_TYPES] = {10, 15, 20, 20, 30, 15, 25, 30, 40};
constexpr int32_t LEVEL_MULT[HAND_TYPES] = {1, 1, 1, 2, 3, 2, 2, 3, 4};
constexpr int32_t ANTE_BASE[FINAL_ANTE] = {300, 700, 1500, 3000, 5500, 9000, 14000, 21000};
constexpr uint8_t PRICES[JOKER_KINDS] = {3, 5, 5, 5, 5, 4, 5, 5, 5, 7, 8, 5, 5, 7, 7, 7, 6, 5, 6, 6, 5, 6, 5, 4, 4};
constexpr uint8_t RARITY[JOKER_KINDS] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 0, 0, 2, 2, 2, 1, 0, 1, 1, 0, 1, 0, 0, 0};
constexpr int RARITY_ROLL[2] = {60, 90};  // percent: common below 60, uncommon below 90, else rare
constexpr int MAX_LEVEL = 99;
constexpr int MAX_INTEREST = 5;
// Bump by hand whenever Run changes shape.
constexpr uint8_t SAVE_VERSION = 2;
constexpr uint8_t MAX_REROLL_PRICE = 99;

int popcount(uint8_t v) {
  int n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

bool owns(const Run& run, const uint8_t joker) {
  for (int i = 0; i < run.jokerCount; i++) {
    if (run.jokers[i] == joker) return true;
  }
  return false;
}

int sortKey(const uint8_t card, const bool bySuit) {
  return bySuit ? suitOf(card) * 16 + (12 - rankOf(card)) : (12 - rankOf(card)) * 4 + suitOf(card);
}

void drawToHandSize(Run& run) {
  const int size = handSize(run);
  while (run.handCount < size && run.deckPos < DECK_SIZE) run.hand[run.handCount++] = run.deck[run.deckPos++];
}

// Removes the masked cards from the hand, keeping order.
void removeCards(Run& run, const uint8_t mask) {
  int kept = 0;
  for (int i = 0; i < run.handCount; i++) {
    if (!(mask & (1u << i))) run.hand[kept++] = run.hand[i];
  }
  run.handCount = static_cast<uint8_t>(kept);
}

bool validSelection(const Run& run, const uint8_t mask) {
  const int n = popcount(mask);
  return n >= 1 && n <= MAX_PLAY && (mask >> run.handCount) == 0;
}

void pickBoss(Run& run) {
  if (run.bossesSeen == (1u << BOSS_KINDS) - 1) run.bossesSeen = 0;
  int left = 0;
  for (int b = 0; b < BOSS_KINDS; b++) left += !(run.bossesSeen & (1u << b));
  int pick = static_cast<int>(nextRandom(run.rng) % left);
  for (int b = 0; b < BOSS_KINDS; b++) {
    if (run.bossesSeen & (1u << b)) continue;
    if (pick-- == 0) {
      run.boss = static_cast<uint8_t>(b);
      run.bossesSeen |= static_cast<uint16_t>(1u << b);
      return;
    }
  }
}

uint8_t rollJoker(Run& run) {
  const int roll = static_cast<int>(nextRandom(run.rng) % 100);
  const int rarity = roll < RARITY_ROLL[0] ? 0 : (roll < RARITY_ROLL[1] ? 1 : 2);
  uint8_t pool[JOKER_KINDS];
  int n = 0;
  for (int pass = 0; pass < 2 && n == 0; pass++) {
    for (int j = 0; j < JOKER_KINDS; j++) {
      const bool offered = run.shop[0] == j || run.shop[1] == j;
      if (!owns(run, j) && !offered && (pass == 1 || RARITY[j] == rarity)) pool[n++] = static_cast<uint8_t>(j);
    }
  }
  return n ? pool[nextRandom(run.rng) % n] : EMPTY;
}

}  // namespace

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

int jokerPrice(const uint8_t joker) { return joker < JOKER_KINDS ? PRICES[joker] : 0; }
int jokerSellValue(const uint8_t joker) { return std::max(1, jokerPrice(joker) / 2); }
int jokerRarity(const uint8_t joker) { return joker < JOKER_KINDS ? RARITY[joker] : 0; }

Level handLevelValues(const HandType type, const int level) {
  const int up = std::max(0, level - 1);
  return {BASE_CHIPS[type] + LEVEL_CHIPS[type] * up, BASE_MULT[type] + LEVEL_MULT[type] * up};
}

HandType evaluate(const uint8_t* cards, const int count, uint8_t& scoringMask) {
  scoringMask = 0;
  if (count <= 0) return HIGH_CARD;
  int counts[13] = {};
  bool flush = count == MAX_PLAY;
  int lo = 12, hi = 0;
  for (int i = 0; i < count; i++) {
    const int r = rankOf(cards[i]);
    counts[r]++;
    lo = std::min(lo, r);
    hi = std::max(hi, r);
    if (suitOf(cards[i]) != suitOf(cards[0])) flush = false;
  }
  int distinct = 0, four = -1, three = -1, pairHi = -1, pairLo = -1;
  for (int r = 0; r < 13; r++) {
    if (!counts[r]) continue;
    distinct++;
    if (counts[r] >= 4) four = r;
    if (counts[r] == 3) three = r;
    if (counts[r] == 2) {
      pairLo = pairHi;
      pairHi = r;
    }
  }
  const bool wheel = counts[12] && counts[0] && counts[1] && counts[2] && counts[3];
  const bool straight = count == MAX_PLAY && distinct == MAX_PLAY && (hi - lo == 4 || wheel);
  const auto markRank = [&](const int rank) {
    for (int i = 0; i < count; i++) {
      if (rankOf(cards[i]) == rank) scoringMask |= static_cast<uint8_t>(1u << i);
    }
  };
  const uint8_t all = static_cast<uint8_t>((1u << count) - 1);
  if (straight && flush) {
    scoringMask = all;
    return STRAIGHT_FLUSH;
  }
  if (four >= 0) {
    markRank(four);
    return FOUR;
  }
  if (three >= 0 && pairHi >= 0) {
    scoringMask = all;
    return FULL_HOUSE;
  }
  if (flush || straight) {
    scoringMask = all;
    return flush ? FLUSH : STRAIGHT;
  }
  if (three >= 0) {
    markRank(three);
    return THREE;
  }
  if (pairLo >= 0) {
    markRank(pairHi);
    markRank(pairLo);
    return TWO_PAIR;
  }
  if (pairHi >= 0) {
    markRank(pairHi);
    return PAIR;
  }
  for (int i = 0; i < count; i++) {
    if (rankOf(cards[i]) == hi) {
      scoringMask = static_cast<uint8_t>(1u << i);
      break;
    }
  }
  return HIGH_CARD;
}

bool handContains(const HandType played, const HandType wanted) {
  switch (wanted) {
    case PAIR:
      return played == PAIR || played == TWO_PAIR || played == THREE || played == FULL_HOUSE || played == FOUR;
    case TWO_PAIR:
      return played == TWO_PAIR || played == FULL_HOUSE;
    case THREE:
      return played == THREE || played == FULL_HOUSE || played == FOUR;
    case STRAIGHT:
    case FLUSH:
      return played == wanted || played == STRAIGHT_FLUSH;
    default:
      return played == wanted;
  }
}

void newRun(Run& run, const uint32_t seed) {
  run = Run{};
  run.rng = seed | 1u;
  for (auto& level : run.levels) level = 1;
  for (auto& slot : run.shop) slot = EMPTY;
  pickBoss(run);
}

int32_t blindTarget(const int ante, const int blind, const bool syndicate) {
  const int32_t base = ANTE_BASE[std::clamp(ante, 1, FINAL_ANTE) - 1];
  const int32_t scaled = blind == 0 ? base : (blind == 1 ? base * 3 / 2 : base * 2);
  return syndicate && blind == 2 ? scaled * 2 : scaled;
}

int32_t currentTarget(const Run& run) { return blindTarget(run.ante, run.blind, bossActive(run, SYNDICATE)); }

bool bossActive(const Run& run, const Boss boss) { return run.blind == 2 && run.boss == boss; }

int handSize(const Run& run) { return MAX_HAND - (bossActive(run, SQUEEZE) ? 1 : 0); }

int blindReward(const int blind) { return 3 + blind; }

bool isDebuffed(const Run& run, const uint8_t card) {
  if (run.blind != 2) return false;
  switch (run.boss) {
    case WIDOW:
      return suitOf(card) == 1;
    case UNDERTAKER:
      return suitOf(card) == 0;
    case FENCE:
      return suitOf(card) == 3;
    case BOOTLEGGER:
      return suitOf(card) == 2;
    case CENSOR:
      return isFace(card);
    default:
      return false;
  }
}

void sortHand(Run& run) {
  for (int i = 1; i < run.handCount; i++) {
    const uint8_t card = run.hand[i];
    int j = i - 1;
    while (j >= 0 && sortKey(run.hand[j], run.sortBySuit) > sortKey(card, run.sortBySuit)) {
      run.hand[j + 1] = run.hand[j];
      j--;
    }
    run.hand[j + 1] = card;
  }
}

void startBlind(Run& run) {
  run.phase = Phase::ROUND;
  for (int i = 0; i < DECK_SIZE; i++) run.deck[i] = static_cast<uint8_t>(i);
  for (int i = DECK_SIZE - 1; i > 0; i--) std::swap(run.deck[i], run.deck[nextRandom(run.rng) % (i + 1)]);
  run.deckPos = 0;
  run.handCount = 0;
  drawToHandSize(run);
  sortHand(run);
  run.handsLeft = static_cast<uint8_t>(START_HANDS - (bossActive(run, MISER) ? 1 : 0));
  run.discardsLeft = static_cast<uint8_t>(bossActive(run, MUTE) ? 0 : START_DISCARDS);
  run.discardsUsed = 0;
  run.roundScore = 0;
}

void scoreHand(const Run& run, const uint8_t mask, Breakdown& out) {
  out = Breakdown{};
  for (int i = 0; i < run.handCount && out.count < MAX_PLAY; i++) {
    if (mask & (1u << i)) out.played[out.count++] = run.hand[i];
  }
  if (out.count == 0) return;
  out.type = evaluate(out.played, out.count, out.scoringMask);
  const Level level = handLevelValues(out.type, run.levels[out.type]);
  out.baseChips = level.chips;
  out.baseMult = level.mult;
  int32_t chips = level.chips, mult10 = level.mult * 10;
  int32_t cardTotal = 0;
  bool aceScored = false;
  for (int i = 0; i < out.count; i++) {
    const uint8_t card = out.played[i];
    if (!(out.scoringMask & (1u << i))) continue;
    if (isDebuffed(run, card)) {
      out.debuffedMask |= static_cast<uint8_t>(1u << i);
      continue;
    }
    out.cardChips[i] = static_cast<int16_t>(cardChips(card));
    chips += out.cardChips[i];
    cardTotal += out.cardChips[i];
    aceScored = aceScored || rankOf(card) == 12;
    for (int j = 0; j < run.jokerCount; j++) {
      JokerFx& fx = out.fx[j];
      const uint8_t joker = run.jokers[j];
      const bool suitHit = (joker == HEARTBREAKER && suitOf(card) == 1) ||
                           (joker == GRAVEDIGGER && suitOf(card) == 0) || (joker == JEWELER && suitOf(card) == 3) ||
                           (joker == BRAWLER && suitOf(card) == 2);
      if (suitHit) {
        mult10 += 30;
        fx = {Fx::MULT, fx.value + 3};
      } else if (joker == MUGSHOT && isFace(card)) {
        chips += 30;
        fx = {Fx::CHIPS, fx.value + 30};
      } else if (joker == LUCKY_SEVEN && rankOf(card) == 5) {
        mult10 = mult10 * 15 / 10;
        fx = {Fx::XMULT, fx.value ? fx.value * 15 / 10 : 15};
      }
    }
  }
  out.cardsChips = level.chips + cardTotal;
  out.cardsMult10 = level.mult * 10;

  int heldFaces = 0;
  for (int i = 0; i < run.handCount; i++) {
    if (!(mask & (1u << i)) && isFace(run.hand[i]) && !isDebuffed(run, run.hand[i])) heldFaces++;
  }
  for (int j = 0; j < run.jokerCount; j++) {
    int32_t addMult = 0, addChips = 0, times10 = 0;
    switch (run.jokers[j]) {
      case WISECRACK:
        addMult = 4;
        break;
      case TWIN_DETECTIVES:
        addMult = handContains(out.type, PAIR) ? 8 : 0;
        break;
      case TRIPLE_CROSS:
        addMult = handContains(out.type, THREE) ? 12 : 0;
        break;
      case GETAWAY_DRIVER:
        addMult = handContains(out.type, STRAIGHT) ? 12 : 0;
        break;
      case FEMME_FATALE:
        addMult = handContains(out.type, FLUSH) ? 10 : 0;
        break;
      case DOUBLE_AGENT:
        times10 = handContains(out.type, TWO_PAIR) ? 20 : 0;
        break;
      case CRIME_FAMILY:
        times10 = handContains(out.type, FULL_HOUSE) ? 30 : 0;
        break;
      case SHORT_FUSE:
        addMult = out.count <= 3 ? 20 : 0;
        break;
      case ACE_IN_THE_HOLE:
        times10 = aceScored ? 20 : 0;
        break;
      case LAST_CALL:
        times10 = run.handsLeft == 1 ? 30 : 0;
        break;
      case CLEAN_HANDS:
        times10 = run.discardsUsed == 0 ? 20 : 0;
        break;
      case HIGH_ROLLER:
        addMult = std::max<int32_t>(0, run.money) / 5;
        break;
      case THE_GANG:
        addMult = 3 * run.jokerCount;
        break;
      case RAP_SHEET:
        addMult = run.jokerCounter[j];
        break;
      case PAPER_SHREDDER:
        addChips = 2 * run.jokerCounter[j];
        break;
      case PALACE_GUARD:
        addMult = 4 * heldFaces;
        break;
      case MUSCLE:
        addChips = 60;
        break;
      default:
        break;
    }
    if (addChips > 0) {
      chips += addChips;
      out.fx[j] = {Fx::CHIPS, addChips};
    } else if (addMult > 0) {
      mult10 += addMult * 10;
      out.fx[j] = {Fx::MULT, addMult};
    } else if (times10 > 0) {
      mult10 = mult10 * times10 / 10;
      out.fx[j] = {Fx::XMULT, times10};
    }
  }
  out.chips = chips;
  out.mult10 = mult10;
  out.score = static_cast<int32_t>(std::min<int64_t>(INT32_MAX, static_cast<int64_t>(chips) * mult10 / 10));
}

bool playHand(Run& run, const uint8_t mask, Breakdown& out) {
  if (run.phase != Phase::ROUND || run.handsLeft == 0 || !validSelection(run, mask)) return false;
  scoreHand(run, mask, out);
  run.roundScore = static_cast<int32_t>(std::min<int64_t>(INT32_MAX, static_cast<int64_t>(run.roundScore) + out.score));
  run.bestHand = std::max(run.bestHand, out.score);
  run.handsLeft--;
  run.handsPlayed++;
  if (bossActive(run, TAXMAN)) run.money = std::max<int32_t>(0, run.money - 1);
  for (int j = 0; j < run.jokerCount; j++) {
    if (run.jokers[j] == RAP_SHEET && run.jokerCounter[j] < INT16_MAX) run.jokerCounter[j]++;
  }
  removeCards(run, mask);
  drawToHandSize(run);
  sortHand(run);
  return true;
}

bool discardCards(Run& run, const uint8_t mask) {
  if (run.phase != Phase::ROUND || run.discardsLeft == 0 || !validSelection(run, mask)) return false;
  int faces = 0;
  for (int i = 0; i < run.handCount; i++) {
    if ((mask & (1u << i)) && isFace(run.hand[i])) faces++;
  }
  const int n = popcount(mask);
  for (int j = 0; j < run.jokerCount; j++) {
    if (run.jokers[j] == SMUGGLER) run.money += faces;
    if (run.jokers[j] == PAPER_SHREDDER) {
      run.jokerCounter[j] = static_cast<int16_t>(std::min<int>(INT16_MAX, run.jokerCounter[j] + n));
    }
  }
  run.discardsLeft--;
  run.discardsUsed++;
  removeCards(run, mask);
  drawToHandSize(run);
  sortHand(run);
  return true;
}

bool roundWon(const Run& run) { return run.phase == Phase::ROUND && run.roundScore >= currentTarget(run); }

bool roundLost(const Run& run) { return run.phase == Phase::ROUND && !roundWon(run) && run.handsLeft == 0; }

Payout cashOut(Run& run) {
  Payout pay;
  pay.blind = blindReward(run.blind);
  pay.hands = run.handsLeft;
  pay.interest = std::min<int32_t>(MAX_INTEREST, std::max<int32_t>(0, run.money) / 5);
  for (int j = 0; j < run.jokerCount; j++) pay.jokers += run.jokers[j] == NEST_EGG ? 3 : 0;
  run.money += pay.total();
  if (run.blind == 2 && run.ante >= FINAL_ANTE) {
    run.won = true;
    run.phase = Phase::OVER;
    return pay;
  }
  run.phase = Phase::SHOP;
  run.rerollCost = REROLL_PRICE;
  rollShop(run);
  return pay;
}

void rollShop(Run& run) {
  for (auto& slot : run.shop) slot = EMPTY;
  for (int i = 0; i < SHOP_JOKERS; i++) run.shop[i] = rollJoker(run);
  run.shop[SHOP_JOKERS] = static_cast<uint8_t>(nextRandom(run.rng) % HAND_TYPES);
}

bool reroll(Run& run) {
  if (run.phase != Phase::SHOP || run.money < run.rerollCost) return false;
  run.money -= run.rerollCost;
  if (run.rerollCost < MAX_REROLL_PRICE) run.rerollCost++;
  rollShop(run);
  return true;
}

int shopPrice(const Run& run, const int slot) {
  if (slot < 0 || slot >= SHOP_SLOTS || run.shop[slot] == EMPTY) return 0;
  return slot < SHOP_JOKERS ? jokerPrice(run.shop[slot]) : PLANET_PRICE;
}

bool buy(Run& run, const int slot) {
  if (run.phase != Phase::SHOP || slot < 0 || slot >= SHOP_SLOTS || run.shop[slot] == EMPTY) return false;
  const int price = shopPrice(run, slot);
  if (run.money < price) return false;
  if (slot < SHOP_JOKERS) {
    if (run.jokerCount >= MAX_JOKERS) return false;
    run.jokers[run.jokerCount] = run.shop[slot];
    run.jokerCounter[run.jokerCount] = 0;
    run.jokerCount++;
  } else {
    uint8_t& level = run.levels[run.shop[slot]];
    level = static_cast<uint8_t>(std::min(MAX_LEVEL, level + 1));
  }
  run.money -= price;
  run.shop[slot] = EMPTY;
  return true;
}

bool sellJoker(Run& run, const int index) {
  if (index < 0 || index >= run.jokerCount) return false;
  run.money += jokerSellValue(run.jokers[index]);
  for (int i = index; i + 1 < run.jokerCount; i++) {
    run.jokers[i] = run.jokers[i + 1];
    run.jokerCounter[i] = run.jokerCounter[i + 1];
  }
  run.jokerCount--;
  return true;
}

void moveJoker(Run& run, const int from, const int to) {
  if (from < 0 || to < 0 || from >= run.jokerCount || to >= run.jokerCount || from == to) return;
  std::swap(run.jokers[from], run.jokers[to]);
  std::swap(run.jokerCounter[from], run.jokerCounter[to]);
}

void advanceBlind(Run& run) {
  if (run.blind < 2) {
    run.blind++;
  } else {
    run.ante++;
    run.blind = 0;
    pickBoss(run);
  }
  run.phase = Phase::BLIND_SELECT;
}

size_t serialize(const Run& run, uint8_t* out, const size_t size) {
  if (size < SAVE_BYTES) return 0;
  out[0] = SAVE_VERSION;
  memcpy(out + 1, &run, sizeof(Run));
  return SAVE_BYTES;
}

bool deserialize(Run& run, const uint8_t* in, const size_t size) {
  if (size != SAVE_BYTES || in[0] != SAVE_VERSION) return false;
  Run v;
  memcpy(&v, in + 1, sizeof(Run));
  const auto phase = static_cast<uint8_t>(v.phase);
  // Everything used as an index or count must be in range.
  bool ok = v.rng != 0 && phase <= static_cast<uint8_t>(Phase::OVER) && v.ante >= 1 && v.ante <= FINAL_ANTE &&
            v.blind <= 2 && v.boss < BOSS_KINDS && v.jokerCount <= MAX_JOKERS && v.deckPos <= DECK_SIZE &&
            v.handCount <= MAX_HAND && v.handsLeft <= START_HANDS && v.discardsLeft <= START_DISCARDS && v.money >= 0 &&
            v.roundScore >= 0;
  for (int i = 0; ok && i < v.jokerCount; i++) ok = v.jokers[i] < JOKER_KINDS;
  for (int i = 0; ok && i < HAND_TYPES; i++) ok = v.levels[i] >= 1 && v.levels[i] <= MAX_LEVEL;
  uint64_t seen = 0;
  for (int i = 0; ok && i < DECK_SIZE; i++) {
    ok = v.deck[i] < DECK_SIZE && !((seen >> v.deck[i]) & 1u);
    seen |= 1ull << v.deck[i];
  }
  // Hand cards must be distinct cards already dealt from the deck.
  uint64_t dealt = 0, held = 0;
  for (int i = 0; ok && i < v.deckPos; i++) dealt |= 1ull << v.deck[i];
  for (int i = 0; ok && i < v.handCount; i++) {
    ok = v.hand[i] < DECK_SIZE && ((dealt >> v.hand[i]) & 1u) && !((held >> v.hand[i]) & 1u);
    if (ok) held |= 1ull << v.hand[i];
  }
  for (int i = 0; ok && i < SHOP_SLOTS; i++)
    ok = v.shop[i] == EMPTY || v.shop[i] < (i < SHOP_JOKERS ? JOKER_KINDS : HAND_TYPES);
  if (ok) run = v;
  return ok;
}

}  // namespace jokers
