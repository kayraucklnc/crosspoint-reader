#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <OrderedDither.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "LibraryMetaStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/CoverTilt.h"
#include "components/UITheme.h"
#include "components/icons/income.h"
#include "components/icons/play.h"
#include "components/icons/settings2.h"
#include "components/icons/trains.h"
#include "fontIds.h"
#include "util/BookProgress.h"
#include "util/BookThumbs.h"

namespace {
constexpr int TOP_PAD = 36;
constexpr int GAP = 22;    // air between the centre cover and the turned neighbours
constexpr int SHADOW = 7;  // dithered drop shadow under the centre cover
constexpr int FOCUS_FRAME = 3;
constexpr int ICON_SIZE = 32;
// Neighbour covers are lightened by clearing a 4x4 Bayer share of their pixels.
constexpr int RECESS_LEVEL = 9;
constexpr uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void lightenRect(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  for (int py = y; py < y + h; py++) {
    for (int px = x; px < x + w; px++) {
      if (BAYER4[py & 3][px & 3] < RECESS_LEVEL) renderer.drawPixel(px, py, false);
    }
  }
}

// Stacked offset outlines darkening toward the cover read as a soft shadow.
void lightenShadow(const GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                   const int strength) {
  for (int py = y; py < y + h; py++) {
    for (int px = x + w - 2; px < x + w; px++) {
      if (BAYER4[py & 3][px & 3] < strength * 2) renderer.drawPixel(px, py, true);
    }
  }
  for (int px = x; px < x + w; px++) {
    for (int py = y + h - 2; py < y + h; py++) {
      if (BAYER4[py & 3][px & 3] < strength * 2) renderer.drawPixel(px, py, true);
    }
  }
}

// Truncates `text` with "..." so it fits maxWidth.
void fitText(const GfxRenderer& renderer, const int fontId, const char* text, const EpdFontFamily::Style style,
             const int maxWidth, char* out, const size_t outSize) {
  snprintf(out, outSize, "%s", text);
  size_t len = strlen(out);
  while (len > 3 && renderer.getTextWidth(fontId, out, style) > maxWidth) {
    len--;
    // Step back over UTF-8 continuation bytes so we never split a character.
    while (len > 0 && (static_cast<uint8_t>(out[len]) & 0xC0) == 0x80) len--;
    snprintf(out + len, outSize - len, "...");
  }
}
}  // namespace

int HomeActivity::centerCoverHeight() const { return renderer.getScreenHeight() * 44 / 100; }
// Neighbours sit a step back: their near edge is shorter than the centre cover.
int HomeActivity::sideCoverHeight() const { return centerCoverHeight() * 86 / 100; }

void HomeActivity::onEnter() {
  Activity::onEnter();
  ordered_dither::mode = SETTINGS.imageDithering;

  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min<size_t>(books.size(), MAX_CAROUSEL_BOOKS));
  for (const RecentBook& book : books) {
    if (static_cast<int>(recentBooks.size()) >= MAX_CAROUSEL_BOOKS) break;
    if (!RecentBooksStore::isMissing(book) && !LIBRARY_META.isArchived(book.path)) recentBooks.push_back(book);
  }
  progressCache.assign(recentBooks.size(), -2);

  // The card shows the Library's size; reading the index header is one sector.
  {
    library::LibraryIndexFile index;
    if (!library::isLibraryIndexDirty() && index.open(library::libraryIndexPath())) {
      libraryBookCount = index.bookCount();
    }
  }

  // Coming back from the Library lands on its card.
  bookIndex = initialMenuItem == HomeMenuItem::LIBRARY ? static_cast<int>(recentBooks.size()) : 0;
  highlight = NO_HIGHLIGHT;
  if (initialMenuItem == HomeMenuItem::INCOME) highlight = 0;
  if (initialMenuItem == HomeMenuItem::TRAINS) highlight = 1;
  if (initialMenuItem == HomeMenuItem::PLAY) highlight = 2;
  if (initialMenuItem == HomeMenuItem::SETTINGS_MENU) highlight = 3;
  coversPending = !recentBooks.empty();
  requestUpdate();
}

std::string HomeActivity::thumbPath(const RecentBook& book, const int height) const {
  return book.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(book.coverBmpPath, height);
}

