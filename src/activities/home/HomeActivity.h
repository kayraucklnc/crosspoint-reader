#pragma once
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Home: a coverflow of recent books above Income, Trains, Play and Settings.
// The flow ends in a "See Library" card that opens the Library. Side page
// buttons flip books, front Left/Right highlight the bottom row, Confirm opens
// the highlighted item (or the centre book / card when none is), Back clears
// the highlight, then resumes the last book.
class HomeActivity final : public Activity {
 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int ROW_COUNT = 4;  // Income, Trains, Play, Settings
  static constexpr int NO_HIGHLIGHT = -1;
  // Recent books shown before the Library card; archived books are skipped.
  static constexpr int MAX_CAROUSEL_BOOKS = 7;

  ButtonNavigator buttonNavigator;
  std::vector<RecentBook> recentBooks;
  std::vector<int> progressCache;  // -2 = not loaded yet, -1 = unavailable
  int highlight = NO_HIGHLIGHT;    // bottom-row item, or the centre book when NO_HIGHLIGHT
  int bookIndex = 0;
  int transitionDir = 0;      // non-zero while the one intermediate "pull" frame is drawn
  int libraryBookCount = -1;  // books in the Library index, -1 when unknown
  bool coversPending = true;
  bool firstRenderDone = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  int centerCoverHeight() const;
  int sideCoverHeight() const;
  void ensureCovers();
  bool ensureThumb(RecentBook& book, int height);
  std::string thumbPath(const RecentBook& book, int height) const;
  int bookProgress(int index);
  // The carousel holds every recent book plus the Library card at the end.
  bool onLibraryCard() const { return bookIndex == static_cast<int>(recentBooks.size()); }

  void activate();
  void flipBook(int direction);
  void moveHighlight(int direction);

  void renderCarousel(int top, int height);
  void renderTransition(int top);
  void renderBookInfo(int top);
  void renderDots(int top, int height) const;
  void renderTile(int x, int y, int width, int height, const uint8_t* icon, const char* label, bool selected) const;
  // Returns the top of the app rows.
  int renderAppRows(int bottom) const;
  bool drawCover(const RecentBook& book, int height, int centerX, int top, int* outWidth) const;
  bool drawTilted(const RecentBook& book, bool leftSide, int nearX, int top, int* outWidth) const;
  // A flat destination tile, deliberately unlike a cover: outline, shelf glyph,
  // label and chevron. `peek` is the smaller neighbour variant.
  void drawLibraryCard(int centerX, int top, int height, bool peek) const;
  void renderLibraryInfo(int top) const;
};
