// Drawing half of LibraryActivity: header bar, Shelf / Grid / List views and
// the button hints. Everything here reads state only; loop() owns mutation.
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "LibraryActivity.h"
#include "LibraryArt.h"
#include "LibraryLayout.h"
#include "MappedInputManager.h"
#include "components/GeneratedCover.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookThumbs.h"

using library_ui::Filter;
using library_ui::Sort;
using library_ui::View;
namespace art = library_art;
namespace layout = library_layout;

namespace {
constexpr int BAR_Y = 48;
constexpr int BAR_H = 34;
constexpr int FOCUS_FRAME = 3;

const char* sortLabel(const Sort sort) {
  switch (sort) {
    case Sort::Title:
      return tr(STR_LIBRARY_TAB_TITLE);
    case Sort::Author:
      return tr(STR_LIBRARY_TAB_AUTHOR);
    case Sort::Progress:
      return tr(STR_LIB_SORT_PROGRESS);
    case Sort::Recent:
    default:
      return tr(STR_LIBRARY_TAB_RECENT);
  }
}

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

const char* viewLabel(const int v) {
  if (v == 1) return tr(STR_LIB_VIEW_GRID);
  if (v == 2) return tr(STR_LIB_VIEW_LIST);
  return tr(STR_LIB_VIEW_SHELF);
}

// Small glyphs drawn in the text colour at (x, y) inside a 14x14 box.
void glyphSort(const GfxRenderer& r, const int x, const int y, const bool black) {
  r.fillRect(x, y + 2, 14, 2, black);
  r.fillRect(x, y + 6, 10, 2, black);
  r.fillRect(x, y + 10, 6, 2, black);
}
void glyphFilter(const GfxRenderer& r, const int x, const int y, const bool black) {
  const int xs[] = {x, x + 14, x + 8, x + 8, x + 6, x + 6};
  const int ys[] = {y + 1, y + 1, y + 7, y + 13, y + 13, y + 7};
  r.fillPolygon(xs, ys, 6, black);
}
void glyphSearch(const GfxRenderer& r, const int x, const int y, const bool black) {
  r.drawRoundedRect(x, y, 10, 10, 2, 5, black);
  r.drawLine(x + 9, y + 9, x + 13, y + 13, 2, black);
}
}  // namespace

int LibraryActivity::contentTop() const { return layout::HEADER_HEIGHT + 4; }

int LibraryActivity::contentBottom() const {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 4;
}

int LibraryActivity::gutter() const { return layout::SIDE_GUTTER; }

// --- header --------------------------------------------------------------------

void LibraryActivity::drawBarItem(const int item, const int x, const int y, const int w, const int h, const char* label,
                                  const bool active) const {
  const bool isView = item <= BAR_LIST;
  if (active) {
    renderer.fillRect(x, y, w, h, true);
  } else if (!isView) {
    renderer.drawRoundedRect(x, y, w, h, 1, 8, true);
  }
  const int glyph = isView ? 0 : (label[0] ? 20 : 14);
  const int tw = label[0] ? renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) : 0;
  const int tx = x + (w - tw - glyph) / 2 + glyph;
  const int gy = y + (h - 14) / 2;
  if (item == BAR_SORT) glyphSort(renderer, tx - glyph, gy, !active);
  if (item == BAR_FILTER) glyphFilter(renderer, tx - glyph, gy, !active);
  if (item == BAR_SEARCH) glyphSearch(renderer, tx - glyph, gy, !active);
  if (label[0]) {
    renderer.drawText(UI_10_FONT_ID, tx, y + (h - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label, !active,
                      EpdFontFamily::BOLD);
  }
  if (zone == Zone::Bar && barFocus == item) {
    renderer.drawRoundedRect(x - 4, y - 4, w + 8, h + 8, FOCUS_FRAME, 10, true);
  }
}

