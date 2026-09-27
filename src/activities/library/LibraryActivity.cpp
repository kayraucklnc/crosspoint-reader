#include "LibraryActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <OrderedDither.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "LibraryArt.h"
#include "LibraryLayout.h"
#include "LibraryMetaStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/browser/GetBooksActivity.h"
#include "components/GeneratedCover.h"
#include "components/UITheme.h"
#include "util/BookProgress.h"
#include "util/BookThumbs.h"

using library_ui::Filter;
using library_ui::Sort;
using library_ui::View;

namespace {
constexpr unsigned long HOLD_TO_OPEN_MS = 600;
// Above this many unknown books the progress scan announces itself.
constexpr int PROGRESS_POPUP_THRESHOLD = 8;

template <typename E>
E cycle(const E value, const int count) {
  return static_cast<E>((static_cast<int>(value) + 1) % count);
}
}  // namespace

void LibraryActivity::onEnter() {
  // One lock across the data phase: the index build wants the card to itself
  // and the render task must not read the catalog before it exists.
  RenderLock lock(*this);
  Activity::onEnter();
  ordered_dither::mode = SETTINGS.imageDithering;
  view = static_cast<View>(LIBRARY_META.view);
  sort = static_cast<Sort>(LIBRARY_META.sort);
  filter = static_cast<Filter>(LIBRARY_META.filter);

  if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
  loadFailed = !openCatalog();
  if (!loadFailed) applyArrangement(false);
  // Entered while Confirm was still held (from Home): ignore its release.
  lockConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void LibraryActivity::onExit() {
  index.close();
  Activity::onExit();
}

bool LibraryActivity::openCatalog() {
  const bool readMetadata = SETTINGS.libraryUseMetadata != 0;
  bool rebuild = library::isLibraryIndexDirty() || !index.open(library::libraryIndexPath()) ||
                 index.header().metadataEnabled != readMetadata;
  if (!rebuild) {
    if (!model.load(index)) return false;
    // Once per boot: a recent book outside the scanned tree must not force a walk on every entry.
    static bool staleRebuildDone = false;
    if (model.missingRecents() == 0 || staleRebuildDone) return true;
    staleRebuildDone = true;
    LOG_INF("LIB", "%d recent books missing from the index; rebuilding", model.missingRecents());
    rebuild = true;
  }
  {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, readMetadata)) LOG_ERR("LIB", "index build failed");
    if (!index.open(library::libraryIndexPath())) {
      LOG_ERR("LIB", "cannot open library index");
      return false;
    }
  }
  return model.load(index);
}

// --- arrangement and paging ----------------------------------------------------

void LibraryActivity::applyArrangement(const bool keepBook) {
  const uint16_t kept = keepBook && focus < model.count() ? model.bookAt(focus) : 0xFFFF;
  if (library_ui::LibraryModel::needsAllProgress(sort, filter)) ensureAllProgress();
  model.arrange(sort, filter);
  layoutShelves();
  const int position = kept != 0xFFFF ? model.positionOf(kept) : -1;
  focus = position >= 0 ? position : std::clamp(focus, 0, std::max(0, model.count() - 1));
  if (!keepBook) focus = 0;
  pageStart = pageEnd = -1;
  orderDirty = false;
  preparePage();
}

void LibraryActivity::layoutShelves() {
  using namespace library_layout;
  shelfStarts.clear();
  const int count = model.count();
  const int inner = shelfInnerWidth(renderer.getScreenWidth()) - 4;
  shelfStarts.reserve(count / 12 + 2);
  int used = inner;  // forces a first shelf
  for (int p = 0; p < count; p++) {
    const int w = library_art::spineWidthFor(model.book(model.bookAt(p)).fileSize) + SPINE_GAP;
    if (used + w > inner) {
      shelfStarts.push_back(static_cast<uint16_t>(p));
      used = 0;
    }
    used += w;
  }
}

int LibraryActivity::shelfOf(const int position) const {
  const auto it = std::upper_bound(shelfStarts.begin(), shelfStarts.end(), static_cast<uint16_t>(position));
  return std::max(0, static_cast<int>(it - shelfStarts.begin()) - 1);
}

int LibraryActivity::pageSize() const {
  if (view == View::Grid) return library_layout::GRID_PAGE;
  return std::max(1, (contentBottom() - contentTop()) / library_layout::LIST_ROW_HEIGHT);
}

