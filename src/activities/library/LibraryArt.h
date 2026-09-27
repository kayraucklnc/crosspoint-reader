#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class GfxRenderer;

// 1-bit drawing vocabulary of the Library: wooden shelves, book spines, the
// archive stamp and small status marks. Pure drawing, no state.
namespace library_art {

// Shelf board: a lit top edge over a dark grained front face and a soft shadow.
inline constexpr int PLANK_HEIGHT = 16;
void drawPlank(const GfxRenderer& r, int x, int y, int width, uint32_t seed);
// Bookcase side panel, grained vertically.
void drawUpright(const GfxRenderer& r, int x, int y, int width, int height, uint32_t seed);
// Sparse dot texture of the bookcase back.
void drawBackPanel(const GfxRenderer& r, int x, int y, int width, int height);

struct Spine {
  int x;
  int baseY;  // top of the shelf board the book stands on
  int width;
  int height;
  uint16_t sig;  // tonal profile, see spineSigFromBmp / spineSigFromSeed
  uint32_t seed;
  const char* title;
  const char* author;
  bool archived;
  bool finished;
  bool inProgress;
  bool focused;  // pulled out toward the reader
};
void drawSpine(const GfxRenderer& r, const Spine& spine);

// L-shaped metal bookend holding the end of a row.
void drawBookend(const GfxRenderer& r, int x, int baseY, int height);

int spineWidthFor(uint32_t fileSize);
int spineHeightFor(uint32_t seed, int maxHeight);

// Six 2-bit tones (0 black .. 3 white) top to bottom, plus a validity bit.
inline constexpr uint16_t SIG_VALID = 0x8000;
uint16_t spineSigFromBmp(const std::string& bmpPath);
uint16_t spineSigFromSeed(uint32_t seed);

// Library "READ" stamp across the lower part of a cover, plus dust.
void drawArchiveStamp(const GfxRenderer& r, int x, int y, int width, int height);
// Clears a Bayer share of the pixels so a region recedes (dims) without gray.
void dim(const GfxRenderer& r, int x, int y, int width, int height, int level);
void drawProgressBar(const GfxRenderer& r, int x, int y, int width, int height, int percent);
void drawCheck(const GfxRenderer& r, int x, int y, int size);
// Placeholder slot while a cover is being prepared.
void drawCoverPlaceholder(const GfxRenderer& r, int x, int y, int width, int height);

// "3h 20m" / "45 min"; empty when zero.
void formatDuration(char* out, size_t size, uint32_t seconds);
// "Today" / "Yesterday" / "5 days ago" when the clock is set, else the date.
void formatLastRead(char* out, size_t size, uint32_t epoch);

}  // namespace library_art