void LibraryActivity::drawHeader() const {
  const int w = renderer.getScreenWidth();
  renderer.drawText(UI_12_FONT_ID, 18, 10, tr(STR_LIBRARY), true, EpdFontFamily::BOLD);
  char info[48];
  const int count = model.count();
  const int written = snprintf(info, sizeof(info), tr(STR_LIBRARY_BOOK_COUNT), count);
  if (pageTotal() > 1 && written > 0 && written < static_cast<int>(sizeof(info))) {
    snprintf(info + written, sizeof(info) - written, "   %d/%d", pageNumber() + 1, pageTotal());
  }
  const int iw = renderer.getTextWidth(UI_10_FONT_ID, info);
  renderer.drawText(UI_10_FONT_ID, w - 18 - iw, 14, info);

  // View segments share one outlined capsule; Sort, Filter and Search are chips.
  int x = 16;
  int segW[3];
  int total = 0;
  for (int v = 0; v < 3; v++) {
    segW[v] = renderer.getTextWidth(UI_10_FONT_ID, viewLabel(v), EpdFontFamily::BOLD) + 18;
    total += segW[v];
  }
  renderer.drawRoundedRect(x, BAR_Y, total, BAR_H, 1, 8, true);
  for (int v = 0; v < 3; v++) {
    if (v > 0) renderer.fillRect(x, BAR_Y + 6, 1, BAR_H - 12, true);
    drawBarItem(BAR_SHELF + v, x, BAR_Y, segW[v], BAR_H, viewLabel(v), static_cast<int>(view) == v);
    x += segW[v];
  }
  const auto chipWidth = [this](const char* label) {
    return renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) + 30;
  };
  x += 10;
  const int sortW = chipWidth(sortLabel(sort));
  drawBarItem(BAR_SORT, x, BAR_Y, sortW, BAR_H, sortLabel(sort), false);
  x += sortW + 8;
  drawBarItem(BAR_FILTER, x, BAR_Y, chipWidth(filterLabel(filter)), BAR_H, filterLabel(filter), false);
  // Search is a glyph-only square: the row must fit "Progress" and "Archived".
  drawBarItem(BAR_SEARCH, w - 16 - BAR_H, BAR_Y, BAR_H, BAR_H, "", false);
  renderer.fillRect(16, layout::HEADER_HEIGHT - 3, w - 32, 1, true);
}

// --- shared --------------------------------------------------------------------

bool LibraryActivity::drawThumb(const Item& item, const int height, const int x, const int y,
                                const int slotWidth) const {
  const uint8_t ready = height == ROW_COVER_HEIGHT ? Item::ROW_READY : Item::COVER_READY;
  if (item.thumbs & ready) {
    HalFile file;
    if (Storage.openFileForRead("LIB", book_thumbs::thumbPath(item.path, height), file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
        const int w = std::min(slotWidth, bitmap.getWidth() * height / bitmap.getHeight());
        const int bx = x + (slotWidth - w) / 2;
        renderer.drawBitmap(bitmap, bx, y, w, height);
        renderer.drawRect(bx, y, w, height, 1, true);
        if (model.book(item.book).archived()) art::drawArchiveStamp(renderer, bx, y, w, height);
        return true;
      }
    }
  }
  art::drawCoverPlaceholder(renderer, x, y, slotWidth, height);
  return false;
}

void LibraryActivity::drawEmpty() const {
  const int mid = (contentTop() + contentBottom()) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, mid - 20, tr(STR_LIB_EMPTY_FILTER), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, mid + 12, filterLabel(filter));
}

// --- shelf ---------------------------------------------------------------------

void LibraryActivity::drawShelfView() {
  const int w = renderer.getScreenWidth();
  const int x0 = gutter() - layout::SHELF_OUTSET;
  const int x1 = w - x0;
  const int top = contentTop();
  const int lastBoard = contentBottom() - art::PLANK_HEIGHT - 4;
  const int boards[] = {lastBoard - 2 * (art::PLANK_HEIGHT + layout::SHELF_COMPARTMENT),
                        lastBoard - (art::PLANK_HEIGHT + layout::SHELF_COMPARTMENT), lastBoard};

  art::drawBackPanel(renderer, x0 + layout::SHELF_UPRIGHT, top + 5, x1 - x0 - 2 * layout::SHELF_UPRIGHT,
                     lastBoard - top - 5);
  renderer.fillRect(x0, top, x1 - x0, 5, true);
  drawShelfDisplay(top + 5, boards[0]);
  for (int b = 0; b < 3; b++) art::drawPlank(renderer, x0, boards[b], x1 - x0, 0x51f3u * (b + 1));
  art::drawUpright(renderer, x0, top, layout::SHELF_UPRIGHT, lastBoard + art::PLANK_HEIGHT - top, 7);
  art::drawUpright(renderer, x1 - layout::SHELF_UPRIGHT, top, layout::SHELF_UPRIGHT,
                   lastBoard + art::PLANK_HEIGHT - top, 11);

  if (model.count() == 0) return;
  const int firstShelf = shelfOf(pageStart);
  const int shelves = static_cast<int>(shelfStarts.size());
  for (int s = 0; s < layout::SHELVES_PER_PAGE && firstShelf + s < shelves; s++) {
    const int from = shelfStarts[firstShelf + s];
    const int to = firstShelf + s + 1 < shelves ? shelfStarts[firstShelf + s + 1] : model.count();
    int x = x0 + layout::SHELF_UPRIGHT + 1;
    for (int p = from; p < to; p++) {
      const Item* item = itemAt(p);
      if (!item) continue;
      const library_ui::Book& book = model.book(item->book);
      art::Spine spine{};
      spine.x = x;
      spine.baseY = boards[s + 1];
      spine.width = art::spineWidthFor(book.fileSize);
      spine.seed = generated_cover::seedFor(item->title.c_str());
      spine.height = art::spineHeightFor(spine.seed, layout::SPINE_MAX_HEIGHT);
      spine.sig = book.spineSig;
      spine.title = item->title.c_str();
      spine.author = item->author.c_str();
      spine.archived = book.archived();
      spine.finished = book.finished();
      spine.inProgress = book.progress > 0;
      spine.focused = p == focus && zone == Zone::Books;
      art::drawSpine(renderer, spine);
      x += spine.width + layout::SPINE_GAP;
    }
    if (x + 26 < x1 - layout::SHELF_UPRIGHT) art::drawBookend(renderer, x + 1, boards[s + 1], 70);
  }
}