bool HomeActivity::ensureThumb(RecentBook& book, const int height) {
  book.coverBmpPath = book_thumbs::cacheDirFor(book.path) + "/thumb_[HEIGHT].bmp";
  cover_tilt::invalidateIfDitherChanged(book_thumbs::thumbPath(book.path, height));
  // Cover-less books get a generated cover, which borrows the renderer.
  RenderLock lock(*this);
  return book_thumbs::ensure(renderer, book.path, height, book.title.c_str(), book.author.c_str());
}

// Thumbnails at the exact drawn sizes plus both tilted variants for the centre
// and its neighbours (the centre's are used by the page-turn frame). Cached on SD.
void HomeActivity::ensureCovers() {
  const int count = static_cast<int>(recentBooks.size());
  for (int offset = -1; offset <= 1; offset++) {
    const int i = bookIndex + offset;
    if (i < 0 || i >= count) continue;
    RecentBook& book = recentBooks[i];
    const bool centre = ensureThumb(book, centerCoverHeight());
    const bool side = ensureThumb(book, sideCoverHeight());
    if (side) {
      cover_tilt::ensure(thumbPath(book, sideCoverHeight()), cover_tilt::Side::LEFT);
      cover_tilt::ensure(thumbPath(book, sideCoverHeight()), cover_tilt::Side::RIGHT);
    }
    if (!centre && !side) book.coverBmpPath.clear();
  }
  coversPending = false;
}

int HomeActivity::bookProgress(const int index) {
  if (progressCache[index] == -2) progressCache[index] = loadBookProgress(recentBooks[index].path);
  return progressCache[index];
}

void HomeActivity::activate() {
  switch (highlight) {
    case 0:
      activityManager.goToIncome();
      return;
    case 1:
      activityManager.goToTrains();
      return;
    case 2:
      activityManager.goToPlay();
      return;
    case 3:
      activityManager.goToSettings();
      return;
    default:
      if (onLibraryCard()) {
        activityManager.goToLibrary();
      } else {
        activityManager.goToReader(recentBooks[bookIndex].path);
      }
  }
}

void HomeActivity::flipBook(const int direction) {
  const int count = static_cast<int>(recentBooks.size());
  const int next = std::clamp(bookIndex + direction, 0, count);  // count = the Library card
  if (next == bookIndex) return;
  const bool involvesCard = next == count || bookIndex == count;
  bookIndex = next;
  // The card is a destination, not a cover: it never swings through the pull frame.
  if (SETTINGS.homeAnimation && firstRenderDone && !involvesCard) {
    // One intermediate frame; the final frame follows once covers are ready.
    transitionDir = direction;
    requestUpdateAndWait();
    transitionDir = 0;
  }
  coversPending = true;
  requestUpdate();
}

void HomeActivity::moveHighlight(const int direction) {
  if (highlight == NO_HIGHLIGHT) {
    highlight = direction > 0 ? 0 : ROW_COUNT - 1;
  } else {
    highlight = (highlight + ROW_COUNT + direction) % ROW_COUNT;
  }
  requestUpdate();
}

void HomeActivity::loop() {
  if (coversPending && firstRenderDone) {
    ensureCovers();
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (highlight != NO_HIGHLIGHT) {
      highlight = NO_HIGHLIGHT;
      requestUpdate();
    } else if (!recentBooks.empty()) {
      activityManager.goToReader(recentBooks[0].path);  // resume the most recent book
    }
    return;
  }

  // Side page buttons (honouring the side-button layout setting) flip books.
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    flipBook(+1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    flipBook(-1);
    return;
  }
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveHighlight(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveHighlight(+1); });
}

bool HomeActivity::drawCover(const RecentBook& book, const int height, const int centerX, const int top,
                             int* outWidth) const {
  int width = height * 2 / 3;
  bool drawn = false;
  const std::string path = thumbPath(book, height);
  if (!path.empty()) {
    HalFile file;
    if (Storage.openFileForRead("HOME", path, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
        width = bitmap.getWidth() * height / bitmap.getHeight();
        drawn = renderer.drawBitmap(bitmap, centerX - width / 2, top, width, height);
      }
    }
  }
  const int x = centerX - width / 2;
  if (!drawn) {
    // No cover art: a plain card with the title.
    renderer.fillRect(x, top, width, height, false);
    renderer.drawRect(x, top, width, height, 2, true);
    char title[48];
    fitText(renderer, UI_10_FONT_ID, book.title.c_str(), EpdFontFamily::BOLD, width - 16, title, sizeof(title));
    const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, title, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, centerX - titleWidth / 2, top + height / 2 - 10, title, true, EpdFontFamily::BOLD);
  }
  renderer.drawRect(x, top, width, height, 1, true);
  if (outWidth) *outWidth = width;
  return drawn;
}

