// Ported from CrossPlay (https://github.com/ma-r-s/crossplay, MIT,
// Copyright (c) 2026 Mario Ruiz).

#include "ConnectionsPack.h"

#include <cstring>

namespace connections {

namespace {

// Little-endian by hand rather than memcpy of a struct: the pack is written on
// one machine and read on another, and RISC-V faults on unaligned wide reads.
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

// --- reading ---------------------------------------------------------------

bool PackReader::open(const ReadFn read, void* indexCtx, void* dataCtx, const uint32_t indexSize) {
  read_ = nullptr;
  count_ = 0;
  if (read == nullptr || indexSize < kIndexHeaderSize) return false;

  uint8_t header[kIndexHeaderSize];
  if (!read(indexCtx, 0, header, kIndexHeaderSize)) return false;
  if (getU32(header) != kPackMagic) return false;
  if (getU16(header + 4) != kPackVersion) return false;

  const uint32_t body = indexSize - kIndexHeaderSize;
  // A partial entry means the import was interrupted. Refuse the pack rather
  // than serve a puzzle whose offset was never written.
  if (body % kIndexEntrySize != 0 || body == 0) return false;

  read_ = read;
  indexCtx_ = indexCtx;
  dataCtx_ = dataCtx;
  count_ = static_cast<int>(body / kIndexEntrySize);
  return true;
}

bool PackReader::readPuzzle(const int index, Puzzle& out) const {
  if (read_ == nullptr || index < 0 || index >= count_) return false;

  uint8_t entry[kIndexEntrySize];
  const uint32_t indexOffset = kIndexHeaderSize + static_cast<uint32_t>(index) * kIndexEntrySize;
  if (!read_(indexCtx_, indexOffset, entry, kIndexEntrySize)) return false;
  uint32_t at = getU32(entry + 4);

  uint8_t head[6];
  if (!read_(dataCtx_, at, head, sizeof(head))) return false;
  at += sizeof(head);
  out = Puzzle{};
  out.id = getU16(head);
  out.date = getU32(head + 2);

  // The date is stored in both files. They are written together, so a mismatch
  // means the pack is corrupt in a way that would otherwise deal the wrong
  // puzzle for a date, which is worse than refusing to deal one.
  if (out.date != getU32(entry)) return false;

  for (int g = 0; g < kGroups; ++g) {
    uint8_t level = 0;
    if (!read_(dataCtx_, at, &level, 1)) return false;
    ++at;
    out.groups[g].level = level;

    uint8_t len = 0;
    if (!read_(dataCtx_, at, &len, 1)) return false;
    ++at;
    if (len > kMaxGroupLen) return false;
    if (len > 0 && !read_(dataCtx_, at, out.groups[g].name, len)) return false;
    out.groups[g].name[len] = '\0';
    at += len;

    for (int m = 0; m < kMembers; ++m) {
      if (!read_(dataCtx_, at, &len, 1)) return false;
      ++at;
      if (len > kMaxWordLen) return false;
      if (len > 0 && !read_(dataCtx_, at, out.groups[g].members[m], len)) return false;
      out.groups[g].members[m][len] = '\0';
      at += len;
    }
  }
  return true;
}

}  // namespace connections
