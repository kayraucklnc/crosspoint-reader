#pragma once

#include <cstdint>
#include <string>

// What the reader's own caches say about a book, read without building any
// cache: progress plus the facts a page estimate needs.
struct BookReadInfo {
  int progress = -1;          // saved reading percentage, -1 when unavailable
  uint32_t contentBytes = 0;  // EPUB spine bytes from book.bin, 0 unknown
  uint32_t exactPages = 0;    // XTC page count or a cached TXT pagination, 0 unknown
  // The chapter being read: its spine bytes and laid-out page count, a sample of
  // bytes-per-page under the current reader settings (both 0 when unknown).
  uint32_t sampleBytes = 0;
  uint16_t samplePages = 0;
};

// Fills `out` from the book's cache dir. False when the book has no usable cache.
bool loadBookReadInfo(const std::string& path, BookReadInfo& out);

// Saved reading percentage, or -1 when unavailable. Never builds reading caches.
int loadBookProgress(const std::string& path);
