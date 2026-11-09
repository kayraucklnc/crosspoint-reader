#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "GetBooksActivity.h"
#include "GetBooksCatalog.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 24;
constexpr int TITLE_Y = 12;
constexpr int LIST_TOP = 54;
constexpr int CARD_HEIGHT = 78;
constexpr int CARD_GAP = 10;
constexpr int ROW_HEIGHT = 64;
constexpr int FOCUS_FRAME = 3;
constexpr int BAR_HEIGHT = 14;

int contentBottom(const GfxRenderer& renderer) {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 8;
}

// Draws up to `maxLines` wrapped lines from `y`; returns the y below them.
int drawWrapped(const GfxRenderer& renderer, const int fontId, const char* text, const int x, int y, const int width,
                const int maxLines, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (maxLines <= 0 || !text || !text[0]) return y;
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : renderer.wrappedText(fontId, text, width, maxLines, style)) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
  return y;
}

void drawChip(const GfxRenderer& renderer, const int x, const int y, const char* label, const bool filled) {
  const int w = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) + 20;
  const int h = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  if (filled) {
    renderer.fillRoundedRect(x, y, w, h, 8, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 1, 8, true);
  }
  renderer.drawText(UI_10_FONT_ID, x + 10, y + 4, label, !filled, EpdFontFamily::BOLD);
}

int chipWidth(const GfxRenderer& renderer, const char* label) {
  return renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) + 20;
}

// Magnifier for search sources, an open book for browsable lists.
void drawSourceGlyph(const GfxRenderer& renderer, const int x, const int y, const bool search) {
  if (search) {
    renderer.drawRoundedRect(x + 2, y + 2, 24, 24, 3, 12, true);
    renderer.drawLine(x + 22, y + 22, x + 32, y + 32, 4, true);
    return;
  }
  renderer.drawRoundedRect(x, y + 4, 16, 26, 2, 3, true);
  renderer.drawRoundedRect(x + 18, y + 4, 16, 26, 2, 3, true);
  renderer.fillRect(x + 15, y + 6, 4, 26, true);
}

// A check mark in a ring, for the "saved" screen.
void drawCheckBadge(const GfxRenderer& renderer, const int cx, const int cy, const int r) {
  renderer.fillRoundedRect(cx - r, cy - r, 2 * r, 2 * r, r, Color::Black);
  renderer.drawLine(cx - r / 2, cy, cx - r / 8, cy + r / 3, 5, false);
  renderer.drawLine(cx - r / 8, cy + r / 3, cx + r / 2, cy - r / 3, 5, false);
}
}  // namespace

int GetBooksActivity::rowsPerPage() const { return std::max(1, (contentBottom(renderer) - LIST_TOP) / ROW_HEIGHT); }

const char* GetBooksActivity::statusText() const {
  if (WiFi.status() != WL_CONNECTED) return tr(STR_CONNECTING);
  if (job == Job::List && !query.empty()) return tr(STR_GB_SEARCHING);
  return tr(STR_LOADING);
}

void GetBooksActivity::drawTitle(const char* title, const char* info) const {
  const int w = renderer.getScreenWidth();
  int infoWidth = 0;
  if (info && info[0]) {
    infoWidth = renderer.getTextWidth(UI_10_FONT_ID, info);
    renderer.drawText(UI_10_FONT_ID, w - SIDE - infoWidth, TITLE_Y + 4, info);
  }
  const std::string fitted =
      renderer.truncatedText(UI_12_FONT_ID, title, w - 2 * SIDE - infoWidth - 12, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, SIDE, TITLE_Y, fitted.c_str(), true, EpdFontFamily::BOLD);
  renderer.fillRect(SIDE, LIST_TOP - 10, w - 2 * SIDE, 1, true);
}

