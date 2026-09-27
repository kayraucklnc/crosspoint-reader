#pragma once

#include <cstdint>
#include <string>

// Book length estimates for the Library, read from cheap metadata only.
//
// An EPUB's length is the uncompressed size of its (X)HTML entries, summed from
// the ZIP central directory: no entry is inflated and no book is parsed. The
// result is cached in `/.crosspoint/library_lengths.bin` so each book is
// measured once:
//
//   [u32 magic 'LLN1'][u32 learned bytes-per-page, 0 = none]
//   { [u64 path hash][u32 file size][u32 content bytes] }*   (appended; later wins)
//
// Pages are content bytes divided by a bytes-per-page figure learned from the
// chapters the reader has actually laid out (see BookReadInfo), so estimates
// follow the reader's font and margins.
namespace library_length {

inline constexpr const char* CACHE_PATH = "/.crosspoint/library_lengths.bin";
// Used until the reader has laid out a chapter large enough to learn from.
inline constexpr uint32_t DEFAULT_BYTES_PER_PAGE = 900;

struct Entry {
  uint64_t pathHash;
  uint32_t fileSize;
  uint32_t contentBytes;
};

// Sum of the uncompressed (X)HTML entry sizes, 0 when the ZIP cannot be read.
uint32_t epubContentBytes(const std::string& path);

// Streams every cached entry to `visit`; returns the stored bytes-per-page (0
// when none was learned or the file is missing).
uint32_t load(void (*visit)(void* ctx, const Entry& entry), void* ctx);

bool append(const Entry* entries, int count);
bool storeBytesPerPage(uint32_t bytesPerPage);

}  // namespace library_length
