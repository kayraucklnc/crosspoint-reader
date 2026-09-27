#include "LibraryListActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "LibraryMetaStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/browser/GetBooksActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/BookCacheUtils.h"
#include "util/BookShelfActions.h"

using library_ui::Filter;
using library_ui::Sort;

namespace fui = freeink::ui;

namespace {
constexpr unsigned long HOLD_MS = 700;
// Above this many books without details the pass announces itself.
constexpr int DETAILS_POPUP_THRESHOLD = 8;
constexpr const char* DETAIL_SEPARATOR = " \xC2\xB7 ";  // " · "
constexpr int SECONDS_PER_MINUTE = 60;

const char* filterLabel(const Filter filter) {
  switch (filter) {
    case Filter::Reading:
      return tr(STR_LIB_FILTER_READING);
    case Filter::Finished:
      return tr(STR_LIB_FILTER_FINISHED);
    case Filter::Archived:
      return tr(STR_LIB_FILTER_ARCHIVED);
    case Filter::All:
    default:
      return tr(STR_LIB_FILTER_ALL);
  }
}
}  // namespace

LibraryListActivity::LibraryListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Library", renderer, mappedInput), UiAppHost(renderer) {}

void LibraryListActivity::onEnter() {
  // One lock across the data phase: the index build wants the card to itself
  // and the render task must not read the catalog before it exists.
  RenderLock lock(*this);
  Activity::onEnter();
  resetUi();
  app.on(ACTION_ROW, &LibraryListActivity::rowAction, this);
  app.on(ACTION_TAB, &LibraryListActivity::tabAction, this);
  app.on(ACTION_SEARCH, &LibraryListActivity::searchAction, this);
  app.on(ACTION_BACK, &LibraryListActivity::backAction, this);
  app.on(ACTION_MENU, &LibraryListActivity::menuAction, this);
  app.setScreen(&LibraryListActivity::screenTrampoline, this);

  sort = static_cast<Sort>(std::min<int>(LIBRARY_META.sort, library_ui::SORT_COUNT - 1));
  descendingMask = LIBRARY_META.descending;
  // The filter is per visit: a Library that reopened on "Archived" would look empty.
  filter = Filter::All;

  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  loadFailed = !openCatalog();
  layout = computeLayout();
  applySearch({});
  if (!loadFailed) applyArrangement(false);
  // Entered while Confirm was still held (from Home): ignore its release.
  lockConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryListActivity::onExit() {
  popup.dismiss();
  index.close();
  Activity::onExit();
}

// --- catalog --------------------------------------------------------------------

bool LibraryListActivity::rebuildIndex() {
  index.close();
  GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
  library::BuildStats stats;
  const bool ok = library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0);
  if (!ok) LOG_ERR("LIB", "index build failed");
  LOG_INF("LIB", "reconciled: %u unchanged, %u added, %u renamed, %u removed (%u dup, %u unreadable)",
          static_cast<unsigned>(stats.unchanged), static_cast<unsigned>(stats.added),
          static_cast<unsigned>(stats.renamed), static_cast<unsigned>(stats.removed),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped));
  if (!index.open(library::libraryIndexPath())) {
    LOG_ERR("LIB", "cannot open library index");
    return false;
  }
  return true;
}

bool LibraryListActivity::openCatalog() {
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  bool rebuild = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                 index.header().metadataEnabled != readMetadata;
  if (!rebuild) {
    if (!model.load(index)) return false;
    // Once per boot: a recent book outside the scanned tree must not force a walk on every entry.
    static bool staleRebuildDone = false;
    rebuild = model.missingRecents() > 0 && !staleRebuildDone;
    staleRebuildDone = staleRebuildDone || rebuild;
    if (rebuild) LOG_INF("LIB", "%d recent books missing from the index; rebuilding", model.missingRecents());
  }
  if (rebuild && (!rebuildIndex() || !model.load(index))) return false;
  degraded = index.ranksDegraded();
  return true;
}

void LibraryListActivity::applyArrangement(const bool keepBook) {
  const int count = model.count();
  const uint16_t kept = keepBook && selected < count ? model.bookAt(selected) : 0xFFFF;
  if (library_ui::LibraryModel::needsAllDetails(sort, filter)) ensureAllDetails();
  model.arrange(sort, sortDescending(), filter, match.get());
  const int position = kept != 0xFFFF ? model.positionOf(kept) : -1;
  if (position >= 0) {
    selected = position;
  } else {
    selected = keepBook ? std::clamp(selected, 0, std::max(0, model.count() - 1)) : 0;
  }
  preparePage(true);
}

