#pragma once
#include <EpdFontFamily.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// Home: a coverflow of recent books above Income, Trains, Play and Settings.
// The flow ends in a "See Library" card that opens the Library.
//
// Each control owns one thing, so nothing depends on an invisible focus:
// - the books: side page buttons flip them, Back ("Read") opens the centre
//   book or the Library card, and holding it opens the book's action sheet;
// - the app row: Left/Right move its highlight (always exactly one tile),
//   Confirm opens it and its hint names the highlighted tile.
class HomeActivity final : public Activity {
 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  static constexpr int ROW_COUNT = 4;  // Income, Trains, Play, Settings
  // Recent books shown before the Library card; archived books are skipped.
  static constexpr int MAX_CAROUSEL_BOOKS = 7;

  // Which popup is up; its choice is handled once the popup has closed.
  enum class Sheet : uint8_t { None, Actions, ResetAsk, ResetSure };

  // Book info panel text, formatted once when the panel opens.
  struct InfoText {
    std::string title;
    std::string author;
    std::string progress;
    std::string timeRead;
    std::string status;
    std::string file;
  };

  ButtonNavigator buttonNavigator;
  std::vector<RecentBook> recentBooks;
  std::vector<int> progressCache;  // -2 = not loaded yet, -1 = unavailable
  int highlight = 0;               // app-row tile, always one of ROW_COUNT
  int bookIndex = 0;
  int transitionDir = 0;      // non-zero while the one intermediate "pull" frame is drawn
  int libraryBookCount = -1;  // books in the Library index, -1 when unknown
  bool coversPending = true;
  bool firstRenderDone = false;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  OptionPopup popup;
  Sheet sheet = Sheet::None;
  int sheetChoice = -1;
  std::string sheetPath;  // the book the open sheet acts on
  std::string sheetTitle;
  bool infoOpen = false;
  InfoText info;

  // Recents minus missing files and archived books; keeps bookIndex in range.
  void loadCarousel();
  // Truncates `text` with "..." so it fits maxWidth.
  static void fitText(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                      int maxWidth, char* out, size_t outSize);
  int centerCoverHeight() const;
  int sideCoverHeight() const;
  void ensureCovers();
  bool ensureThumb(RecentBook& book, int height);
  std::string thumbPath(const RecentBook& book, int height) const;
  int bookProgress(int index);
  // The carousel holds every recent book plus the Library card at the end.
  bool onLibraryCard() const { return bookIndex == static_cast<int>(recentBooks.size()); }
  const char* rowLabel(int row) const;

  void readCenter();
  void openRow();
  void flipBook(int direction);
  void moveHighlight(int direction);

  // --- book actions (HomeBookActions.cpp) ---
  void showBookActions();
  void handleSheetInput();
  void onSheetChoice(Sheet done, int choice);
  void openBookInfo();
  void handleInfoInput();
  void renderInfoPanel() const;

  void renderCarousel(int top, int height);
  void renderTransition(int top);
  void renderBookInfo(int top);
  void renderReadCue(int centerX, int coverBottom, const char* label) const;
  void renderDots(int top, int height) const;
  void renderTile(int x, int y, int width, int height, const uint8_t* icon, const char* label, bool selected) const;
  // Returns the top of the app rows.
  int renderAppRows(int bottom) const;
  bool drawCover(const RecentBook& book, int height, int centerX, int top, int* outWidth) const;
  bool drawTilted(const RecentBook& book, bool leftSide, int nearX, int top, int* outWidth) const;
  // Outline of a turned cover, shown until the real thumbnail is (re)built.
  void drawTiltedPlaceholder(bool leftSide, int nearX, int top) const;
  // A flat destination tile, deliberately unlike a cover: outline, shelf glyph,
  // label and chevron. `peek` is the smaller neighbour variant.
  void drawLibraryCard(int centerX, int top, int height, bool peek) const;
  void renderLibraryInfo(int top) const;
};