void GetBooksActivity::render(RenderLock&&) {
  if (job != Job::None) {
    // Overlay on the frame already on the panel: one quick refresh.
    GUI.drawPopup(renderer, statusText());
    return;
  }
  renderer.clearScreen();
  switch (screen) {
    case Screen::Sources:
      drawSources();
      break;
    case Screen::Results:
      drawResults();
      break;
    case Screen::Detail:
      drawDetail();
      break;
    case Screen::Download:
      drawDownload();
      break;
    case Screen::Done:
      drawDone();
      break;
    case Screen::Error:
      drawError();
      break;
  }
  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void GetBooksActivity::drawSources() const {
  drawTitle(tr(STR_GET_BOOKS), tr(STR_GB_TAGLINE));
  const int w = renderer.getScreenWidth();
  const int count = sourceCount();
  const int perPage = std::max(1, (contentBottom(renderer) - LIST_TOP + CARD_GAP) / (CARD_HEIGHT + CARD_GAP));
  const int first = (sourceFocus / perPage) * perPage;
  const int last = std::min(count, first + perPage);
  for (int i = first; i < last; i++) {
    const int y = LIST_TOP + (i - first) * (CARD_HEIGHT + CARD_GAP);
    const bool focused = i == sourceFocus;
    renderer.drawRoundedRect(SIDE, y, w - 2 * SIDE, CARD_HEIGHT, focused ? FOCUS_FRAME : 1, 10, true);
    const char* title;
    const char* description;
    const bool search = i < getbooks::SOURCE_COUNT && getbooks::isSearch(getbooks::SOURCES[i]);
    drawSourceGlyph(renderer, SIDE + 18, y + (CARD_HEIGHT - 34) / 2, search);
    if (i < getbooks::SOURCE_COUNT) {
      title = I18N.get(getbooks::SOURCES[i].title);
      description = I18N.get(getbooks::SOURCES[i].description);
    } else {
      const OpdsServer* server = OPDS_STORE.getServer(i - getbooks::SOURCE_COUNT);
      title = server ? server->name.c_str() : "";
      description = tr(STR_GB_CATALOG_DESC);
    }
    const int textX = SIDE + 18 + 34 + 18;
    const int textWidth = w - SIDE - 18 - textX;
    renderer.drawText(UI_12_FONT_ID, textX, y + 12,
                      renderer.truncatedText(UI_12_FONT_ID, title, textWidth, EpdFontFamily::BOLD).c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, textX, y + 44,
                      renderer.truncatedText(UI_10_FONT_ID, description, textWidth).c_str());
  }
}

void GetBooksActivity::drawResults() const {
  char info[32];
  snprintf(info, sizeof(info), nextUrl.empty() ? tr(STR_GB_COUNT) : tr(STR_GB_COUNT_MORE),
           static_cast<int>(results.size()));
  char title[80];
  if (query.empty()) {
    snprintf(title, sizeof(title), "%s", I18N.get(getbooks::SOURCES[sourceIndex].title));
  } else {
    snprintf(title, sizeof(title), "“%s”", query.c_str());
  }
  drawTitle(title, info);

  const int w = renderer.getScreenWidth();
  const int perPage = rowsPerPage();
  const int first = (focus / perPage) * perPage;
  const int last = std::min(static_cast<int>(results.size()), first + perPage);
  const int x0 = SIDE;
  const int x1 = w - SIDE;
  for (int i = first; i < last; i++) {
    const legacy_opds::OpdsEntry& entry = results[i];
    const int y = LIST_TOP + (i - first) * ROW_HEIGHT;
    char size[16];
    getbooks::formatSize(entry.size, size, sizeof(size));
    const int sizeWidth = size[0] ? renderer.getTextWidth(UI_10_FONT_ID, size) + 12 : 0;
    const int textWidth = x1 - x0 - 20 - sizeWidth;
    renderer.drawText(
        UI_12_FONT_ID, x0 + 10, y + 6,
        renderer.truncatedText(UI_12_FONT_ID, entry.title.c_str(), x1 - x0 - 20, EpdFontFamily::BOLD).c_str(), true,
        EpdFontFamily::BOLD);
    const std::string& byline = entry.author.empty() ? entry.summary : entry.author;
    const std::string author = getbooks::displayAuthor(byline);
    renderer.drawText(UI_10_FONT_ID, x0 + 10, y + 36,
                      renderer.truncatedText(UI_10_FONT_ID, author.c_str(), textWidth).c_str());
    if (size[0]) renderer.drawText(UI_10_FONT_ID, x1 - 10 - sizeWidth + 12, y + 36, size);
    if (i == focus) {
      renderer.drawRoundedRect(x0, y + 1, x1 - x0, ROW_HEIGHT - 4, 2, 8, true);
    } else {
      for (int px = x0 + 10; px < x1 - 10; px += 3) renderer.drawPixel(px, y + ROW_HEIGHT - 2, true);
    }
  }
  if (!nextUrl.empty() && last == static_cast<int>(results.size())) {
    const int y = LIST_TOP + (last - first) * ROW_HEIGHT + 8;
    if (y + renderer.getLineHeight(UI_10_FONT_ID) < contentBottom(renderer)) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_GB_MORE_HINT));
    }
  }
}