bool LibraryListActivity::ensureDetails(const uint16_t book) {
  if (model.detailsKnown(book)) return false;
  library::ClixRecord record{};
  std::string path;
  uint64_t hash = 0;
  if (!index.readRecord(book, record) || !index.readPath(record, path) || !index.readPathHash(record, hash)) {
    LOG_ERR("LIB", "cannot resolve book %u", static_cast<unsigned>(book));
    return false;
  }
  return model.ensureDetails(book, path, record.fileSize, hash);
}

void LibraryListActivity::ensureAllDetails() {
  int unknown = 0;
  for (uint16_t i = 0; i < model.total(); i++) {
    if (!model.detailsKnown(i)) unknown++;
  }
  if (unknown == 0) return;
  if (unknown > DETAILS_POPUP_THRESHOLD) GUI.drawPopup(renderer, tr(STR_LIB_CHECKING_PROGRESS));
  for (uint16_t i = 0; i < model.total(); i++) {
    ensureDetails(i);
    if ((i + 1) % library_ui::LibraryModel::FLUSH_EVERY == 0) model.flushLengths();
  }
  model.flushLengths();
  invalidateFrame();
}

bool LibraryListActivity::rowPath(const uint16_t book, std::string& path) {
  library::ClixRecord record{};
  return index.readRecord(book, record) && index.readPath(record, path);
}

// Callers hold the render lock: the render task reads `rows` and `pageStart`.
void LibraryListActivity::preparePage(const bool force) {
  const int perPage = std::max(1, layout.rowsPerPage);
  const int start = selected / perPage * perPage;
  if (!force && start == pageStart) return;

  const int end = std::min(model.count(), start + perPage);
  std::vector<Row> fresh;
  fresh.reserve(static_cast<size_t>(std::max(0, end - start)));
  std::string author;
  for (int p = start; p < end; p++) {
    Row row{};
    row.book = model.bookAt(p);
    library::ClixRecord record{};
    author.clear();
    if (index.readRecord(row.book, record)) {
      if (!index.readTitle(record, row.title) || row.title.empty()) index.readName(record, row.title);
      if (!index.readAuthor(record, author)) author.clear();
      index.readPath(record, row.path);
      index.readName(record, row.fileName);
    }
    if (row.title.empty()) row.title = tr(STR_LIBRARY_UNKNOWN_TITLE);
    ensureDetails(row.book);
    formatDetail(row.book, author, row.detail);
    fresh.push_back(std::move(row));
  }
  model.flushLengths();
  rows.swap(fresh);
  pageStart = start;
  invalidateFrame();
}

void LibraryListActivity::formatDetail(const uint16_t book, const std::string& author, std::string& out) const {
  const library_ui::Book& info = model.book(book);
  out = author;
  char part[32];
  const auto append = [&out](const char* text) {
    if (!out.empty()) out += DETAIL_SEPARATOR;
    out += text;
  };
  const uint32_t minutes = info.readSeconds / SECONDS_PER_MINUTE;
  if (minutes >= 60) {
    snprintf(part, sizeof(part), tr(STR_LIB_READ_HOURS), static_cast<int>(minutes / 60),
             static_cast<int>(minutes % 60));
    append(part);
  } else if (minutes > 0) {
    snprintf(part, sizeof(part), tr(STR_LIB_READ_MINUTES), static_cast<int>(minutes));
    append(part);
  }
  bool exact = false;
  const uint32_t pages = model.pagesFor(book, exact);
  if (pages > 0) {
    snprintf(part, sizeof(part), exact ? tr(STR_LIB_PAGES) : tr(STR_LIB_PAGES_ABOUT), static_cast<unsigned>(pages));
    append(part);
  }
}

