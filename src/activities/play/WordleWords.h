#pragma once

#include <cstdint>

// Wordle alphabets, keyboards, scoring and the SD word lists.
//
// Letters are indices into the language's lowercase alphabet (EN a..z, TR
// "abcçdefgğhıijklmnoöprsştuüvyz"). A word packs its five indices 5 bits each,
// first letter highest, so numeric order is alphabetical order. Lists live on
// SD under /.crosspoint/games/wordle/ (see scripts/gen_wordle_words.py) and are
// binary-searched in place; without them a small built-in answer set is used
// and any five letters are accepted.
namespace wordle {

constexpr int WORD_LEN = 5;
constexpr int MAX_GUESSES = 6;
constexpr uint8_t KEY_ENTER = 0xFE;
constexpr int KEY_ROWS = 3;

enum Lang : uint8_t { LANG_EN = 0, LANG_TR = 1, LANG_COUNT = 2 };
enum Mark : uint8_t { MARK_NONE = 0, MARK_ABSENT = 1, MARK_PRESENT = 2, MARK_CORRECT = 3 };

using Word = uint32_t;

// Uppercase codepoint for a letter index (Turkish i -> U+0130 İ, ı -> I).
uint32_t letterCodepoint(Lang lang, uint8_t letter);
int alphabetSize(Lang lang);

Word pack(const uint8_t* letters);
void unpack(Word word, uint8_t* letters);
// Parses five letters of UTF-8 (either case) in the language's alphabet.
bool parseWord(Lang lang, const char* utf8, uint8_t* letters);
// Standard two-pass Wordle marks: exact matches first, then present letters
// limited by how many of each remain unmatched in the answer.
void score(Word guess, Word answer, uint8_t* marks);

struct KeyRow {
  const uint8_t* keys;  // letter indices, KEY_ENTER for the submit key
  uint8_t count;
};
const KeyRow* keyboard(Lang lang);  // KEY_ROWS rows

class Dictionary {
 public:
  void open(Lang lang);
  Lang language() const { return lang; }
  uint32_t answerCount() const;
  Word answerAt(uint32_t index) const;
  bool isValid(Word word) const;
  bool hasLists() const { return answers > 0 && valid > 0; }

 private:
  Lang lang = LANG_EN;
  uint32_t answers = 0;  // 0: built-in fallback
  uint32_t valid = 0;    // 0: accept any word
};

}  // namespace wordle
