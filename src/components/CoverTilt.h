#pragma once

#include <string>

// Pre-rendered "coverflow" variants of a cover thumbnail: the cover turned on
// its vertical axis, drawn as a trapezoid whose near edge keeps the full height
// and whose far edge shrinks to FAR_SCALE, with columns spaced by true
// perspective so horizontal lines converge toward the far edge. Generated once
// per thumbnail and cached next to it, so drawing a neighbour is a plain blit.
namespace cover_tilt {

enum class Side { LEFT, RIGHT };  // which side of the centre cover the variant sits on

constexpr int FAR_SCALE_PERCENT = 62;
constexpr int WIDTH_PERCENT = 46;  // output width relative to the flat cover

// "<dir>/thumb_330.bmp" -> "<dir>/thumb_330_L.bmp" / "_R.bmp".
std::string pathFor(const std::string& thumbPath, Side side);

// Writes the tilted variant of `thumbPath` unless it already exists.
bool ensure(const std::string& thumbPath, Side side);

// Deletes thumb_* files in a book's cache dir when they were dithered with a
// different Image Dithering mode, so covers regenerate in the current mode.
void invalidateIfDitherChanged(const std::string& thumbPath);

}  // namespace cover_tilt