// One bit per book whose folded title or author contains the query. Folded the
// same way the stored folds were, articles removed included — otherwise "the
// hobbit" searches for a word no record contains.
void LibraryListActivity::applySearch(const std::string& text) {
  query = text;
  match.reset();
  if (!query.empty()) {
    const uint16_t total = model.total();
    auto bits = makeUniqueNoThrow<uint8_t[]>((total + 7u) / 8u);
    if (!bits) {
      LOG_ERR("LIB", "OOM: search bitmap for %u books", static_cast<unsigned>(total));
      query.clear();
    } else {
      memset(bits.get(), 0, (total + 7u) / 8u);
      const std::string needle = library::fold(query, /*stripArticle=*/true);
      std::string author;
      for (uint16_t i = 0; i < total; i++) {
        library::ClixRecord record{};
        if (!index.readRecord(i, record)) continue;
        bool hit = library::matchesQuery(std::string_view(record.fold, record.foldLen), needle);
        if (!hit) {
          // The stored fold covers the title only, so the author is folded here.
          author.clear();
          hit = index.readAuthor(record, author) && library::matchesQuery(library::fold(author), needle);
        }
        if (hit) bits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      }
      match = std::move(bits);
    }
  }
  refreshHeaderLabel();
}

void LibraryListActivity::refreshHeaderLabel() {
  if (!query.empty()) {
    headerLabel = "\xE2\x80\x9C" + query + "\xE2\x80\x9D";  // “query”
  } else if (degraded) {
    headerLabel = tr(STR_LIBRARY_TITLE_UNSORTED);
  } else {
    headerLabel = tr(STR_LIBRARY);
    if (filter != Filter::All) {
      headerLabel += DETAIL_SEPARATOR;
      headerLabel += filterLabel(filter);
    }
  }
}

void LibraryListActivity::savePrefs() const { LIBRARY_META.savePrefs(static_cast<uint8_t>(sort), descendingMask); }

// --- input ----------------------------------------------------------------------

void LibraryListActivity::moveSelection(const int position) {
  const int count = model.count();
  if (count <= 0) return;
  const int next = std::clamp(position, 0, count - 1);
  if (next == selected) return;
  const int perPage = std::max(1, layout.rowsPerPage);
  if (next / perPage * perPage == pageStart) {
    selected = next;  // same page: the render repaints just the two rows
  } else {
    RenderLock lock(*this);
    selected = next;
    preparePage(false);
  }
  requestUpdate();
}

void LibraryListActivity::selectSort(const int index, const bool toggleIfActive) {
  if (index < 0 || index >= library_ui::SORT_COUNT) return;
  RenderLock lock(*this);
  if (toggleIfActive && index == static_cast<int>(sort)) descendingMask ^= static_cast<uint8_t>(1u << index);
  sort = static_cast<Sort>(index);
  applyArrangement(false);
  savePrefs();
  requestUpdate();
}

void LibraryListActivity::stepSort(const int direction) {
  const int next = (static_cast<int>(sort) + library_ui::SORT_COUNT + direction) % library_ui::SORT_COUNT;
  selectSort(next, false);
}

void LibraryListActivity::reverseSort() { selectSort(static_cast<int>(sort), true); }

void LibraryListActivity::setFilter(const Filter next) {
  RenderLock lock(*this);
  filter = next;
  refreshHeaderLabel();
  applyArrangement(false);
  requestUpdate();
}

void LibraryListActivity::openSelected() {
  const int slot = selected - pageStart;
  if (slot < 0 || slot >= static_cast<int>(rows.size())) return;
  openBook(rows[static_cast<size_t>(slot)].path);
}

void LibraryListActivity::openBook(const std::string& path) {
  if (path.empty()) return;
  const std::string target = path;  // `path` may live in `rows`, which onExit frees
  app.clearTapFlash();
  index.close();
  onSelectBook(target);
}

