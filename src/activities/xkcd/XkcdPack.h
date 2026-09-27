#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

// Read side of the offline comic pack built by scripts/gen_xkcd_pack.py
// (layout documented there). Nothing stays open or resident: each call is a
// short positioned read, so the reader keeps no pack buffers while it is
// online and the TLS stack has the heap.
class XkcdPack {
 public:
  struct Entry {
    uint16_t num = 0;
    bool landscape = false;
    uint8_t dataFile = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t titleLen = 0;
    uint16_t altLen = 0;
    uint32_t textOffset = 0;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
  };

  // Reads the index header; false when no (valid) pack is on the card.
  bool open();
  bool available() const { return count_ > 0; }
  uint32_t count() const { return count_; }
  uint16_t maxNum() const { return maxNum_; }

  bool entryAt(uint32_t index, Entry& out) const;
  // Index of comic `num`, or -1 when the pack does not have it.
  int32_t find(uint16_t num) const;
  // First index whose number is >= num (count() when none).
  uint32_t lowerBound(uint16_t num) const;
  bool readText(const Entry& entry, std::string& title, std::string& alt) const;
  // Draws the 1-bit image with its top-left corner at (x, y), ink only.
  bool draw(const Entry& entry, const GfxRenderer& renderer, int x, int y) const;

 private:
  uint32_t count_ = 0;
  uint16_t maxNum_ = 0;
  uint16_t dataFiles_ = 0;
};
