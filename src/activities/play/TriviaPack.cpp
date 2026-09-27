#include "TriviaPack.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace trivia {
namespace {
constexpr uint8_t MAGIC[4] = {'T', 'R', 'V', '1'};
constexpr uint16_t VERSION = 1;
constexpr int HEADER = 16;
constexpr int MAX_QUESTIONS = 20000;

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

bool matches(const uint8_t meta, const uint8_t difficulty, const uint8_t category) {
  if (difficulty != ANY_DIFFICULTY && metaDifficulty(meta) != difficulty) return false;
  return category == ALL_CATEGORIES || metaCategory(meta) == category;
}

bool Pack::open() {
  close();
  if (!Storage.exists(PATH) || !Storage.openFileForRead("TRIVIA", PATH, file)) return false;
  uint8_t header[HEADER];
  if (file.read(header, HEADER) != HEADER || memcmp(header, MAGIC, 4) != 0 || u16(header + 4) != VERSION) {
    LOG_ERR("TRIVIA", "Not a trivia pack: %s", PATH);
    close();
    return false;
  }
  const int count = static_cast<int>(u32(header + 8));
  const int categories = header[6];
  const uint32_t tableAt = u32(header + 12);
  const uint32_t size = file.fileSize();
  if (count <= 0 || count > MAX_QUESTIONS || categories > MAX_CATEGORIES || tableAt > size ||
      HEADER + static_cast<uint32_t>(count) * 5u + 4u > tableAt) {
    LOG_ERR("TRIVIA", "Corrupt pack header");
    close();
    return false;
  }
  meta_ = makeUniqueNoThrow<uint8_t[]>(count);
  if (!meta_) {
    LOG_ERR("TRIVIA", "OOM: meta %d bytes", count);
    close();
    return false;
  }
  if (file.read(meta_.get(), count) != count) {
    LOG_ERR("TRIVIA", "Truncated meta table");
    close();
    return false;
  }
  // Category names and sizes, read once.
  if (!file.seek(tableAt)) {
    close();
    return false;
  }
  for (int c = 0; c < categories; c++) {
    uint8_t length = 0;
    if (file.read(&length, 1) != 1) break;
    const int kept = length < CATEGORY_NAME_LEN - 1 ? length : CATEGORY_NAME_LEN - 1;
    // Skip the rest of an over-long name and the u16 question count.
    if (file.read(names_[c], kept) != kept || !file.seek(file.position() + (length - kept) + 2)) {
      LOG_ERR("TRIVIA", "Truncated category table");
      close();
      return false;
    }
    names_[c][kept] = '\0';
  }
  categories_ = categories;
  count_ = count;
  indexAt_ = HEADER + static_cast<uint32_t>(count);
  blobAt_ = indexAt_ + static_cast<uint32_t>(count + 1) * 4u;
  return true;
}

void Pack::close() {
  file.close();
  meta_.reset();
  count_ = 0;
  categories_ = 0;
}

const char* Pack::categoryName(const int category) const {
  return category >= 0 && category < categories_ ? names_[category] : "";
}

int Pack::matching(const uint8_t difficulty, const uint8_t category) const {
  int n = 0;
  for (int i = 0; i < count_; i++) n += matches(meta_[i], difficulty, category) ? 1 : 0;
  return n;
}

bool Pack::read(const int index, Question& out) {
  if (index < 0 || index >= count_) return false;
  uint8_t pair[8];
  if (!file.seek(indexAt_ + static_cast<uint32_t>(index) * 4u) || file.read(pair, 8) != 8) return false;
  const uint32_t start = u32(pair), end = u32(pair + 4);
  if (end <= start || end - start > MAX_RECORD) return false;  // reject, never truncate
  const int length = static_cast<int>(end - start);
  out = Question{};
  // The record is read into the question's own buffer and unpacked in place:
  // each length prefix (1-2 bytes) becomes the string's terminator, so the
  // write position never passes the read position.
  auto* record = reinterpret_cast<uint8_t*>(out.bytes);
  if (!file.seek(blobAt_ + start) || file.read(record, length) != length) return false;
  out.meta = meta_[index];
  out.wrongCount = record[0];
  if (out.wrongCount < 1 || out.wrongCount > MAX_OPTIONS - 1) return false;
  int at = 1;
  int written = 0;
  const auto take = [&](const int lengthBytes, const char*& field) {
    if (at + lengthBytes > length) return false;
    const int n = lengthBytes == 2 ? u16(record + at) : record[at];
    at += lengthBytes;
    if (at + n > length) return false;
    memmove(out.bytes + written, record + at, n);
    out.bytes[written + n] = '\0';
    field = out.bytes + written;
    written += n + 1;
    at += n;
    return true;
  };
  if (!take(2, out.text) || !take(1, out.answer)) return false;
  for (int i = 0; i < out.wrongCount; i++) {
    if (!take(1, out.wrong[i])) return false;
  }
  return true;
}

}  // namespace trivia