// Blits a pre-tilted neighbour: its near (tall) edge at nearX, far edge outward.
bool HomeActivity::drawTilted(const RecentBook& book, const bool leftSide, const int nearX, const int top,
                              int* outWidth) const {
  const std::string flat = thumbPath(book, sideCoverHeight());
  if (flat.empty()) return false;
  const std::string path = cover_tilt::pathFor(flat, leftSide ? cover_tilt::Side::LEFT : cover_tilt::Side::RIGHT);
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  const int w = bitmap.getWidth();
  const int h = bitmap.getHeight();
  const int x = leftSide ? nearX - w : nearX;
  // White outside the trapezoid is transparent so overlaps keep what is below.
  renderer.drawBitmap(bitmap, x, top, w, h, 0, 0, true);
  lightenRect(renderer, x, top, w, h);  // recede behind the centre cover
  if (outWidth) *outWidth = w;
  return true;
}

void HomeActivity::renderCarousel(const int top, const int height) {
  const int screenWidth = renderer.getScreenWidth();
  const int centerX = screenWidth / 2;
  const int centerHeight = centerCoverHeight();
  const int sideTop = top + (centerHeight - sideCoverHeight()) / 2;
  const int centerWidth = centerHeight * 2 / 3;
  const int count = static_cast<int>(recentBooks.size());

  // Neighbours stand apart from the centre cover; clip to the screen.
  const auto savedClip = renderer.getClipRect();
  renderer.setClipRect(0, 0, screenWidth, renderer.getScreenHeight());
  if (bookIndex > 0) drawTilted(recentBooks[bookIndex - 1], true, centerX - centerWidth / 2 - GAP, sideTop, nullptr);
  if (bookIndex + 1 < count) {
    drawTilted(recentBooks[bookIndex + 1], false, centerX + centerWidth / 2 + GAP, sideTop, nullptr);
  } else if (bookIndex + 1 == count) {
    // The Library card peeks in flat from the right edge, never tilted like a book.
    const int peekHeight = sideCoverHeight();
    drawLibraryCard(centerX + centerWidth / 2 + GAP + peekHeight / 3, sideTop, peekHeight, true);
  }
  renderer.setClipRect(savedClip[0], savedClip[1], savedClip[2], savedClip[3]);

  if (onLibraryCard()) {
    drawLibraryCard(centerX, top, centerHeight, false);
    if (highlight == NO_HIGHLIGHT) {
      const int pad = FOCUS_FRAME + 3;
      renderer.drawRoundedRect(centerX - centerWidth / 2 - pad, top - pad, centerWidth + pad * 2,
                               centerHeight + pad * 2, FOCUS_FRAME, 16, true);
    }
    renderDots(top, height);
    return;
  }

  // Soft dithered shadow down and to the right lifts the centre cover off the page.
  for (int i = SHADOW; i > 0; i--) {
    lightenShadow(renderer, centerX - centerWidth / 2 + i, top + i, centerWidth, centerHeight, SHADOW - i + 1);
  }
  int drawnWidth = centerWidth;
  drawCover(recentBooks[bookIndex], centerHeight, centerX, top, &drawnWidth);
  if (highlight == NO_HIGHLIGHT) {
    const int pad = FOCUS_FRAME + 3;
    renderer.drawRoundedRect(centerX - drawnWidth / 2 - pad, top - pad, drawnWidth + pad * 2, centerHeight + pad * 2,
                             FOCUS_FRAME, 6, true);
  }
  renderDots(top, height);
}

// One square per book, then a chevron for the Library card.
void HomeActivity::renderDots(const int top, const int height) const {
  constexpr int DOT = 6;
  constexpr int DOT_GAP = 8;
  const int count = static_cast<int>(recentBooks.size());
  const int slots = count + 1;
  const int centerX = renderer.getScreenWidth() / 2;
  const int dotsWidth = slots * DOT + (slots - 1) * DOT_GAP;
  const int dotsY = top + height - DOT;
  for (int i = 0; i < slots; i++) {
    const int dx = centerX - dotsWidth / 2 + i * (DOT + DOT_GAP);
    if (i == count) {
      const int w = i == bookIndex ? 2 : 1;
      renderer.drawLine(dx, dotsY - 1, dx + DOT - 1, dotsY + DOT / 2, w, true);
      renderer.drawLine(dx + DOT - 1, dotsY + DOT / 2, dx, dotsY + DOT, w, true);
    } else if (i == bookIndex) {
      renderer.fillRect(dx, dotsY, DOT, DOT, true);
    } else {
      renderer.drawRect(dx, dotsY, DOT, DOT, 1, true);
    }
  }
}