void LibraryActivity::pageBounds(const int position, int& start, int& end) const {
  const int count = model.count();
  if (view == View::Shelf) {
    const int first = shelfOf(position) / library_layout::SHELVES_PER_PAGE * library_layout::SHELVES_PER_PAGE;
    const int shelves = static_cast<int>(shelfStarts.size());
    start = first < shelves ? shelfStarts[first] : 0;
    const int next = first + library_layout::SHELVES_PER_PAGE;
    end = next < shelves ? shelfStarts[next] : count;
    return;
  }
  const int per = pageSize();
  start = position / per * per;
  end = std::min(count, start + per);
}

int LibraryActivity::pageNumber() const {
  if (view == View::Shelf) return shelfOf(focus) / library_layout::SHELVES_PER_PAGE;
  return focus / pageSize();
}

int LibraryActivity::pageTotal() const {
  if (view == View::Shelf) {
    const int shelves = static_cast<int>(shelfStarts.size());
    return std::max(1, (shelves + library_layout::SHELVES_PER_PAGE - 1) / library_layout::SHELVES_PER_PAGE);
  }
  return std::max(1, (model.count() + pageSize() - 1) / pageSize());
}

// --- page data -----------------------------------------------------------------

void LibraryActivity::ensureProgress(Item& item) {
  library_ui::Book& book = model.book(item.book);
  if (!(book.flags & library_ui::Book::OPENED_KNOWN)) {
    book.flags |= library_ui::Book::OPENED_KNOWN;
    if (book_thumbs::wasOpened(item.path)) book.flags |= library_ui::Book::OPENED;
  }
  if (book.progress != library_ui::PROGRESS_UNKNOWN) return;
  book.progress = (book.flags & library_ui::Book::OPENED) ? static_cast<int8_t>(loadBookProgress(item.path))
                                                          : library_ui::PROGRESS_NONE;
}

void LibraryActivity::ensureAllProgress() {
  int unknown = 0;
  for (uint16_t i = 0; i < model.total(); i++) {
    if (model.book(i).progress == library_ui::PROGRESS_UNKNOWN) unknown++;
  }
  if (unknown == 0) return;
  if (unknown > PROGRESS_POPUP_THRESHOLD) GUI.drawPopup(renderer, tr(STR_LIB_CHECKING_PROGRESS));
  Item scratch{};
  for (uint16_t i = 0; i < model.total(); i++) {
    if (model.book(i).progress != library_ui::PROGRESS_UNKNOWN) continue;
    library::ClixRecord record{};
    if (!index.readRecord(i, record) || !index.readPath(record, scratch.path)) {
      model.book(i).progress = library_ui::PROGRESS_NONE;
      continue;
    }
    scratch.book = i;
    ensureProgress(scratch);
  }
}

// Callers hold the render lock whenever the page can change: the render task
// reads `items` and the shelf layout.
void LibraryActivity::preparePage() {
  int start = 0;
  int end = 0;
  pageBounds(focus, start, end);
  if (start == pageStart && end == pageEnd) return;

  std::vector<Item> fresh;
  fresh.reserve(end - start);
  const int coverHeight = renderer.getScreenHeight() * 44 / 100;  // Home's centre cover, often cached
  for (int p = start; p < end; p++) {
    Item item{};
    item.book = model.bookAt(p);
    library::ClixRecord record{};
    if (index.readRecord(item.book, record)) {
      index.readTitle(record, item.title);
      index.readAuthor(record, item.author);
      index.readPath(record, item.path);
    }
    if (item.title.empty()) item.title = tr(STR_LIBRARY_UNKNOWN_TITLE);
    ensureProgress(item);
    if (Storage.exists(book_thumbs::thumbPath(item.path, COVER_HEIGHT).c_str())) item.thumbs |= Item::COVER_READY;
    if (view == View::List && Storage.exists(book_thumbs::thumbPath(item.path, ROW_COVER_HEIGHT).c_str())) {
      item.thumbs |= Item::ROW_READY;
    }
    library_ui::Book& book = model.book(item.book);
    if (view == View::Shelf && !(book.spineSig & library_art::SIG_VALID)) {
      // The spine takes its tones from any cover already cached for the book.
      const int heights[] = {ROW_COVER_HEIGHT, COVER_HEIGHT, coverHeight};
      for (const int h : heights) {
        book.spineSig = library_art::spineSigFromBmp(book_thumbs::thumbPath(item.path, h));
        if (book.spineSig) break;
      }
      if (!book.spineSig) book.spineSig = library_art::spineSigFromSeed(generated_cover::seedFor(item.title.c_str()));
    }
    fresh.push_back(std::move(item));
  }

  items.swap(fresh);
  pageStart = start;
  pageEnd = end;
  pendingThumb = 0;
  fullRedraw = true;
}

