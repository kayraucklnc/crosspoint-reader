#pragma once

#include <string>

// Per-book shelf actions shared by Home, the Library and the reader menu, so
// every surface means the same thing by "Remove from Home" or "archive".
namespace book_shelf {

// True while the book has a recents entry (it may still be hidden from Home
// when archived).
bool isOnHome(const std::string& path);

// Drops the book's recents entry only: progress, reading time and Library
// flags stay. Returns false when it was not on Home.
bool removeFromHome(const std::string& path);

// Library flags FINISHED + ARCHIVED (clearing an UNFINISHED override); archived
// books are hidden from Home and listed only in the Library's Archived view.
void markFinishedAndArchive(const std::string& path);

// Deletes the saved reading position and the recents entry, and clears a
// manual FINISHED flag. Reading time and bookmarks are kept.
bool resetProgress(const std::string& path);

}  // namespace book_shelf