// The single "pull" frame: the previous centre book swings out toward its new
// side while the new one swings in from the other, both mid-way and tilted.
void HomeActivity::renderTransition(const int top) {
  const int screenWidth = renderer.getScreenWidth();
  const int centerX = screenWidth / 2;
  const int centerWidth = centerCoverHeight() * 2 / 3;
  const int sideTop = top + (centerCoverHeight() - sideCoverHeight()) / 2;
  const int half = centerWidth / 4;
  const auto savedClip = renderer.getClipRect();
  renderer.setClipRect(0, 0, screenWidth, renderer.getScreenHeight());
  const RecentBook& outgoing = recentBooks[bookIndex - transitionDir];
  const RecentBook& incoming = recentBooks[bookIndex];
  if (transitionDir > 0) {
    drawTilted(outgoing, true, centerX - half, sideTop, nullptr);
    drawTilted(incoming, false, centerX + half, sideTop, nullptr);
  } else {
    drawTilted(outgoing, false, centerX + half, sideTop, nullptr);
    drawTilted(incoming, true, centerX - half, sideTop, nullptr);
  }
  renderer.setClipRect(savedClip[0], savedClip[1], savedClip[2], savedClip[3]);
}

void HomeActivity::renderBookInfo(const int top) {
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - 64;
  const RecentBook& book = recentBooks[bookIndex];

  char line[96];
  fitText(renderer, UI_12_FONT_ID, book.title.c_str(), EpdFontFamily::BOLD, maxWidth, line, sizeof(line));
  renderer.drawCenteredText(UI_12_FONT_ID, top, line, true, EpdFontFamily::BOLD);
  if (!book.author.empty()) {
    fitText(renderer, UI_10_FONT_ID, book.author.c_str(), EpdFontFamily::REGULAR, maxWidth, line, sizeof(line));
    renderer.drawCenteredText(UI_10_FONT_ID, top + 30, line);
  }

  const int progress = bookProgress(bookIndex);
  if (progress >= 0) {
    constexpr int BAR_WIDTH = 160;
    constexpr int BAR_HEIGHT = 6;
    const int barX = (screenWidth - BAR_WIDTH) / 2;
    const int barY = top + 64;
    renderer.drawRect(barX, barY, BAR_WIDTH, BAR_HEIGHT, 1, true);
    renderer.fillRect(barX, barY, BAR_WIDTH * std::min(progress, 100) / 100, BAR_HEIGHT, true);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", progress);
    renderer.drawText(SMALL_FONT_ID, barX + BAR_WIDTH + 10, barY - 8, pct);
  }
}

void HomeActivity::drawLibraryCard(const int centerX, const int top, const int height, const bool peek) const {
  const int width = height * 2 / 3;
  const int x = centerX - width / 2;
  renderer.fillRoundedRect(x, top, width, height, 16, Color::White);
  renderer.drawRoundedRect(x, top, width, height, peek ? 1 : 2, 16, true);

  // Shelf glyph: a few spines, one leaning, on a board.
  const int unit = std::max(3, width / 36);
  const int glyphW = unit * 14;
  const int gx = centerX - glyphW / 2;
  const int baseY = top + height * (peek ? 40 : 30) / 100;
  constexpr int SPINES[][2] = {{2, 9}, {3, 11}, {2, 8}, {3, 10}};
  int sx = gx;
  for (int i = 0; i < 4; i++) {
    const int sw = SPINES[i][0] * unit;
    const int sh = SPINES[i][1] * unit;
    if (i == 2) {
      renderer.drawRect(sx, baseY - sh, sw, sh, 2, true);
    } else {
      renderer.fillRect(sx, baseY - sh, sw, sh, true);
    }
    sx += sw + unit / 2;
  }
  const int leanX[] = {sx + unit, sx + unit * 4, sx + unit * 7, sx + unit * 4};
  const int leanY[] = {baseY, baseY - unit * 9, baseY - unit * 8, baseY};
  renderer.fillPolygon(leanX, leanY, 4, true);
  renderer.fillRect(gx - unit, baseY, glyphW + unit * 4, std::max(2, unit / 2 + 1), true);

  // Chevron in a ring: reads as "go", never as cover art.
  const int ring = std::max(10, width / 9);
  const int cy = top + height - ring - (peek ? height / 6 : height / 9);
  renderer.drawRoundedRect(centerX - ring, cy - ring, ring * 2, ring * 2, 2, ring, true);
  const int arm = ring / 2;
  renderer.drawLine(centerX - arm / 2, cy - arm, centerX + arm / 2, cy, 3, true);
  renderer.drawLine(centerX + arm / 2, cy, centerX - arm / 2, cy + arm, 3, true);
  if (peek) return;

  const int labelY = baseY + (cy - ring - baseY) / 2 - 22;
  renderer.drawCenteredText(UI_12_FONT_ID, labelY, tr(STR_HOME_SEE_LIBRARY), true, EpdFontFamily::BOLD);
  char count[32];
  // An index holding fewer books than the carousel is stale; the Library
  // rebuilds it on entry, so until then promise nothing specific.
  const bool known = libraryBookCount > 0 && libraryBookCount >= static_cast<int>(recentBooks.size());
  if (known && libraryBookCount == 1) {
    snprintf(count, sizeof(count), "%s", tr(STR_LIBRARY_ONE_BOOK));
  } else if (known) {
    snprintf(count, sizeof(count), tr(STR_LIBRARY_BOOK_COUNT), libraryBookCount);
  } else {
    snprintf(count, sizeof(count), "%s", tr(STR_HOME_ALL_BOOKS));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, labelY + 30, count);
}

