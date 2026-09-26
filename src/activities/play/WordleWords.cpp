#include "WordleWords.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstring>

#include "WordleFallback.h"

namespace wordle {
namespace {
constexpr int EN_SIZE = 26;
constexpr int TR_SIZE = 29;
constexpr int HEADER_BYTES = 8;
constexpr const char* LIST_PATHS[LANG_COUNT][2] = {
    {"/.crosspoint/games/wordle/en_answers.bin", "/.crosspoint/games/wordle/en_valid.bin"},
    {"/.crosspoint/games/wordle/tr_answers.bin", "/.crosspoint/games/wordle/tr_valid.bin"},
};
constexpr char LANG_TAGS[LANG_COUNT] = {'E', 'T'};

// Turkish alphabet order: a b c ç d e f g ğ h ı i j k l m n o ö p r s ş t u ü v y z.
constexpr uint16_t TR_UPPER[TR_SIZE] = {'A', 'B',   'C',   0xC7, 'D', 'E',  'F', 'G', 0x11E, 'H',
                                        'I', 0x130, 'J',   'K',  'L', 'M',  'N', 'O', 0xD6,  'P',
                                        'R', 'S',   0x15E, 'T',  'U', 0xDC, 'V', 'Y', 'Z'};
constexpr uint16_t TR_LOWER[TR_SIZE] = {'a',   'b', 'c',   0xE7, 'd', 'e',  'f', 'g', 0x11F, 'h',
                                        0x131, 'i', 'j',   'k',  'l', 'm',  'n', 'o', 0xF6,  'p',
                                        'r',   's', 0x15F, 't',  'u', 0xFC, 'v', 'y', 'z'};
// QWERTY, and the Turkish Q layout without Q, W and X (not Turkish letters).
constexpr uint8_t EN_ROW0[] = {16, 22, 4, 17, 19, 24, 20, 8, 14, 15};    // Q W E R T Y U I O P
constexpr uint8_t EN_ROW1[] = {0, 18, 3, 5, 6, 7, 9, 10, 11};            // A S D F G H J K L
constexpr uint8_t EN_ROW2[] = {25, 23, 2, 21, 1, 13, 12, KEY_ENTER};     // Z X C V B N M
constexpr uint8_t TR_ROW0[] = {5, 20, 23, 27, 24, 10, 17, 19, 8, 25};    // E R T Y U I O P Ğ Ü
constexpr uint8_t TR_ROW1[] = {0, 21, 4, 6, 7, 9, 12, 13, 14, 22, 11};   // A S D F G H J K L Ş İ
constexpr uint8_t TR_ROW2[] = {28, 2, 26, 1, 16, 15, 18, 3, KEY_ENTER};  // Z C V B N M Ö Ç
constexpr KeyRow KEYBOARDS[LANG_COUNT][KEY_ROWS] = {
    {{EN_ROW0, sizeof(EN_ROW0)}, {EN_ROW1, sizeof(EN_ROW1)}, {EN_ROW2, sizeof(EN_ROW2)}},
    {{TR_ROW0, sizeof(TR_ROW0)}, {TR_ROW1, sizeof(TR_ROW1)}, {TR_ROW2, sizeof(TR_ROW2)}},
};

const uint32_t* fallbackWords(const Lang lang, uint32_t& count) {
  if (lang == LANG_TR) {
    count = sizeof(FALLBACK_TR) / sizeof(FALLBACK_TR[0]);
    return FALLBACK_TR;
  }
  count = sizeof(FALLBACK_EN) / sizeof(FALLBACK_EN[0]);
  return FALLBACK_EN;
}

// Record count of a list file, or 0 if it is missing or malformed.
uint32_t readHeader(const char* path, const char lang, const char kind) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("WORDLE", path, file)) return 0;
  uint8_t header[HEADER_BYTES];
  if (file.read(header, HEADER_BYTES) != HEADER_BYTES || header[0] != 'W' || header[1] != 'D' || header[2] != lang ||
      header[3] != kind) {
    LOG_ERR("WORDLE", "Bad list header: %s", path);
    return 0;
  }
  uint32_t count;
  memcpy(&count, header + 4, sizeof(count));
  if (file.fileSize() != HEADER_BYTES + static_cast<size_t>(count) * 4) {
    LOG_ERR("WORDLE", "Truncated list: %s", path);
    return 0;
  }
  return count;
}