void GetBooksActivity::drawDetail() const {
  drawTitle(tr(STR_GB_DETAILS), originName());
  const int w = renderer.getScreenWidth();
  const int top = LIST_TOP + 4;
  int coverBottom = top;
  int x = SIDE;
  if (hasCover) {
    HalFile file;
    if (Storage.openFileForRead("GETBOOKS", getbooks::COVER_BMP_PATH, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const int cw = std::min(bitmap.getWidth(), getbooks::COVER_WIDTH);
        const int ch = std::min(bitmap.getHeight(), getbooks::COVER_HEIGHT);
        renderer.drawBitmap(bitmap, SIDE, top, cw, ch);
        renderer.drawRect(SIDE - 1, top - 1, cw + 2, ch + 2, true);
        coverBottom = top + ch;
        x = SIDE + cw + 18;
      }
    }
  }
  const int width = w - SIDE - x;
  int y = top;
  y = drawWrapped(renderer, UI_12_FONT_ID, detail.title.c_str(), x, y, width, 4, EpdFontFamily::BOLD);
  if (!detail.author.empty()) y = drawWrapped(renderer, UI_10_FONT_ID, detail.author.c_str(), x, y + 4, width, 2);
  y += 14;

  // Format and size chips; "In library" wraps below when the row is full.
  char size[16];
  getbooks::formatSize(detail.size, size, sizeof(size));
  const int chipH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  int cx = x;
  drawChip(renderer, cx, y, "EPUB", false);
  cx += chipWidth(renderer, "EPUB") + 8;
  if (size[0]) {
    drawChip(renderer, cx, y, size, false);
    cx += chipWidth(renderer, size) + 8;
  }
  if (detailOnCard) {
    if (cx + chipWidth(renderer, tr(STR_GB_IN_LIBRARY)) > w - SIDE) {
      cx = x;
      y += chipH + 8;
    }
    drawChip(renderer, cx, y, tr(STR_GB_IN_LIBRARY), true);
  }
  y += chipH;

  y = std::max(y, coverBottom) + 16;
  renderer.fillRect(SIDE, y, w - 2 * SIDE, 1, true);
  y += 14;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int lines = (contentBottom(renderer) - y) / lineHeight;
  const char* about = detail.summary.empty() ? tr(STR_GB_NO_DESCRIPTION) : detail.summary.c_str();
  drawWrapped(renderer, UI_10_FONT_ID, about, SIDE, y, w - 2 * SIDE, lines);
}

