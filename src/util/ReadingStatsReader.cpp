#include "ReadingStatsReader.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace reading_stats {
namespace {
constexpr int MAX_DEPTH = 16;
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

// Byte-at-a-time reader over a 256-byte window of the file.
class Scanner {
 public:
  explicit Scanner(HalFile& file) : file(file) {}

  int peek() {
    if (pos >= len) {
      len = file.read(buf, sizeof(buf));
      pos = 0;
      if (len <= 0) {
        len = 0;
        return -1;
      }
    }
    return buf[pos];
  }
  int next() {
    const int c = peek();
    if (c >= 0) pos++;
    return c;
  }
  void skipWs() {
    for (int c = peek(); c == ' ' || c == '\n' || c == '\r' || c == '\t'; c = peek()) pos++;
  }
  bool expect(const char c) {
    skipWs();
    return next() == c;
  }

  // Reads a string (opening quote not yet consumed). Keeps a truncated copy in
  // `dst` when given, and folds the decoded UTF-8 bytes into `hash` when given.
  bool readString(char* dst, const size_t cap, uint64_t* hash) {
    skipWs();
    if (next() != '"') return false;
    size_t n = 0;
    uint64_t h = FNV_OFFSET;
    auto emit = [&](const uint8_t b) {
      if (dst && n + 1 < cap) dst[n++] = static_cast<char>(b);
      h = (h ^ b) * FNV_PRIME;
    };
    for (;;) {
      int c = next();
      if (c < 0) return false;
      if (c == '"') break;
      if (c != '\\') {
        emit(static_cast<uint8_t>(c));
        continue;
      }
      c = next();
      switch (c) {
        case 'n':
          emit('\n');
          break;
        case 't':
          emit('\t');
          break;
        case 'r':
          emit('\r');
          break;
        case 'b':
          emit('\b');
          break;
        case 'f':
          emit('\f');
          break;
        case 'u': {
          uint32_t cp = readHex4();
          if (cp >= 0xD800 && cp < 0xDC00 && next() == '\\' && next() == 'u') {
            const uint32_t low = readHex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          emitUtf8(cp, emit);
          break;
        }
        case -1:
          return false;
        default:
          emit(static_cast<uint8_t>(c));  // \" \\ \/
      }
    }
    if (dst && cap) dst[n] = '\0';
    if (hash) *hash = h;
    return true;
  }

  // Integer part of a number or literal; fractions and exponents are skipped.
  bool readScalar(int64_t& value, bool& isTrue) {
    skipWs();
    value = 0;
    isTrue = false;
    int c = peek();
    if (c == 't' || c == 'f' || c == 'n') {
      isTrue = c == 't';
      while (c >= 'a' && c <= 'z') {
        pos++;
        c = peek();
      }
      return true;
    }
    const bool negative = c == '-';
    if (negative) pos++;
    bool digits = false;
    for (c = peek(); c >= '0' && c <= '9'; c = peek()) {
      if (value < (INT64_MAX / 10)) value = value * 10 + (c - '0');
      digits = true;
      pos++;
    }
    for (c = peek(); c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-' || (c >= '0' && c <= '9'); c = peek()) {
      pos++;
    }
    if (negative) value = -value;
    return digits;
  }

  // Skips any value, iteratively so a hostile file cannot recurse the stack.
  bool skipValue() {
    skipWs();
    int depth = 0;
    do {
      skipWs();
      const int c = peek();
      if (c < 0) return false;
      if (c == '"') {
        if (!readString(nullptr, 0, nullptr)) return false;
      } else if (c == '{' || c == '[') {
        pos++;
        if (++depth > MAX_DEPTH) return false;
        continue;
      } else if (c == '}' || c == ']') {
        pos++;
        depth--;
      } else if (c == ',' || c == ':') {
        pos++;
        continue;
      } else {
        int64_t v;
        bool t;
        if (!readScalar(v, t) && peek() == c) return false;  // no progress: garbage byte
      }
    } while (depth > 0);
    return true;
  }

 private:
  uint32_t readHex4() {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      const int c = next();
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= c - '0';
      else if (c >= 'a' && c <= 'f')
        v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v |= c - 'A' + 10;
    }
    return v;
  }
  template <typename Emit>
  static void emitUtf8(const uint32_t cp, Emit& emit) {
    if (cp < 0x80) {
      emit(static_cast<uint8_t>(cp));
    } else if (cp < 0x800) {
      emit(static_cast<uint8_t>(0xC0 | (cp >> 6)));
      emit(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      emit(static_cast<uint8_t>(0xE0 | (cp >> 12)));
      emit(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
      emit(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    } else {
      emit(static_cast<uint8_t>(0xF0 | (cp >> 18)));
      emit(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
      emit(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
      emit(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
    }
  }

  HalFile& file;
  uint8_t buf[256];
  int pos = 0;
  int len = 0;
};

uint32_t clampU32(const int64_t v) { return v < 0 ? 0 : (v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v)); }

bool readBook(Scanner& in, BookStats& book) {
  if (!in.expect('{')) return false;
  in.skipWs();
  if (in.peek() == '}') return in.next() == '}';
  for (;;) {
    char key[12];
    if (!in.readString(key, sizeof(key), nullptr) || !in.expect(':')) return false;
    in.skipWs();
    const int c = in.peek();
    if (c == '{' || c == '[' || c == '"') {
      if (!in.skipValue()) return false;
    } else {
      int64_t v;
      bool isTrue;
      in.readScalar(v, isTrue);
      if (strcmp(key, "sec") == 0) book.seconds = clampU32(v);
      if (strcmp(key, "last") == 0) book.lastRead = clampU32(v);
      if (strcmp(key, "sessions") == 0) book.sessions = static_cast<uint16_t>(clampU32(v) > 0xFFFF ? 0xFFFF : v);
      if (strcmp(key, "finished") == 0) book.finished = isTrue;
    }
    in.skipWs();
    const int sep = in.next();
    if (sep == '}') return true;
    if (sep != ',') return false;
  }
}

bool readBooks(Scanner& in, std::vector<BookStats>& out) {
  if (!in.expect('{')) return false;
  in.skipWs();
  if (in.peek() == '}') return in.next() == '}';
  for (;;) {
    BookStats book{};
    if (!in.readString(nullptr, 0, &book.pathHash) || !in.expect(':') || !readBook(in, book)) return false;
    if (out.size() < MAX_BOOKS) out.push_back(book);
    in.skipWs();
    const int sep = in.next();
    if (sep == '}') return true;
    if (sep != ',') return false;
  }
}
}  // namespace

bool load(std::vector<BookStats>& out) {
  out.clear();
  HalFile file;
  if (!Storage.exists(STATS_PATH) || !Storage.openFileForRead("STATS", STATS_PATH, file)) return false;
  // One reservation sized from the file: a book entry is at least ~90 bytes.
  out.reserve(std::min<size_t>(MAX_BOOKS, file.fileSize() / 90 + 1));
  Scanner in(file);
  if (!in.expect('{')) {
    LOG_ERR("STATS", "reading_stats.json is not an object");
    return false;
  }
  in.skipWs();
  if (in.peek() == '}') return true;
  for (;;) {
    char key[8];
    if (!in.readString(key, sizeof(key), nullptr) || !in.expect(':')) break;
    const bool ok = strcmp(key, "books") == 0 ? readBooks(in, out) : in.skipValue();
    if (!ok) break;
    in.skipWs();
    const int sep = in.next();
    if (sep == '}') return true;
    if (sep != ',') break;
  }
  // Keep whatever parsed cleanly: a torn write should cost the tail, not everything.
  LOG_ERR("STATS", "reading_stats.json malformed; using %d parsed books", static_cast<int>(out.size()));
  return !out.empty();
}

const BookStats* find(const std::vector<BookStats>& stats, const uint64_t pathHash) {
  for (const BookStats& s : stats) {
    if (s.pathHash == pathHash) return &s;
  }
  return nullptr;
}

}  // namespace reading_stats
