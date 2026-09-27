#pragma once

#include <string>

class GfxRenderer;

// Cover thumbnails shared by Home and the Library. A thumbnail lives in the
// book's cache dir as thumb_<height>.bmp: extracted from the book's own cover
// when it has one, otherwise a generated typographic cover written to the same
// path, so every consumer (coverflow tilt included) treats both alike.
namespace book_thumbs {

// Cache dir of the book's parser (epub_/xtc_/txt_<hash> under /.crosspoint).
std::string cacheDirFor(const std::string& bookPath);
std::string thumbPath(const std::string& bookPath, int height);

// Makes sure thumb_<height>.bmp exists. May parse the book (hundreds of ms on
// the first call per size) and, for cover-less books, renders one: the caller
// must hold the render lock.
bool ensure(GfxRenderer& renderer, const std::string& bookPath, int height, const char* title, const char* author);

// Width a generated cover uses for `height` (2:3, like the Home placeholder card).
inline int generatedWidth(const int height) { return height * 2 / 3; }

// True once the reader has saved a position for the book.
bool wasOpened(const std::string& bookPath);

}  // namespace book_thumbs
