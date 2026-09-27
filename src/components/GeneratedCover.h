#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;

// Typographic "noir" covers for books that carry no cover image: black board,
// a double white rule, a geometric motif picked from a hash of the title, and
// the title and author set in white. Every title gets a different motif and
// arrangement, so a shelf of cover-less books still reads as distinct objects.
namespace generated_cover {

// Draws the cover into the current render target.
void draw(const GfxRenderer& renderer, int x, int y, int width, int height, const char* title, const char* author);

// Renders the cover band by band (a 32-row strip, ~3 KB) and writes it as a
// top-down 1-bit BMP, so it is cached and blitted like any extracted thumbnail
// (including the coverflow tilt). Caller must hold the render lock: it borrows
// the renderer's orientation and strip target.
bool writeThumb(GfxRenderer& renderer, const std::string& path, int width, int height, const char* title,
                const char* author);

// Stable 32-bit FNV-1a of the title, the seed of every design choice.
uint32_t seedFor(const char* title);

}  // namespace generated_cover
