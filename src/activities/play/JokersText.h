#pragma once

#include <cstddef>
#include <cstdint>

// Translated names for Noir Jokers content, indexed like JokersRules enums.
namespace jokers::text {

const char* handName(uint8_t hand);
const char* planetName(uint8_t hand);
const char* jokerName(uint8_t joker);
// Fills `out` with the joker's rule text; counters show a scaling joker's current bonus.
void jokerDesc(uint8_t joker, int counter, char* out, size_t size);
const char* bossName(uint8_t boss);
const char* bossDesc(uint8_t boss);
const char* blindName(int blind, uint8_t boss);
// 1234567 -> "1,234,567".
void number(int32_t value, char* out, size_t size);

}  // namespace jokers::text