void HomeActivity::renderLibraryInfo(const int top) const {
  renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_LIBRARY), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 30,
                            recentBooks.empty() ? tr(STR_HOME_NO_BOOKS) : tr(STR_HOME_LIBRARY_HINT));
}

void HomeActivity::renderTile(const int x, const int y, const int width, const int height, const uint8_t* icon,
                              const char* label, const bool selected) const {
  renderer.drawRoundedRect(x, y, width, height, selected ? FOCUS_FRAME : 1, 10, true);
  const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
  const int contentWidth = ICON_SIZE + 12 + labelWidth;
  const int cx = x + (width - contentWidth) / 2;
  renderer.drawIcon(icon, cx, y + (height - ICON_SIZE) / 2, ICON_SIZE);
  renderer.drawText(UI_12_FONT_ID, cx + ICON_SIZE + 12, y + (height - 26) / 2, label, true,
                    selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

int HomeActivity::renderAppRows(const int bottom) const {
  constexpr int SIDE = 28;
  constexpr int GAP = 14;
  constexpr int TILE_HEIGHT = 60;
  constexpr int SETTINGS_HEIGHT = 46;
  const int screenWidth = renderer.getScreenWidth();
  const int rowWidth = screenWidth - SIDE * 2;
  const int settingsTop = bottom - SETTINGS_HEIGHT;
  const int appsTop = settingsTop - GAP - TILE_HEIGHT;
  const int tileWidth = (rowWidth - GAP) / 2;

  renderTile(SIDE, appsTop, tileWidth, TILE_HEIGHT, IncomeIcon, tr(STR_INCOME), highlight == 0);
  renderTile(SIDE + tileWidth + GAP, appsTop, tileWidth, TILE_HEIGHT, TrainsIcon, tr(STR_TRAINS), highlight == 1);
  // Play shares the lower row with Settings so the carousel keeps its height.
  renderTile(SIDE, settingsTop, tileWidth, SETTINGS_HEIGHT, PlayIcon, tr(STR_PLAY), highlight == 2);
  renderTile(SIDE + tileWidth + GAP, settingsTop, tileWidth, SETTINGS_HEIGHT, Settings2Icon, tr(STR_SETTINGS_TITLE),
             highlight == 3);
  return appsTop;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  renderAppRows(screenHeight - metrics.buttonHintsHeight - 16);
  const int carouselHeight = centerCoverHeight() + 28;

  if (transitionDir != 0) {
    renderTransition(TOP_PAD);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  renderCarousel(TOP_PAD, carouselHeight);
  if (onLibraryCard()) {
    renderLibraryInfo(TOP_PAD + carouselHeight + 12);
  } else {
    renderBookInfo(TOP_PAD + carouselHeight + 12);
  }

  const bool onBook = highlight == NO_HIGHLIGHT;
  const char* back = onBook ? (recentBooks.empty() ? "" : tr(STR_RESUME)) : tr(STR_BACK);
  const char* confirm = onBook && onLibraryCard() ? tr(STR_LIBRARY) : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(back, confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
