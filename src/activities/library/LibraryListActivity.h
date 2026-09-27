#pragma once

#include <LibraryIndexFile.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LibraryModel.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// The Library: every indexed book as one list of title/author rows with the
// reading state on the right (percentage and a thin bar, or a check mark) and
// a detail line ("Author · 1 h 30 m read · ~312 pages").
//
// Six sort tabs (Recent, Title, Author, Progress, Time read, Length) and a
// filter (All / Reading / Finished / Archived; archived books only show under
// Archived). Buttons follow the device: side Up/Down move one row (hold: a
// page), front Left/Right step the sort tab (hold: reverse it), Confirm opens
// the book (hold: Open / Finished / Archive / Delete), Back goes Home (hold:
// the View menu with the filters, Search and Refresh). Touch boards tap rows,
// tabs and the header icons.
//
// Pages are whole screens (rows never scroll one at a time), so moving within a
// page repaints only the two rows involved. Only the page on screen keeps its
// strings; the catalog holds 20 bytes per book.
class LibraryListActivity final : public Activity, protected UiAppHost {
 public:
  LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_TAB = 2;
  static constexpr freeink::ui::ActionId ACTION_SEARCH = 3;
  static constexpr freeink::ui::ActionId ACTION_BACK = 4;
  static constexpr freeink::ui::ActionId ACTION_MENU = 5;

  // One book on the current page.
  struct Row {
    uint16_t book;
    std::string title;
    std::string detail;  // author · time read · pages
    std::string path;
    std::string fileName;  // on-card name, picks the row icon
  };

  // Screen geometry, identical for the loop (paging) and the render task.
  struct Layout {
    int16_t headerTop, headerHeight;
    int16_t tabTop, tabHeight;
    int16_t listTop, listBottom;
    int16_t rowHeight, rowGap;
    int16_t left, width;
    int rowsPerPage;
  };

  // --- data (LibraryListActivity.cpp) ------------------------------------------
  bool openCatalog();
  bool rebuildIndex();
  void applyArrangement(bool keepBook);
  // Details for every book when the sort or filter needs them; a popup
  // announces the pass when it has real work to do.
  void ensureAllDetails();
  bool ensureDetails(uint16_t book);
  // Strings and details for the page holding `selected`. Loop task, under the
  // render lock whenever the page changes.
  void preparePage(bool force);
  void formatDetail(uint16_t book, const std::string& author, std::string& out) const;
  bool rowPath(uint16_t book, std::string& path);
  void applySearch(const std::string& text);
  void refreshHeaderLabel();
  void savePrefs() const;
  bool sortDescending() const { return (descendingMask >> static_cast<int>(sort)) & 1u; }

  // --- input -------------------------------------------------------------------
  void moveSelection(int position);
  void stepSort(int direction);
  void selectSort(int index, bool toggleIfActive);
  void reverseSort();
  void setFilter(library_ui::Filter next);
  void openSelected();
  void openBook(const std::string& path);
  void showBookActions();
  void showViewMenu();
  void promptDelete(const std::string& path, const std::string& title);
  void openSearch();
  void promptRebuild();
  void handleBack();
  void swallowHeldReleases();
  static void rowAction(const freeink::ui::ActionEvent& event, void* user);
  static void tabAction(const freeink::ui::ActionEvent& event, void* user);
  static void searchAction(const freeink::ui::ActionEvent& event, void* user);
  static void backAction(const freeink::ui::ActionEvent& event, void* user);
  static void menuAction(const freeink::ui::ActionEvent& event, void* user);

  // --- drawing (LibraryListView.cpp) ----------------------------------------------
  Layout computeLayout() const;
  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  void buildHeader(UiScreen& screen);
  void buildTabs(UiScreen& screen);
  const char* headerTitle() const;
  freeink::ui::Rect rowRect(int slot) const;
  void drawRow(freeink::ui::DrawTarget& target, int position, bool selected) const;
  void drawProgress(freeink::ui::DrawTarget& target, freeink::ui::Rect column, freeink::ui::Rect bar,
                    const library_ui::Book& book, freeink::ui::Paint ink, freeink::ui::Paint paper) const;
  int readoutWidth() const;
  void drawReadout() const;
  void drawFooter() const;
  bool canRepaintRows() const;

  library::LibraryIndexFile index;
  library_ui::LibraryModel model;
  library_ui::Sort sort = library_ui::Sort::Recent;
  library_ui::Filter filter = library_ui::Filter::All;
  uint8_t descendingMask = 0;
  ButtonNavigator navigator;
  Layout layout{};

  int selected = 0;  // position in the arranged order
  int pageStart = -1;
  std::vector<Row> rows;
  bool loadFailed = false;
  bool degraded = false;  // index ranks unsorted

  // Search: one bit per book, null when no search is active.
  std::string query;
  // Header title: the search in quotes, or "Library" plus a non-default filter.
  std::string headerLabel;
  std::unique_ptr<uint8_t[]> match;

  // Row styles resolved from the theme on the last full build.
  freeink::ui::StyleSet rowStyles{};
  freeink::ui::TextStyle titleText{};
  freeink::ui::TextStyle detailText{};
  freeink::ui::SelectionMarker marker = freeink::ui::SelectionMarker::None;
  int16_t rowSidePadding = 8;

  // What the framebuffer shows, for row-only repaints. Anything but a
  // selection move bumps frameGen, which forces a full frame.
  void invalidateFrame() { frameGen.fetch_add(1); }
  std::atomic<uint32_t> frameGen{1};
  uint32_t drawnGen = 0;
  int drawnSelected = -1;
  int paintSelected = 0;  // the selection the frame being built shows

  bool lockConfirmRelease = false;
  bool lockBackRelease = false;
  OptionPopup popup;
};
