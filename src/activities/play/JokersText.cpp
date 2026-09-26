#include "JokersText.h"

#include <I18n.h>

#include <cstdio>

#include "JokersRules.h"

namespace jokers::text {
namespace {
constexpr StrId HANDS[HAND_TYPES] = {StrId::STR_JK_HAND_HIGH,  StrId::STR_JK_HAND_PAIR,     StrId::STR_JK_HAND_TWO_PAIR,
                                     StrId::STR_JK_HAND_THREE, StrId::STR_JK_HAND_STRAIGHT, StrId::STR_JK_HAND_FLUSH,
                                     StrId::STR_JK_HAND_FULL,  StrId::STR_JK_HAND_FOUR,     StrId::STR_JK_HAND_SF};
constexpr StrId PLANETS[HAND_TYPES] = {
    StrId::STR_JK_PLANET_IO,     StrId::STR_JK_PLANET_EUROPA,   StrId::STR_JK_PLANET_TITAN,
    StrId::STR_JK_PLANET_TRITON, StrId::STR_JK_PLANET_CALLISTO, StrId::STR_JK_PLANET_GANYMEDE,
    StrId::STR_JK_PLANET_OBERON, StrId::STR_JK_PLANET_PHOBOS,   StrId::STR_JK_PLANET_CHARON};
constexpr StrId JOKER_NAMES[JOKER_KINDS] = {
    StrId::STR_JK_J_WISECRACK, StrId::STR_JK_J_HEARTBREAKER, StrId::STR_JK_J_GRAVEDIGGER, StrId::STR_JK_J_JEWELER,
    StrId::STR_JK_J_BRAWLER,   StrId::STR_JK_J_TWINS,        StrId::STR_JK_J_TRIPLE,      StrId::STR_JK_J_GETAWAY,
    StrId::STR_JK_J_FEMME,     StrId::STR_JK_J_DOUBLE,       StrId::STR_JK_J_FAMILY,      StrId::STR_JK_J_FUSE,
    StrId::STR_JK_J_MUGSHOT,   StrId::STR_JK_J_SEVEN,        StrId::STR_JK_J_ACE,         StrId::STR_JK_J_LAST_CALL,
    StrId::STR_JK_J_CLEAN,     StrId::STR_JK_J_ROLLER,       StrId::STR_JK_J_GANG,        StrId::STR_JK_J_RAP,
    StrId::STR_JK_J_SHREDDER,  StrId::STR_JK_J_GUARD,        StrId::STR_JK_J_NEST,        StrId::STR_JK_J_SMUGGLER,
    StrId::STR_JK_J_MUSCLE};
constexpr StrId JOKER_DESCS[JOKER_KINDS] = {
    StrId::STR_JK_D_WISECRACK, StrId::STR_JK_D_HEARTBREAKER, StrId::STR_JK_D_GRAVEDIGGER, StrId::STR_JK_D_JEWELER,
    StrId::STR_JK_D_BRAWLER,   StrId::STR_JK_D_TWINS,        StrId::STR_JK_D_TRIPLE,      StrId::STR_JK_D_GETAWAY,
    StrId::STR_JK_D_FEMME,     StrId::STR_JK_D_DOUBLE,       StrId::STR_JK_D_FAMILY,      StrId::STR_JK_D_FUSE,
    StrId::STR_JK_D_MUGSHOT,   StrId::STR_JK_D_SEVEN,        StrId::STR_JK_D_ACE,         StrId::STR_JK_D_LAST_CALL,
    StrId::STR_JK_D_CLEAN,     StrId::STR_JK_D_ROLLER,       StrId::STR_JK_D_GANG,        StrId::STR_JK_D_RAP,
    StrId::STR_JK_D_SHREDDER,  StrId::STR_JK_D_GUARD,        StrId::STR_JK_D_NEST,        StrId::STR_JK_D_SMUGGLER,
    StrId::STR_JK_D_MUSCLE};
constexpr StrId BOSS_NAMES[BOSS_KINDS] = {
    StrId::STR_JK_B_WIDOW,   StrId::STR_JK_B_UNDERTAKER, StrId::STR_JK_B_FENCE, StrId::STR_JK_B_BOOTLEGGER,
    StrId::STR_JK_B_MISER,   StrId::STR_JK_B_CENSOR,     StrId::STR_JK_B_MUTE,  StrId::STR_JK_B_SYNDICATE,
    StrId::STR_JK_B_SQUEEZE, StrId::STR_JK_B_TAXMAN};
constexpr StrId BOSS_DESCS[BOSS_KINDS] = {
    StrId::STR_JK_BD_WIDOW,   StrId::STR_JK_BD_UNDERTAKER, StrId::STR_JK_BD_FENCE, StrId::STR_JK_BD_BOOTLEGGER,
    StrId::STR_JK_BD_MISER,   StrId::STR_JK_BD_CENSOR,     StrId::STR_JK_BD_MUTE,  StrId::STR_JK_BD_SYNDICATE,
    StrId::STR_JK_BD_SQUEEZE, StrId::STR_JK_BD_TAXMAN};
}  // namespace

const char* handName(const uint8_t hand) { return I18N.get(HANDS[hand % HAND_TYPES]); }
const char* planetName(const uint8_t hand) { return I18N.get(PLANETS[hand % HAND_TYPES]); }
const char* jokerName(const uint8_t joker) { return I18N.get(JOKER_NAMES[joker % JOKER_KINDS]); }
const char* bossName(const uint8_t boss) { return I18N.get(BOSS_NAMES[boss % BOSS_KINDS]); }
const char* bossDesc(const uint8_t boss) { return I18N.get(BOSS_DESCS[boss % BOSS_KINDS]); }

void jokerDesc(const uint8_t joker, const int counter, char* out, const size_t size) {
  const char* format = I18N.get(JOKER_DESCS[joker % JOKER_KINDS]);
  const int bonus = joker == PAPER_SHREDDER ? counter * 2 : counter;
  // Only the scaling jokers carry a %d.
  if (joker == RAP_SHEET || joker == PAPER_SHREDDER) {
    snprintf(out, size, format, bonus);
  } else {
    snprintf(out, size, "%s", format);
  }
}

const char* blindName(const int blind, const uint8_t boss) {
  if (blind == 0) return tr(STR_JK_SMALL_BLIND);
  if (blind == 1) return tr(STR_JK_BIG_BLIND);
  return bossName(boss);
}

void number(const int32_t value, char* out, const size_t size) {
  char digits[16];
  const bool negative = value < 0;
  snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(negative ? -static_cast<int64_t>(value) : value));
  size_t len = 0;
  for (const char* p = digits; *p; p++) len++;
  size_t o = 0;
  if (negative && o + 1 < size) out[o++] = '-';
  for (size_t i = 0; i < len && o + 1 < size; i++) {
    if (i > 0 && (len - i) % 3 == 0 && o + 1 < size) out[o++] = ',';
    if (o + 1 < size) out[o++] = digits[i];
  }
  if (size) out[o < size ? o : size - 1] = '\0';
}

}  // namespace jokers::text