// The focused book stands face-out in the top compartment beside a shelf-talker
// card with its details, the way a bookshop features a title.
void LibraryActivity::drawShelfDisplay(const int top, const int bottom) {
  const Item* item = itemAt(focus);
  const int x0 = gutter() - layout::SHELF_OUTSET + layout::SHELF_UPRIGHT;
  const int x1 = renderer.getScreenWidth() - x0;
  if (!item) {
    if (model.count() == 0) {
      renderer.fillRect(x0 + 40, top + 50, x1 - x0 - 80, 110, false);
      renderer.drawRect(x0 + 40, top + 50, x1 - x0 - 80, 110, 1, true);
      renderer.drawCenteredText(UI_12_FONT_ID, top + 80, tr(STR_LIB_EMPTY_FILTER), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + 112, filterLabel(filter));
    }
    return;
  }
  const library_ui::Book& book = model.book(item->book);
  const int coverW = book_thumbs::generatedWidth(COVER_HEIGHT);
  const int coverX = x0 + 14;
  const int coverY = bottom - COVER_HEIGHT;
  renderer.fillRectDither(coverX + 5, coverY + 5, coverW, COVER_HEIGHT - 5, Color::DarkGray);
  renderer.fillRect(coverX, coverY, coverW, COVER_HEIGHT, false);
  drawThumb(*item, COVER_HEIGHT, coverX, coverY, coverW);

  // Shelf talker.
  const int cx = coverX + coverW + 20;
  const int cy = top + 12;
  const int cw = x1 - 12 - cx;
  const int ch = bottom - 14 - cy;
  renderer.fillRectDither(cx + 4, cy + 4, cw, ch, Color::DarkGray);
  renderer.fillRect(cx, cy, cw, ch, false);
  renderer.drawRect(cx, cy, cw, ch, 1, true);
  renderer.fillRoundedRect(cx + cw / 2 - 5, cy - 5, 10, 10, 5, Color::Black);

  const int pad = 12;
  const int tx = cx + pad;
  const int textW = cw - pad * 2;
  int y = cy + 12;
  const auto lines = renderer.wrappedText(NOTOSERIF_14_FONT_ID, item->title.c_str(), textW, 2, EpdFontFamily::BOLD);
  for (const auto& line : lines) {
    renderer.drawText(NOTOSERIF_14_FONT_ID, tx, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(NOTOSERIF_14_FONT_ID);
  }
  if (!item->author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, item->author.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, tx, y + 2, author.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
  }
  y += 10;
  char line[48];
  if (book.progress >= 0) {
    art::drawProgressBar(renderer, tx, y + 6, textW - 56, 7, book.progress);
    snprintf(line, sizeof(line), "%d%%", book.progress);
    const int pw = renderer.getTextWidth(UI_10_FONT_ID, line, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, tx + textW - pw, y, line, true, EpdFontFamily::BOLD);
    y += 24;
  } else if (!(book.flags & library_ui::Book::OPENED)) {
    renderer.drawText(UI_10_FONT_ID, tx, y, tr(STR_LIB_NOT_STARTED));
    y += 24;
  }
  char value[32];
  art::formatDuration(value, sizeof(value), book.readSeconds);
  if (value[0]) {
    snprintf(line, sizeof(line), "%s  %s", tr(STR_LIB_TIME_READ), value);
    renderer.drawText(SMALL_FONT_ID, tx, y, line);
    y += 18;
  }
  if (book.lastRead) {
    art::formatLastRead(value, sizeof(value), book.lastRead);
    snprintf(line, sizeof(line), "%s  %s", tr(STR_LIB_LAST_READ), value);
    renderer.drawText(SMALL_FONT_ID, tx, y, line);
  }
  const char* status =
      book.archived() ? tr(STR_LIB_FILTER_ARCHIVED) : (book.finished() ? tr(STR_LIB_FILTER_FINISHED) : nullptr);
  const int footY = cy + ch - 22;
  if (status) {
    art::drawCheck(renderer, tx, footY + 2, 10);
    renderer.drawText(SMALL_FONT_ID, tx + 16, footY, status, true, EpdFontFamily::BOLD);
  }
  const char* hold = tr(STR_LIB_HOLD_TO_OPEN);
  renderer.drawText(SMALL_FONT_ID, tx + textW - renderer.getTextWidth(SMALL_FONT_ID, hold), footY, hold);
}

// --- grid ----------------------------------------------------------------------

void LibraryActivity::drawGridCell(const int position, const bool focused) {
  const Item* item = itemAt(position);
  if (!item) return;
  const int w = renderer.getScreenWidth();
  const int coverW = book_thumbs::generatedWidth(COVER_HEIGHT);
  const int colGap = (w - 2 * gutter() - layout::GRID_COLUMNS * coverW) / (layout::GRID_COLUMNS - 1);
  const int rowPitch = (contentBottom() - contentTop()) / layout::GRID_ROWS;
  const int index = position - pageStart;
  const int x = gutter() + (index % layout::GRID_COLUMNS) * (coverW + colGap);
  const int y = contentTop() + (index / layout::GRID_COLUMNS) * rowPitch + 8;

  renderer.fillRect(x - 8, y - 8, coverW + 16, rowPitch, false);
  drawThumb(*item, COVER_HEIGHT, x, y, coverW);
  const library_ui::Book& book = model.book(item->book);
  int by = y + COVER_HEIGHT + 7;
  if (book.progress >= 0 || book.finished()) {
    const int pct = book.finished() ? 100 : book.progress;
    art::drawProgressBar(renderer, x, by, book.finished() ? coverW - 14 : coverW, 6, pct);
    if (book.finished()) art::drawCheck(renderer, x + coverW - 10, by - 3, 10);
  }
  by += 10;
  const std::string title = renderer.truncatedText(SMALL_FONT_ID, item->title.c_str(), coverW,
                                                   focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  renderer.drawText(SMALL_FONT_ID, x, by, title.c_str(), true, focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (focused) {
    renderer.drawRoundedRect(x - 6, y - 6, coverW + 12, COVER_HEIGHT + 12, FOCUS_FRAME, 6, true);
  }
}

void LibraryActivity::drawGridView() {
  for (int p = pageStart; p < pageEnd; p++) drawGridCell(p, p == focus && zone == Zone::Books);
}

// --- list ----------------------------------------------------------------------

void LibraryActivity::drawListRow(const int position, const bool focused) {
  const Item* item = itemAt(position);
  if (!item) return;
  const int x0 = gutter();
  const int x1 = renderer.getScreenWidth() - gutter();
  const int y = contentTop() + (position - pageStart) * layout::LIST_ROW_HEIGHT;
  const int rowH = layout::LIST_ROW_HEIGHT;
  const library_ui::Book& book = model.book(item->book);
  renderer.fillRect(x0 - 8, y, x1 - x0 + 16, rowH, false);

  const int thumbW = book_thumbs::generatedWidth(ROW_COVER_HEIGHT);
  drawThumb(*item, ROW_COVER_HEIGHT, x0 + 2, y + (rowH - ROW_COVER_HEIGHT) / 2, thumbW);

  const int rightW = 72;
  const int tx = x0 + thumbW + 16;
  const int textW = x1 - rightW - 8 - tx;
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, item->title.c_str(), textW, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, tx, y + 6, title.c_str(), true, EpdFontFamily::BOLD);
  if (!item->author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, item->author.c_str(), textW);
    renderer.drawText(UI_10_FONT_ID, tx, y + 32, author.c_str());
  }
  char duration[24];
  char last[32];
  char meta[64];
  art::formatDuration(duration, sizeof(duration), book.readSeconds);
  last[0] = '\0';
  if (book.lastRead) art::formatLastRead(last, sizeof(last), book.lastRead);
  snprintf(meta, sizeof(meta), "%s%s%s", duration, duration[0] && last[0] ? "  \xC2\xB7  " : "", last);
  if (meta[0]) renderer.drawText(SMALL_FONT_ID, tx, y + 55, meta);

  const int rx = x1 - rightW;
  if (book.archived()) {
    const char* stamp = tr(STR_LIB_READ_STAMP);
    const int sw = renderer.getTextWidth(SMALL_FONT_ID, stamp, EpdFontFamily::BOLD) + 12;
    renderer.fillRect(x1 - sw, y + 12, sw, 18, true);
    renderer.drawText(SMALL_FONT_ID, x1 - sw + 6, y + 13, stamp, false, EpdFontFamily::BOLD);
  } else if (book.finished()) {
    art::drawCheck(renderer, x1 - 18, y + 14, 14);
  } else if (book.progress >= 0) {
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", book.progress);
    const int pw = renderer.getTextWidth(UI_12_FONT_ID, pct, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x1 - pw, y + 6, pct, true, EpdFontFamily::BOLD);
  }
  if (book.progress >= 0 && !book.archived()) {
    art::drawProgressBar(renderer, rx, y + 40, rightW, 6, book.finished() ? 100 : book.progress);
  }

  if (focused) {
    renderer.drawRoundedRect(x0 - 5, y + 2, x1 - x0 + 10, rowH - 4, 2, 8, true);
  } else {
    for (int px = tx; px < x1; px += 3) renderer.drawPixel(px, y + rowH - 1, true);
  }
}

void LibraryActivity::drawListView() {
  for (int p = pageStart; p < pageEnd; p++) drawListRow(p, p == focus && zone == Zone::Books);
}

// --- frame ---------------------------------------------------------------------

void LibraryActivity::drawHints() const {
  const char* back;
  const char* confirm;
  const char* up = "";
  const char* down = "";
  if (cardOpen) {
    back = tr(STR_LIB_CLOSE);
    confirm = tr(STR_SELECT);
    up = tr(STR_LIB_PREV_BOOK);
    down = tr(STR_LIB_NEXT_BOOK);
  } else if (zone == Zone::Bar) {
    back = tr(STR_LIB_BOOKS);
    confirm =
        barFocus == BAR_SEARCH ? tr(STR_LIBRARY_SEARCH) : (barFocus <= BAR_LIST ? tr(STR_SELECT) : tr(STR_LIB_CHANGE));
    down = tr(STR_LIB_BOOKS);
  } else {
    back = tr(STR_HOME_SHORTCUT);
    confirm = model.count() > 0 ? tr(STR_LIB_DETAILS) : "";
    up = pageNumber() == 0 ? tr(STR_LIB_VIEWS) : tr(STR_LIB_PAGE_UP);
    down = pageNumber() + 1 < pageTotal() ? tr(STR_LIB_PAGE_DOWN) : "";
  }
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_LIB_PREV), tr(STR_LIB_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, up, down);
}

void LibraryActivity::render(RenderLock&&) {
  // Taken first: a request that lands while this frame draws must survive it.
  const bool full = fullRedraw;
  fullRedraw = false;

  if (loadFailed) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 20, tr(STR_LIBRARY_REBUILD_FAILED), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_HOME_SHORTCUT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const bool differential = !full && !cardOpen && zone == Zone::Books && view != View::Shelf &&
                            drawnPageStart == pageStart && drawnFocus >= 0 && drawnFocus != focus;
  if (differential) {
    if (view == View::Grid) {
      drawGridCell(drawnFocus, false);
      drawGridCell(focus, true);
    } else {
      drawListRow(drawnFocus, false);
      drawListRow(focus, true);
    }
  } else {
    renderer.clearScreen();
    drawHeader();
    if (view == View::Shelf) {
      drawShelfView();
    } else if (model.count() == 0) {
      drawEmpty();
    } else if (view == View::Grid) {
      drawGridView();
    } else {
      drawListView();
    }
    if (cardOpen) drawDetailCard();
    drawHints();
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  drawnFocus = focus;
  drawnPageStart = pageStart;
}
