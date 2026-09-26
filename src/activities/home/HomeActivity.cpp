#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OrderedDither.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/CoverTilt.h"
#include "components/icons/income.h"
#include "components/icons/settings2.h"
#include "components/icons/trains.h"
#include "fontIds.h"
#include "util/BookProgress.h"

namespace {
constexpr int TOP_PAD = 36;
constexpr int GAP = 22;  // air between the centre cover and the turned neighbours
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
void lightenShadow(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const int strength) {
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
  recentBooks.reserve(books.size());
  for (const RecentBook& book : books) {
    if (!RecentBooksStore::isMissing(book)) recentBooks.push_back(book);
  }
  progressCache.assign(recentBooks.size(), -2);

  highlight = recentBooks.empty() ? 0 : NO_HIGHLIGHT;
  if (initialMenuItem == HomeMenuItem::INCOME) highlight = 0;
  if (initialMenuItem == HomeMenuItem::TRAINS) highlight = 1;
  if (initialMenuItem == HomeMenuItem::SETTINGS_MENU) highlight = 2;
  coversPending = !recentBooks.empty();
  requestUpdate();
}

std::string HomeActivity::thumbPath(const RecentBook& book, const int height) const {
  return book.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(book.coverBmpPath, height);
}

bool HomeActivity::ensureThumb(RecentBook& book, const int height) {
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasEpubExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return false;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    cover_tilt::invalidateIfDitherChanged(epub->getThumbBmpPath(height));
    return Storage.exists(epub->getThumbBmpPath(height).c_str()) || epub->generateThumbBmpFromSource(height);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return false;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    cover_tilt::invalidateIfDitherChanged(xtc->getThumbBmpPath(height));
    return Storage.exists(xtc->getThumbBmpPath(height).c_str()) || (xtc->load() && xtc->generateThumbBmp(height));
  }
  return false;
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
      activityManager.goToSettings();
      return;
    default:
      if (!recentBooks.empty()) activityManager.goToReader(recentBooks[bookIndex].path);
  }
}

void HomeActivity::flipBook(const int direction) {
  const int count = static_cast<int>(recentBooks.size());
  const int next = std::clamp(bookIndex + direction, 0, std::max(0, count - 1));
  if (next == bookIndex) return;
  bookIndex = next;
  if (SETTINGS.homeAnimation && firstRenderDone) {
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
    if (highlight != NO_HIGHLIGHT && !recentBooks.empty()) {
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
    renderer.drawText(UI_10_FONT_ID, centerX - titleWidth / 2, top + height / 2 - 10, title, true,
                      EpdFontFamily::BOLD);
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
  }
  renderer.setClipRect(savedClip[0], savedClip[1], savedClip[2], savedClip[3]);

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

  // Position dots.
  if (count > 1) {
    constexpr int DOT = 6;
    constexpr int DOT_GAP = 8;
    const int dotsWidth = count * DOT + (count - 1) * DOT_GAP;
    const int dotsY = top + height - DOT;
    for (int i = 0; i < count; i++) {
      const int dx = centerX - dotsWidth / 2 + i * (DOT + DOT_GAP);
      if (i == bookIndex) {
        renderer.fillRect(dx, dotsY, DOT, DOT, true);
      } else {
        renderer.drawRect(dx, dotsY, DOT, DOT, 1, true);
      }
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

void HomeActivity::renderEmptyState(const int top, const int height) const {
  const int screenWidth = renderer.getScreenWidth();
  const int cardHeight = height * 2 / 3;
  const int cardWidth = cardHeight * 2 / 3;
  const int cardX = (screenWidth - cardWidth) / 2;
  // Dashed outline of an empty book slot.
  for (int d = 0; d < cardWidth; d += 12) {
    renderer.fillRect(cardX + d, top, 6, 2, true);
    renderer.fillRect(cardX + d, top + cardHeight - 2, 6, 2, true);
  }
  for (int d = 0; d < cardHeight; d += 12) {
    renderer.fillRect(cardX, top + d, 2, 6, true);
    renderer.fillRect(cardX + cardWidth - 2, top + d, 2, 6, true);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, top + cardHeight + 24, tr(STR_HOME_NO_BOOKS), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + cardHeight + 56, tr(STR_HOME_NO_BOOKS_HINT));
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
  renderTile(SIDE, settingsTop, rowWidth, SETTINGS_HEIGHT, Settings2Icon, tr(STR_SETTINGS_TITLE), highlight == 2);
  return appsTop;
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenHeight = renderer.getScreenHeight();
  renderer.clearScreen();

  const int appsTop = renderAppRows(screenHeight - metrics.buttonHintsHeight - 16);
  const int carouselHeight = centerCoverHeight() + 28;

  if (transitionDir != 0) {
    renderTransition(TOP_PAD);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (recentBooks.empty()) {
    renderEmptyState(TOP_PAD, appsTop - TOP_PAD - 40);
  } else {
    renderCarousel(TOP_PAD, carouselHeight);
    renderBookInfo(TOP_PAD + carouselHeight + 12);
  }

  const bool onBook = highlight == NO_HIGHLIGHT;
  const char* back = onBook ? (recentBooks.empty() ? "" : tr(STR_RESUME)) : tr(STR_BACK);
  const auto labels = mappedInput.mapLabels(back, tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                 : HalDisplay::FAST_REFRESH);
  firstRenderDone = true;
}