void LibraryListActivity::showBookActions() {
  const int slot = selected - pageStart;
  if (slot < 0 || slot >= static_cast<int>(rows.size())) return;
  const Row& row = rows[static_cast<size_t>(slot)];
  const library_ui::Book& book = model.book(row.book);
  // "Remove from Home" is offered only while the book is on Home.
  std::array<BookAction, BOOK_ACTION_COUNT> actions{};
  const char* options[BOOK_ACTION_COUNT] = {};
  int count = 0;
  const auto add = [&](const BookAction action, const char* label) {
    actions[count] = action;
    options[count++] = label;
  };
  add(BookAction::Open, tr(STR_OPEN));
  add(BookAction::Finish, book.finished() ? tr(STR_LIB_MARK_UNFINISHED) : tr(STR_LIB_MARK_FINISHED));
  add(BookAction::Archive, book.archived() ? tr(STR_LIB_UNARCHIVE) : tr(STR_LIB_ARCHIVE));
  if (book_shelf::isOnHome(row.path)) add(BookAction::RemoveFromHome, tr(STR_HOME_REMOVE));
  add(BookAction::Delete, tr(STR_DELETE));
  const uint16_t id = row.book;
  const std::string path = row.path;
  const std::string title = row.title;
  app.clearTapFlash();
  popup.show(tr(STR_LIBRARY), title.c_str(), options, count, 0, [this, id, path, title, actions, count](int choice) {
    swallowHeldReleases();
    invalidateFrame();
    if (choice < 0 || choice >= count) return;
    const library_ui::Book& current = model.book(id);
    switch (actions[static_cast<size_t>(choice)]) {
      case BookAction::Open:
        openBook(path);
        return;
      case BookAction::Finish: {
        // A finish that stats or progress imply needs an explicit override to undo.
        const bool implied = (current.flags & library_ui::Book::STATS_FINISHED) || current.progress >= 100;
        const bool finished = current.finished();
        const uint8_t set =
            finished ? (implied ? LibraryMetaStore::UNFINISHED : 0) : (implied ? 0 : LibraryMetaStore::FINISHED);
        const uint8_t clear = finished ? LibraryMetaStore::FINISHED : LibraryMetaStore::UNFINISHED;
        RenderLock lock(*this);
        model.setMetaFlags(id, LIBRARY_META.updateFlags(path, set, clear));
        applyArrangement(true);
        break;
      }
      case BookAction::Archive: {
        const bool archived = current.archived();
        RenderLock lock(*this);
        model.setMetaFlags(id, LIBRARY_META.updateFlags(path, archived ? 0 : LibraryMetaStore::ARCHIVED,
                                                        archived ? LibraryMetaStore::ARCHIVED : 0));
        applyArrangement(true);
        break;
      }
      case BookAction::RemoveFromHome:
        book_shelf::removeFromHome(path);
        break;
      case BookAction::Delete:
        promptDelete(path, title);
        return;
    }
    requestUpdate();
  });
  requestUpdate();
}

void LibraryListActivity::showViewMenu() {
  const char* options[] = {tr(STR_LIB_FILTER_ALL),      tr(STR_LIB_FILTER_READING), tr(STR_LIB_FILTER_FINISHED),
                           tr(STR_LIB_FILTER_ARCHIVED), tr(STR_LIBRARY_SEARCH),     tr(STR_GET_BOOKS),
                           tr(STR_LIBRARY_REBUILD)};
  app.clearTapFlash();
  popup.show(tr(STR_LIB_VIEW), options, 7, static_cast<int>(filter), [this](const int choice) {
    swallowHeldReleases();
    invalidateFrame();
    if (choice >= 0 && choice < library_ui::FILTER_COUNT) {
      setFilter(static_cast<Filter>(choice));
    } else if (choice == 4) {
      openSearch();
    } else if (choice == 5) {
      openGetBooks();
    } else if (choice == 6) {
      promptRebuild();
    }
    requestUpdate();
  });
  requestUpdate();
}

void LibraryListActivity::openGetBooks() {
  if (auto getBooks = makeUniqueNoThrow<GetBooksActivity>(renderer, mappedInput)) {
    activityManager.replaceActivity(std::move(getBooks));
  } else {
    LOG_ERR("LIB", "OOM: Get Books");
  }
}

void LibraryListActivity::promptDelete(const std::string& path, const std::string& title) {
  // The dialog and the delete both want the card; reopen when we resume.
  index.close();
  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("? "), title);
  if (!confirmation) {
    LOG_ERR("LIB", "OOM: delete confirmation");
    if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    return;
  }
  startActivityForResult(std::move(confirmation), [this, path](const ActivityResult& result) {
    swallowHeldReleases();
    RenderLock lock(*this);
    if (result.isCancelled) {
      if (!index.open(library::libraryIndexPath())) LOG_ERR("LIB", "cannot reopen library index");
    } else {
      LOG_DBG("LIB", "deleting %s", path.c_str());
      clearBookCache(path);
      if (!Storage.remove(path.c_str())) LOG_ERR("LIB", "cannot delete %s", path.c_str());
      book_shelf::removeFromHome(path);
      LIBRARY_META.updateFlags(path, 0, 0xFF);
      loadFailed = !rebuildIndex() || !model.load(index);
      applySearch(query);
      if (!loadFailed) applyArrangement(true);
    }
    invalidateFrame();
  });
}