uint8_t LibraryActivity::thumbsWanted(const int i) const {
  const bool focused = pageStart + i == focus;
  if (cardOpen) return focused ? Item::COVER_READY : 0;
  switch (view) {
    case View::Grid:
      return Item::COVER_READY;
    case View::List:
      return Item::ROW_READY;
    case View::Shelf:
    default:
      return focused ? Item::COVER_READY : 0;
  }
}

bool LibraryActivity::generateNextThumb() {
  // Focused book first: it is what the shelf display and the card show.
  const int count = static_cast<int>(items.size());
  for (int n = 0; n < count; n++) {
    const int i = n == 0 ? focus - pageStart : (n <= focus - pageStart ? n - 1 : n);
    if (i < 0 || i >= count) continue;
    Item& item = items[i];
    const uint8_t wanted = thumbsWanted(i);
    for (const uint8_t ready : {Item::COVER_READY, Item::ROW_READY}) {
      if (!(wanted & ready) || (item.thumbs & ready)) continue;
      const uint8_t tried = ready == Item::COVER_READY ? Item::COVER_TRIED : Item::ROW_TRIED;
      if (item.thumbs & tried) continue;
      const int height = ready == Item::COVER_READY ? COVER_HEIGHT : ROW_COVER_HEIGHT;
      RenderLock lock(*this);
      const bool ok = book_thumbs::ensure(renderer, item.path, height, item.title.c_str(), item.author.c_str());
      item.thumbs |= tried | (ok ? ready : 0);
      return true;
    }
  }
  return false;
}

LibraryActivity::Item* LibraryActivity::focusedItem() {
  const int i = focus - pageStart;
  return i >= 0 && i < static_cast<int>(items.size()) ? &items[i] : nullptr;
}

const LibraryActivity::Item* LibraryActivity::itemAt(const int position) const {
  const int i = position - pageStart;
  return i >= 0 && i < static_cast<int>(items.size()) ? &items[i] : nullptr;
}

// --- input -----------------------------------------------------------------------

void LibraryActivity::moveFocus(const int delta) {
  const int next = std::clamp(focus + delta, 0, std::max(0, model.count() - 1));
  if (next == focus) return;
  int start = 0;
  int end = 0;
  pageBounds(next, start, end);
  if (start == pageStart && end == pageEnd) {
    focus = next;  // same page: the render repaints only the two cells
  } else {
    RenderLock lock(*this);
    focus = next;
    preparePage();
  }
  requestUpdate();
}

void LibraryActivity::movePage(const int direction) {
  RenderLock lock(*this);
  if (view == View::Shelf) {
    const int target =
        (shelfOf(focus) / library_layout::SHELVES_PER_PAGE + direction) * library_layout::SHELVES_PER_PAGE;
    if (target < 0 || target >= static_cast<int>(shelfStarts.size())) return;
    focus = shelfStarts[target];
  } else {
    const int per = pageSize();
    const int start = focus / per * per + direction * per;
    if (start < 0 || start >= model.count()) return;
    focus = std::min(model.count() - 1, start + focus % per);
  }
  preparePage();
  requestUpdate();
}

void LibraryActivity::setView(const View next) {
  if (next == view) return;
  RenderLock lock(*this);
  view = next;
  pageStart = pageEnd = -1;
  preparePage();
  savePrefs();
}

void LibraryActivity::savePrefs() const {
  LIBRARY_META.savePrefs(static_cast<uint8_t>(view), static_cast<uint8_t>(sort), static_cast<uint8_t>(filter));
}

void LibraryActivity::handleBarConfirm() {
  switch (barFocus) {
    case BAR_SHELF:
    case BAR_GRID:
    case BAR_LIST:
      setView(static_cast<View>(barFocus - BAR_SHELF));
      break;
    case BAR_SORT: {
      RenderLock lock(*this);
      sort = cycle(sort, LibraryMetaStore::SORT_COUNT);
      applyArrangement(true);
      savePrefs();
      break;
    }
    case BAR_FILTER: {
      RenderLock lock(*this);
      filter = cycle(filter, LibraryMetaStore::FILTER_COUNT);
      applyArrangement(false);
      savePrefs();
      break;
    }
    case BAR_SEARCH:
      index.close();
      activityManager.goToLibraryList();
      return;
    case BAR_GET_BOOKS:
      index.close();
      if (auto getBooks = makeUniqueNoThrow<GetBooksActivity>(renderer, mappedInput)) {
        activityManager.replaceActivity(std::move(getBooks));
      }
      return;
    default:
      break;
  }
  fullRedraw = true;
  requestUpdate();
}