void GetBooksActivity::drawDownload() const {
  drawTitle(tr(STR_DOWNLOADING), originName());
  const int w = renderer.getScreenWidth();
  const int width = w - 2 * SIDE;
  int y = renderer.getScreenHeight() / 3;
  y = drawWrapped(renderer, UI_12_FONT_ID, detail.title.c_str(), SIDE, y, width, 2, EpdFontFamily::BOLD);
  y = drawWrapped(renderer, UI_10_FONT_ID, detail.author.c_str(), SIDE, y + 4, width, 1);
  y += 24;

  renderer.drawRoundedRect(SIDE, y, width, BAR_HEIGHT, 2, BAR_HEIGHT / 2, true);
  if (total > 0) {
    const int fill = static_cast<int>(static_cast<uint64_t>(std::min(downloaded, total)) * (width - 6) / total);
    if (fill > 0) renderer.fillRoundedRect(SIDE + 3, y + 3, fill, BAR_HEIGHT - 6, (BAR_HEIGHT - 6) / 2, Color::Black);
  }
  y += BAR_HEIGHT + 12;

  char done[16];
  char all[16];
  getbooks::formatSize(static_cast<uint32_t>(downloaded), done, sizeof(done));
  getbooks::formatSize(static_cast<uint32_t>(total), all, sizeof(all));
  char line[48];
  if (total > 0) {
    snprintf(line, sizeof(line), "%s / %s", done[0] ? done : "0 KB", all);
  } else {
    snprintf(line, sizeof(line), "%s", done[0] ? done : tr(STR_GB_STARTING));
  }
  renderer.drawText(UI_10_FONT_ID, SIDE, y, line);
  if (total > 0) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%",
             static_cast<int>(static_cast<uint64_t>(std::min(downloaded, total)) * 100 / total));
    renderer.drawText(UI_10_FONT_ID, w - SIDE - renderer.getTextWidth(UI_10_FONT_ID, percent, EpdFontFamily::BOLD), y,
                      percent, true, EpdFontFamily::BOLD);
  }
}

void GetBooksActivity::drawDone() const {
  drawTitle(tr(STR_GET_BOOKS), originName());
  const int w = renderer.getScreenWidth();
  const int width = w - 2 * SIDE;
  int y = renderer.getScreenHeight() / 4;
  drawCheckBadge(renderer, w / 2, y, 34);
  y += 58;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_GB_SAVED), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 18;
  for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, detail.title.c_str(), width, 2, EpdFontFamily::BOLD)) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  if (!detail.author.empty()) renderer.drawCenteredText(UI_10_FONT_ID, y + 4, detail.author.c_str());
  y += renderer.getLineHeight(UI_10_FONT_ID) + 24;
  const std::string path = renderer.truncatedText(SMALL_FONT_ID, savedPath.c_str(), width);
  renderer.drawCenteredText(SMALL_FONT_ID, y, path.c_str());
}

void GetBooksActivity::drawError() const {
  drawTitle(tr(STR_GET_BOOKS), "");
  const int w = renderer.getScreenWidth();
  const int mid = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, mid - 40, I18N.get(errorText), true, EpdFontFamily::BOLD);
  const int width = w - 2 * SIDE;
  int y = mid;
  for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_GB_ERROR_HINT), width, 3)) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
}

void GetBooksActivity::drawHints() const {
  const char* back = tr(STR_BACK);
  const char* confirm = "";
  const char* prev = "";
  const char* next = "";
  const char* up = "";
  const char* down = "";
  switch (screen) {
    case Screen::Sources:
      confirm = tr(STR_OPEN);
      prev = tr(STR_DIR_UP);
      next = tr(STR_DIR_DOWN);
      break;
    case Screen::Results: {
      confirm = tr(STR_GB_DETAILS);
      prev = tr(STR_GB_PREV);
      next = tr(STR_GB_NEXT);
      const int perPage = rowsPerPage();
      const int page = focus / perPage;
      const int lastPage = (static_cast<int>(results.size()) - 1) / perPage;
      if (page > 0 || !previousUrls.empty()) up = tr(STR_GB_PAGE_UP);
      if (page < lastPage) {
        down = tr(STR_GB_PAGE_DOWN);
      } else if (!nextUrl.empty()) {
        down = tr(STR_GB_MORE);
      }
      break;
    }
    case Screen::Detail:
      confirm = detailOnCard ? tr(STR_OPEN) : tr(STR_GB_GET);
      if (focus > 0) prev = tr(STR_GB_PREV);
      if (focus + 1 < static_cast<int>(results.size())) next = tr(STR_GB_NEXT);
      break;
    case Screen::Download:
      back = tr(STR_CANCEL);
      break;
    case Screen::Done:
      confirm = tr(STR_GB_OPEN_NOW);
      break;
    case Screen::Error:
      confirm = tr(STR_RETRY);
      break;
  }
  const auto labels = mappedInput.mapLabels(back, confirm, prev, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (up[0] || down[0]) GUI.drawSideButtonHints(renderer, up, down);
}