void LibraryListActivity::promptRebuild() {
  RenderLock lock(*this);
  loadFailed = !rebuildIndex() || !model.load(index);
  degraded = index.ranksDegraded();
  layout = computeLayout();
  applySearch(query);
  if (!loadFailed) applyArrangement(false);
  invalidateFrame();
  requestUpdate(true);
}

void LibraryListActivity::openSearch() {
  app.clearTapFlash();
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LIBRARY_SEARCH), query, 48,
                                                           InputType::Text);
  if (!keyboard) {
    LOG_ERR("LIB", "OOM: search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    swallowHeldReleases();
    invalidateFrame();
    if (result.isCancelled) return;
    RenderLock lock(*this);
    applySearch(std::get<KeyboardResult>(result.data).text);
    applyArrangement(false);
  });
}

// Back clears an active search first, then leaves for Home.
void LibraryListActivity::handleBack() {
  if (!query.empty()) {
    RenderLock lock(*this);
    applySearch({});
    applyArrangement(false);
    requestUpdate();
    return;
  }
  index.close();
  onGoHome(HomeMenuItem::LIBRARY);
}

void LibraryListActivity::swallowHeldReleases() {
  lockConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  lockBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
}

void LibraryListActivity::rowAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  if (event.value < 0 || event.value >= self->model.count()) return;
  self->moveSelection(event.value);
  if (event.longPress) {
    self->showBookActions();
  } else {
    self->openSelected();
  }
}

void LibraryListActivity::tabAction(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryListActivity*>(user);
  self->app.clearTapFlash();
  self->selectSort(event.value, true);
}

void LibraryListActivity::searchAction(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->openSearch();
}

void LibraryListActivity::backAction(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->handleBack();
}

void LibraryListActivity::menuAction(const fui::ActionEvent&, void* user) {
  static_cast<LibraryListActivity*>(user)->showViewMenu();
}

void LibraryListActivity::loop() {
  using Button = MappedInputManager::Button;
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (loadFailed) {
    if (mappedInput.wasReleased(Button::Back)) onGoHome();
    return;
  }
  if (lockConfirmRelease || lockBackRelease) {
    if (lockConfirmRelease && (mappedInput.wasReleased(Button::Confirm) || !mappedInput.isPressed(Button::Confirm))) {
      lockConfirmRelease = false;
    }
    if (lockBackRelease && (mappedInput.wasReleased(Button::Back) || !mappedInput.isPressed(Button::Back))) {
      lockBackRelease = false;
    }
    return;
  }

  const auto route = routeTouch(mappedInput, /*withLongPress=*/true);
  if (route) return;
  const auto swipe = mappedInput.wasSwipe();
  const int perPage = std::max(1, layout.rowsPerPage);
  const int count = model.count();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int page = pageStart + (swipe == MappedInputManager::SwipeDir::Up ? perPage : -perPage);
    if (page >= 0 && page < count) moveSelection(page);
    return;
  }

  // Every hold fires at the threshold; its release is swallowed globally.
  if (count > 0 && mappedInput.wasLongPressed(Button::Confirm, HOLD_MS)) {
    showBookActions();
    return;
  }
  if (mappedInput.wasLongPressed(Button::Back, HOLD_MS)) {
    showViewMenu();
    return;
  }
  if (!degraded &&
      (mappedInput.wasLongPressed(Button::Left, HOLD_MS) || mappedInput.wasLongPressed(Button::Right, HOLD_MS))) {
    reverseSort();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    openSelected();
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    handleBack();
    return;
  }
  if (!degraded && mappedInput.wasReleased(Button::Left)) {
    stepSort(-1);
    return;
  }
  if (!degraded && mappedInput.wasReleased(Button::Right)) {
    stepSort(+1);
    return;
  }

  // Side buttons: one row per press, a page per repeat while held.
  const bool swapped = mappedInput.isNavDirectionSwapped();
  const Button down = swapped ? Button::Up : Button::Down;
  const Button up = swapped ? Button::Down : Button::Up;
  navigator.onPress({down}, [this, count] { moveSelection(ButtonNavigator::nextIndex(selected, count)); });
  navigator.onPress({up}, [this, count] { moveSelection(ButtonNavigator::previousIndex(selected, count)); });
  navigator.onContinuous(
      {down}, [this, count, perPage] { moveSelection(ButtonNavigator::nextPageIndex(selected, count, perPage)); });
  navigator.onContinuous(
      {up}, [this, count, perPage] { moveSelection(ButtonNavigator::previousPageIndex(selected, count, perPage)); });
}