void LibraryActivity::handleCardConfirm() {
  Item* item = focusedItem();
  if (!item) return;
  library_ui::Book& book = model.book(item->book);
  switch (cardAction) {
    case ACTION_OPEN:
      openBook(item->path);
      return;
    case ACTION_FINISHED: {
      const uint8_t flags = LIBRARY_META.toggle(item->path, LibraryMetaStore::FINISHED);
      book.flags = (book.flags & ~library_ui::Book::MARKED_FINISHED) |
                   ((flags & LibraryMetaStore::FINISHED) ? library_ui::Book::MARKED_FINISHED : 0);
      orderDirty = true;
      break;
    }
    case ACTION_ARCHIVE: {
      const uint8_t flags = LIBRARY_META.toggle(item->path, LibraryMetaStore::ARCHIVED);
      book.flags = (book.flags & ~library_ui::Book::ARCHIVED) |
                   ((flags & LibraryMetaStore::ARCHIVED) ? library_ui::Book::ARCHIVED : 0);
      // The book leaves (or joins) the current shelf: close the card on the new
      // order, focus staying on the slot it left. `item` dies with the page.
      RenderLock lock(*this);
      cardOpen = false;
      applyArrangement(true);
      break;
    }
    default:
      break;
  }
  fullRedraw = true;
  requestUpdate();
}

void LibraryActivity::openBook(const std::string& path) {
  const std::string target = path;  // `path` may live in `items`, which onExit frees
  index.close();
  onSelectBook(target);
}

void LibraryActivity::loop() {
  using Button = MappedInputManager::Button;
  if (loadFailed) {
    if (mappedInput.wasReleased(Button::Back)) onGoHome();
    return;
  }
  if (lockConfirmRelease) {
    if (mappedInput.wasReleased(Button::Confirm) || !mappedInput.isPressed(Button::Confirm)) lockConfirmRelease = false;
    return;
  }

  if (zone == Zone::Books && !cardOpen && model.count() > 0 &&
      mappedInput.wasLongPressed(Button::Confirm, HOLD_TO_OPEN_MS)) {
    if (Item* item = focusedItem()) openBook(item->path);
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    if (cardOpen) {
      handleCardConfirm();
    } else if (zone == Zone::Bar) {
      handleBarConfirm();
    } else if (model.count() > 0) {
      cardOpen = true;
      cardAction = ACTION_OPEN;
      fullRedraw = true;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    if (cardOpen) {
      cardOpen = false;
      if (orderDirty) {
        RenderLock lock(*this);
        applyArrangement(true);
      }
      fullRedraw = true;
      requestUpdate();
    } else if (zone == Zone::Bar) {
      zone = Zone::Books;
      fullRedraw = true;
      requestUpdate();
    } else {
      index.close();
      onGoHome();
    }
    return;
  }
  if (mappedInput.wasPressed(Button::Up)) {
    if (cardOpen) {
      moveFocus(-1);
    } else if (zone == Zone::Books) {
      if (pageNumber() == 0) {
        zone = Zone::Bar;
        barFocus = static_cast<int>(view);
        fullRedraw = true;
        requestUpdate();
      } else {
        movePage(-1);
      }
    }
    return;
  }
  if (mappedInput.wasPressed(Button::Down)) {
    if (cardOpen) {
      moveFocus(+1);
    } else if (zone == Zone::Bar) {
      zone = Zone::Books;
      fullRedraw = true;
      requestUpdate();
    } else {
      movePage(+1);
    }
    return;
  }

  bool stepped = false;
  const auto step = [this, &stepped](const int direction) {
    stepped = true;
    if (cardOpen) {
      cardAction = (cardAction + ACTION_COUNT + direction) % ACTION_COUNT;
      fullRedraw = true;
      requestUpdate();
    } else if (zone == Zone::Bar) {
      barFocus = (barFocus + BAR_COUNT + direction) % BAR_COUNT;
      fullRedraw = true;
      requestUpdate();
    } else {
      moveFocus(direction);
    }
  };
  navigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&step] { step(-1); });
  navigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&step] { step(+1); });
  if (stepped) return;

  // Idle: fill in covers the page is still missing, then show them at once.
  if (generateNextThumb()) {
    bool more = false;
    for (int i = 0; i < static_cast<int>(items.size()) && !more; i++) {
      const uint8_t wanted = thumbsWanted(i);
      const uint8_t tried = items[i].thumbs;
      more = ((wanted & Item::COVER_READY) && !(tried & (Item::COVER_READY | Item::COVER_TRIED))) ||
             ((wanted & Item::ROW_READY) && !(tried & (Item::ROW_READY | Item::ROW_TRIED)));
    }
    if (!more) {
      fullRedraw = true;
      requestUpdate();
    }
  }
}
