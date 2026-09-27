#pragma once

// Ported from CrossPlay (https://github.com/ma-r-s/crossplay, MIT,
// Copyright (c) 2026 Mario Ruiz).

// The on-card puzzle pack: how the puzzles live on the SD card and how one is
// read back without holding the rest in RAM. Written on a computer by
// scripts/gen_connections.py (English) and scripts/gen_connections_tr.py
// (Turkish); the device only reads.
//
//   <name>.idx   magic, version, then one 8-byte entry per puzzle
//                (date or number, offset into the .dat)
//   <name>.dat   the puzzle records themselves, back to back
//
// The puzzle count falls out of the index file's size. Freestanding: I/O
// arrives as a function pointer; the device passes HalFile.

#include <cstdint>

#include "ConnectionsCore.h"

namespace connections {

// "CXP1". Bumped if the record layout changes, so an old pack is refused
// rather than misread.
constexpr uint32_t kPackMagic = 0x31505843;
constexpr uint16_t kPackVersion = 1;
constexpr uint32_t kIndexHeaderSize = 8;
constexpr uint32_t kIndexEntrySize = 8;

// Random-access reads over the index and data files. Returns false on a short
// or failed read; a truncated pack must fail loudly rather than deal a puzzle
// with half its words.
using ReadFn = bool (*)(void* ctx, uint32_t offset, void* dst, uint32_t len);

class PackReader {
 public:
  // `indexSize` is the byte size of the index file, which is where the puzzle
  // count comes from.
  bool open(ReadFn read, void* indexCtx, void* dataCtx, uint32_t indexSize);

  bool isOpen() const { return count_ > 0; }
  int count() const { return count_; }

  bool readPuzzle(int index, Puzzle& out) const;

 private:
  ReadFn read_ = nullptr;
  void* indexCtx_ = nullptr;
  void* dataCtx_ = nullptr;
  int count_ = 0;
};

}  // namespace connections
