#pragma once

// Geometry shared by the Library's paging logic and its views, in portrait
// logical pixels relative to the content box the activity computes.
namespace library_layout {

inline constexpr int HEADER_HEIGHT = 96;
// Clearance for the side-button hint boxes that sit on the screen edges.
inline constexpr int SIDE_GUTTER = 44;

inline constexpr int GRID_COLUMNS = 3;
inline constexpr int GRID_ROWS = 3;
inline constexpr int GRID_PAGE = GRID_COLUMNS * GRID_ROWS;

inline constexpr int LIST_ROW_HEIGHT = 80;

// Shelf: a display compartment with the focused book face-out, then two
// shelves of spines.
inline constexpr int SHELVES_PER_PAGE = 2;
inline constexpr int SHELF_COMPARTMENT = 178;  // spine space above each board
inline constexpr int SHELF_UPRIGHT = 10;
inline constexpr int SHELF_OUTSET = 10;  // bookcase edge outside the side gutter
inline constexpr int SPINE_GAP = 1;
inline constexpr int SPINE_MAX_HEIGHT = 164;

inline int shelfInnerWidth(const int screenWidth) {
  return screenWidth - 2 * (SIDE_GUTTER - SHELF_OUTSET) - 2 * SHELF_UPRIGHT;
}

}  // namespace library_layout
