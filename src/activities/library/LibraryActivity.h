#pragma once

#include <LibraryIndexFile.h>

#include <cstdint>
#include <string>
#include <vector>

#include "LibraryModel.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// The Library: every indexed book as a wooden bookcase (Shelf), a cover Grid
// or a compact List, sortable (recent, title, author, progress) and filtered
// (all, reading, finished, archived). One control scheme across views:
//
//   Left / Right      previous / next book (hold to repeat)
//   Side Up / Down    previous / next page; Up on the first page enters the bar
//   Confirm           details card (hold: open the book right away)
//   Back              Home
//
// In the top bar Left/Right pick Shelf | Grid | List | Sort | Filter |
// Get Books | Search,
// Confirm applies, Back or side Down returns to the books. The details card
// offers Open / Mark finished / Archive; side buttons step through books.
//
// Only the page on screen is materialized (strings and covers); the catalog
// keeps 28 bytes per book. Focus moves inside a Grid or List page repaint just
// the two affected cells.
class LibraryActivity final : public Activity {
 public:
  LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Library", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Thumbnail heights, shared with nothing else so the cache holds two sizes.
  static constexpr int COVER_HEIGHT = 170;  // grid, shelf display and details card
  static constexpr int ROW_COVER_HEIGHT = 66;

 private:
  enum class Zone : uint8_t { Books, Bar };
  enum BarItem : uint8_t { BAR_SHELF, BAR_GRID, BAR_LIST, BAR_SORT, BAR_FILTER, BAR_GET_BOOKS, BAR_SEARCH, BAR_COUNT };
  enum CardAction : uint8_t { ACTION_OPEN, ACTION_FINISHED, ACTION_ARCHIVE, ACTION_COUNT };

  // One book on the current page.
  struct Item {
    enum : uint8_t { ROW_READY = 1, COVER_READY = 2, ROW_TRIED = 4, COVER_TRIED = 8 };
    uint16_t book;
    uint8_t thumbs;
    std::string title;
    std::string author;
    std::string path;
  };

  library::LibraryIndexFile index;
  library_ui::LibraryModel model;
  library_ui::View view = library_ui::View::Shelf;
  library_ui::Sort sort = library_ui::Sort::Recent;
  library_ui::Filter filter = library_ui::Filter::All;
  ButtonNavigator navigator;

  Zone zone = Zone::Books;
  int barFocus = BAR_SHELF;
  int focus = 0;  // position in the arranged order
  bool cardOpen = false;
  int cardAction = ACTION_OPEN;
  bool loadFailed = false;
  bool orderDirty = false;  // a card action changed what the filter shows
  bool lockConfirmRelease = false;

  // Page on screen: positions [pageStart, pageEnd) and their strings.
  int pageStart = 0;
  int pageEnd = 0;
  std::vector<Item> items;
  int pendingThumb = 0;  // next item to check for a missing cover
  // First position of every shelf in the Shelf view (packing by spine width).
  std::vector<uint16_t> shelfStarts;

  // What the framebuffer shows, for focus-only repaints.
  int drawnFocus = -1;
  int drawnPageStart = -1;
  bool fullRedraw = true;

  // --- data -----------------------------------------------------------------
  bool openCatalog();
  void applyArrangement(bool keepBook);
  void layoutShelves();
  int shelfOf(int position) const;
  void pageBounds(int position, int& start, int& end) const;
  int pageSize() const;
  int pageNumber() const;
  int pageTotal() const;
  // Loads strings, progress and spine tones for the page around `focus`.
  void preparePage();
  void ensureProgress(Item& item);
  void ensureAllProgress();
  // Makes one missing cover the screen needs; false when none is left.
  bool generateNextThumb();
  // Thumbnail sizes item `i` needs right now (Item::ROW_READY / COVER_READY bits).
  uint8_t thumbsWanted(int itemIndex) const;
  Item* focusedItem();

  // --- input ----------------------------------------------------------------
  void moveFocus(int delta);
  void movePage(int direction);
  void handleBarConfirm();
  void handleCardConfirm();
  void openBook(const std::string& path);
  void setView(library_ui::View next);
  void savePrefs() const;

  // --- drawing (LibraryViews.cpp / LibraryDetailCard.cpp) --------------------
  int contentTop() const;
  int contentBottom() const;
  int gutter() const;
  void drawHeader() const;
  void drawBarItem(int item, int x, int y, int w, int h, const char* label, bool active) const;
  void drawShelfView();
  void drawShelfDisplay(int top, int bottom);
  void drawGridView();
  void drawGridCell(int position, bool focused);
  void drawListView();
  void drawListRow(int position, bool focused);
  void drawEmpty() const;
  void drawDetailCard();
  void drawHints() const;
  bool drawThumb(const Item& item, int height, int x, int y, int slotWidth) const;
  const Item* itemAt(int position) const;
};
