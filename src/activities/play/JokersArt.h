#pragma once

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>

// Noir Jokers art: 1-bit RLE images streamed from the SD pack
// /.crosspoint/games/jokers/art.bin (built by scripts/gen_jokers_art.py, which
// documents the format). Every draw falls back to simple vector art when the
// pack is missing, so the game stays playable without it.
namespace jokers::art {

constexpr int CARD_W = 116;
constexpr int CARD_H = 164;
constexpr int JOKER_W = 84;
constexpr int JOKER_H = 112;
constexpr int CHIP = 96;
constexpr int PLANET = 72;
constexpr int TITLE_W = 528;
constexpr int TITLE_H = 300;

constexpr int CARD_BACK = 0;
constexpr int FACE_BASE = 1;
constexpr int JOKER_BASE = 53;
constexpr int SMALL_CHIP = 78;
constexpr int BOSS_BASE = 80;
constexpr int PLANET_BASE = 90;
constexpr int TITLE = 99;
constexpr int GLYPH_BASE = 100;  // '0'..'9', ',', 'x', '=', '+', '$'
constexpr int ASSET_COUNT = 115;

class Pack {
 public:
  bool open();
  void close();
  bool ready() const { return index != nullptr; }
  // Draws asset `id` with its top-left at (x, y). `invert` swaps black and
  // white ink. Returns false when the asset is unavailable.
  bool draw(const GfxRenderer& renderer, int id, int x, int y, bool invert = false) const;
  int width(int id) const;
  int height(int id) const;

 private:
  mutable HalFile file;
  std::unique_ptr<uint8_t[]> index;
};

// Scene pieces with vector fallbacks.
void drawCard(const GfxRenderer& r, const Pack& pack, uint8_t card, int x, int y);
void drawCardBack(const GfxRenderer& r, const Pack& pack, int x, int y);
void drawJoker(const GfxRenderer& r, const Pack& pack, uint8_t joker, int x, int y);
void drawBlindChip(const GfxRenderer& r, const Pack& pack, int blind, uint8_t boss, int x, int y);
void drawPlanet(const GfxRenderer& r, const Pack& pack, uint8_t hand, int x, int y);
void drawSuit(const GfxRenderer& r, int suit, int cx, int cy, int radius, bool black);
// Big display numerals ("1,240", "x", "=", "$"): pack glyphs or the largest UI font.
int bigTextWidth(const GfxRenderer& r, const Pack& pack, const char* text);
int bigTextHeight(const Pack& pack);
void drawBigText(const GfxRenderer& r, const Pack& pack, const char* text, int x, int y, bool white = false);
// Menu glyph for the Play screen.
void drawMenuIcon(const GfxRenderer& r, int x, int y);

}  // namespace jokers::art
