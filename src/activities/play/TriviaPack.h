#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

// The on-card Trivia pack (/.crosspoint/games/trivia/questions.bin), built by
// scripts/gen_trivia.py from the Open Trivia Database; the layout is in that
// script. Reading follows CrossPlay's trivia pack (MIT, Copyright (c) 2026
// Mario Ruiz): an index of offsets, one record read per question, and a record
// that would overflow is rejected rather than truncated.
//
// The one-byte-per-question meta table (difficulty, category, true/false) is
// held in RAM (~5 KB for the shipped 5,286 questions) because every pick scans
// it with the player's filters; doing that scan on the card would be thousands
// of SD reads per question.
namespace trivia {

constexpr int MAX_RECORD = 512;
constexpr int MAX_OPTIONS = 4;
constexpr int MAX_CATEGORIES = 32;
constexpr int CATEGORY_NAME_LEN = 28;
constexpr int DIFFICULTIES = 3;  // easy, medium, hard
constexpr uint8_t ANY_DIFFICULTY = 3;
constexpr uint8_t ALL_CATEGORIES = 0xFF;

inline int metaDifficulty(const uint8_t meta) { return meta & 3; }
inline bool metaBoolean(const uint8_t meta) { return (meta & 4) != 0; }
inline int metaCategory(const uint8_t meta) { return meta >> 3; }

struct Question {
  uint8_t meta = 0;
  int wrongCount = 0;
  const char* text = "";
  const char* answer = "";
  const char* wrong[MAX_OPTIONS - 1] = {};
  char bytes[MAX_RECORD + MAX_OPTIONS + 2] = {};  // the record's strings, each null-terminated
};

class Pack {
 public:
  static constexpr const char* PATH = "/.crosspoint/games/trivia/questions.bin";

  bool open();
  void close();
  bool isOpen() const { return count_ > 0; }
  int count() const { return count_; }
  uint8_t meta(const int index) const { return index >= 0 && index < count_ ? meta_[index] : 0; }
  int categoryCount() const { return categories_; }
  const char* categoryName(int category) const;
  // Questions matching the filters (ANY_DIFFICULTY / ALL_CATEGORIES widen them).
  int matching(uint8_t difficulty, uint8_t category) const;
  bool read(int index, Question& out);

 private:
  HalFile file;
  std::unique_ptr<uint8_t[]> meta_;
  int count_ = 0;
  uint32_t indexAt_ = 0;
  uint32_t blobAt_ = 0;
  int categories_ = 0;
  char names_[MAX_CATEGORIES][CATEGORY_NAME_LEN] = {};
};

// xorshift32: small and seedable, so a day's questions are the same all day.
class Rng {
 public:
  explicit Rng(const uint32_t seed = 1) : state(seed ? seed : 1) {}
  void seed(const uint32_t s) { state = s ? s : 1; }
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  uint32_t below(const uint32_t bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  uint32_t state;
};

bool matches(uint8_t meta, uint8_t difficulty, uint8_t category);

}  // namespace trivia