bool readRecord(HalFile& file, const uint32_t index, Word& out) {
  if (!file.seekSet(HEADER_BYTES + index * 4)) return false;
  uint8_t bytes[4];
  if (file.read(bytes, 4) != 4) return false;
  out = static_cast<Word>(bytes[0]) | static_cast<Word>(bytes[1]) << 8 | static_cast<Word>(bytes[2]) << 16 |
        static_cast<Word>(bytes[3]) << 24;
  return true;
}
}  // namespace

int alphabetSize(const Lang lang) { return lang == LANG_TR ? TR_SIZE : EN_SIZE; }

uint32_t letterCodepoint(const Lang lang, const uint8_t letter) {
  if (letter >= alphabetSize(lang)) return '?';
  return lang == LANG_TR ? TR_UPPER[letter] : 'A' + letter;
}

Word pack(const uint8_t* letters) {
  Word word = 0;
  for (int i = 0; i < WORD_LEN; i++) word = (word << 5) | (letters[i] & 31u);
  return word;
}

void unpack(Word word, uint8_t* letters) {
  for (int i = WORD_LEN - 1; i >= 0; i--) {
    letters[i] = static_cast<uint8_t>(word & 31u);
    word >>= 5;
  }
}

bool parseWord(const Lang lang, const char* utf8, uint8_t* letters) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(utf8);
  for (int i = 0; i < WORD_LEN; i++) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    int found = -1;
    for (int k = 0; k < alphabetSize(lang) && found < 0; k++) {
      const uint32_t upper = letterCodepoint(lang, static_cast<uint8_t>(k));
      const uint32_t lower = lang == LANG_TR ? TR_LOWER[k] : 'a' + k;
      if (cp == upper || cp == lower) found = k;
    }
    if (found < 0) return false;
    letters[i] = static_cast<uint8_t>(found);
  }
  return *cursor == '\0';
}

void score(const Word guess, const Word answer, uint8_t* marks) {
  uint8_t g[WORD_LEN], a[WORD_LEN];
  unpack(guess, g);
  unpack(answer, a);
  uint8_t unmatched[32] = {};
  for (int i = 0; i < WORD_LEN; i++) {
    marks[i] = g[i] == a[i] ? MARK_CORRECT : MARK_ABSENT;
    if (marks[i] != MARK_CORRECT) unmatched[a[i]]++;
  }
  for (int i = 0; i < WORD_LEN; i++) {
    if (marks[i] == MARK_CORRECT || unmatched[g[i]] == 0) continue;
    marks[i] = MARK_PRESENT;
    unmatched[g[i]]--;
  }
}

const KeyRow* keyboard(const Lang lang) { return KEYBOARDS[lang == LANG_TR ? 1 : 0]; }

void Dictionary::open(const Lang language) {
  lang = language;
  answers = readHeader(LIST_PATHS[lang][0], LANG_TAGS[lang], 'A');
  valid = readHeader(LIST_PATHS[lang][1], LANG_TAGS[lang], 'V');
  LOG_INF("WORDLE", "Lists %c: %lu answers, %lu guesses", LANG_TAGS[lang], static_cast<unsigned long>(answers),
          static_cast<unsigned long>(valid));
}

uint32_t Dictionary::answerCount() const {
  if (answers > 0) return answers;
  uint32_t count;
  fallbackWords(lang, count);
  return count;
}

Word Dictionary::answerAt(const uint32_t index) const {
  uint32_t count;
  const uint32_t* builtIn = fallbackWords(lang, count);
  if (answers > 0) {
    HalFile file;
    Word word;
    if (Storage.openFileForRead("WORDLE", LIST_PATHS[lang][0], file) && readRecord(file, index % answers, word)) {
      return word;
    }
    LOG_ERR("WORDLE", "Answer read failed, using built-in list");
  }
  return builtIn[index % count];
}

bool Dictionary::isValid(const Word word) const {
  if (valid == 0) return true;
  HalFile file;
  if (!Storage.openFileForRead("WORDLE", LIST_PATHS[lang][1], file)) return true;
  uint32_t low = 0, high = valid;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    Word probe;
    if (!readRecord(file, mid, probe)) return true;  // an unreadable card must not block play
    if (probe == word) return true;
    if (probe < word) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return false;
}

}  // namespace wordle
