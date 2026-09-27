#include "LibraryLengthCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace library_length {
namespace {
constexpr uint32_t MAGIC = 0x314E4C4C;  // "LLN1"
constexpr size_t HEADER_BYTES = 8;
constexpr size_t ENTRY_BYTES = 16;
static_assert(sizeof(Entry) == ENTRY_BYTES, "cache entries are stored raw");
// Past this many entries (books replaced many times over) the file starts over.
constexpr size_t MAX_FILE_BYTES = HEADER_BYTES + ENTRY_BYTES * 8192;

constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint32_t CDIR_SIG = 0x02014b50;
constexpr size_t EOCD_BYTES = 22;
constexpr size_t CDIR_BYTES = 46;
constexpr size_t SCAN_BUFFER = 512;

uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool isMarkup(const char* name, const size_t len) {
  const auto endsWith = [&](const char* suffix) {
    const size_t n = strlen(suffix);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++) {
      char c = name[len - n + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != suffix[i]) return false;
    }
    return true;
  };
  return endsWith(".xhtml") || endsWith(".html") || endsWith(".htm");
}

bool writeHeader(HalFile& file, const uint32_t bytesPerPage) {
  uint8_t header[HEADER_BYTES];
  memcpy(header, &MAGIC, 4);
  memcpy(header + 4, &bytesPerPage, 4);
  return file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
}

// Opens the cache for writing, starting a fresh file when it is missing,
// foreign or oversized.
bool openForUpdate(HalFile& file) {
  file = Storage.open(CACHE_PATH, O_RDWR | O_CREAT);
  if (!file) {
    LOG_ERR("LLEN", "cannot open %s", CACHE_PATH);
    return false;
  }
  uint8_t header[HEADER_BYTES]{};
  const size_t size = file.fileSize();
  const bool valid = size >= HEADER_BYTES && size <= MAX_FILE_BYTES &&
                     file.read(header, HEADER_BYTES) == static_cast<int>(HEADER_BYTES) && le32(header) == MAGIC;
  if (valid) return true;
  file.close();
  file = Storage.open(CACHE_PATH, O_RDWR | O_CREAT | O_TRUNC);
  return file && writeHeader(file, 0);
}
}  // namespace

uint32_t epubContentBytes(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("LLEN", path, file)) return 0;
  const size_t size = file.fileSize();
  if (size < EOCD_BYTES) return 0;
  auto buf = makeUniqueNoThrow<uint8_t[]>(SCAN_BUFFER);
  if (!buf) {
    LOG_ERR("LLEN", "OOM: zip scan buffer");
    return 0;
  }

  // The end-of-central-directory record sits in the last bytes unless the
  // archive carries a long comment, which EPUB writers do not add.
  const size_t tail = std::min(size, SCAN_BUFFER);
  if (!file.seek(size - tail) || file.read(buf.get(), tail) != static_cast<int>(tail)) return 0;
  const uint8_t* eocd = nullptr;
  for (size_t i = tail - EOCD_BYTES + 1; i-- > 0;) {
    if (le32(buf.get() + i) == EOCD_SIG) {
      eocd = buf.get() + i;
      break;
    }
  }
  if (!eocd) return 0;
  const uint16_t entries = le16(eocd + 10);
  const uint32_t dirSize = le32(eocd + 12);
  const uint32_t dirOffset = le32(eocd + 16);
  if (static_cast<uint64_t>(dirOffset) + dirSize > size || !file.seek(dirOffset)) return 0;

  uint64_t total = 0;
  uint8_t* header = buf.get();
  char* name = reinterpret_cast<char*>(buf.get() + CDIR_BYTES);
  constexpr size_t NAME_CAP = SCAN_BUFFER - CDIR_BYTES;
  for (uint16_t e = 0; e < entries; e++) {
    if (file.read(header, CDIR_BYTES) != static_cast<int>(CDIR_BYTES) || le32(header) != CDIR_SIG) break;
    const uint32_t inflated = le32(header + 24);
    const uint16_t nameLen = le16(header + 28);
    const uint32_t skip = static_cast<uint32_t>(le16(header + 30)) + le16(header + 32);
    if (nameLen > NAME_CAP) {
      if (!file.seekCur(static_cast<int64_t>(nameLen) + skip)) break;
      continue;
    }
    if (file.read(name, nameLen) != nameLen) break;
    if (isMarkup(name, nameLen)) total += inflated;
    if (skip > 0 && !file.seekCur(skip)) break;
  }
  return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

uint32_t load(void (*visit)(void* ctx, const Entry& entry), void* ctx) {
  HalFile file;
  if (!Storage.exists(CACHE_PATH) || !Storage.openFileForRead("LLEN", CACHE_PATH, file)) return 0;
  uint8_t chunk[ENTRY_BYTES * 8];
  if (file.read(chunk, HEADER_BYTES) != static_cast<int>(HEADER_BYTES) || le32(chunk) != MAGIC) return 0;
  const uint32_t bytesPerPage = le32(chunk + 4);
  for (;;) {
    const int got = file.read(chunk, sizeof(chunk));
    if (got <= 0) break;
    for (int off = 0; off + static_cast<int>(ENTRY_BYTES) <= got; off += ENTRY_BYTES) {
      Entry entry;
      memcpy(&entry, chunk + off, ENTRY_BYTES);
      visit(ctx, entry);
    }
    if (got < static_cast<int>(sizeof(chunk))) break;
  }
  return bytesPerPage;
}

bool append(const Entry* entries, const int count) {
  if (count <= 0) return true;
  HalFile file;
  if (!openForUpdate(file) || !file.seek(file.fileSize())) return false;
  const size_t bytes = static_cast<size_t>(count) * ENTRY_BYTES;
  if (file.write(reinterpret_cast<const uint8_t*>(entries), bytes) != bytes) {
    LOG_ERR("LLEN", "cache append failed");
    return false;
  }
  return true;
}

bool storeBytesPerPage(const uint32_t bytesPerPage) {
  HalFile file;
  if (!openForUpdate(file)) return false;
  if (!writeHeader(file, bytesPerPage)) {
    LOG_ERR("LLEN", "cache header write failed");
    return false;
  }
  return true;
}

}  // namespace library_length
