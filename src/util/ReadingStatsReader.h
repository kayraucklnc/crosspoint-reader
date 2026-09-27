#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Read-only view of `/.crosspoint/reading_stats.json`:
//   {"v":1,"books":{"<sd path>":{"sec":int,"pages":int,"sessions":int,
//    "first":epoch,"last":epoch,"finished":bool,"wpm":int}},"days":{...}}
//
// A small streaming scanner instead of a JsonDocument: the file grows with the
// reading history ("days"), and only four fields per book are needed. Paths are
// kept as their FNV-1a 64 hash (library::clixPathHash), so the resident cost is
// 24 bytes per book and nothing proportional to the path lengths.
namespace reading_stats {

struct BookStats {
  uint64_t pathHash;
  uint32_t seconds;
  uint32_t lastRead;  // epoch seconds, 0 when unknown
  uint16_t sessions;
  bool finished;
};

inline constexpr const char* STATS_PATH = "/.crosspoint/reading_stats.json";
inline constexpr size_t MAX_BOOKS = 1024;

// Replaces `out` with every book in the file. A missing file is not an error:
// it returns false with `out` empty, and callers simply show no stats.
bool load(std::vector<BookStats>& out);

const BookStats* find(const std::vector<BookStats>& stats, uint64_t pathHash);

}  // namespace reading_stats
