// The details card LibraryActivity lays over any view: cover, title, reading
// history and the Open / Mark finished / Archive actions.
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "LibraryActivity.h"
#include "LibraryArt.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookThumbs.h"

namespace art = library_art;

namespace {
constexpr int PAD = 18;
constexpr int ACTION_HEIGHT = 48;
constexpr int STAT_ROW = 34;
// Clears most pixels of the view behind the card so it reads as backdrop.
constexpr int BACKDROP_DIM = 13;

void statRow(const GfxRenderer& r, const int x, const int y, const int w, const char* label, const char* value) {
  r.drawText(UI_10_FONT_ID, x, y, label);
  const int vw = r.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
  r.drawText(UI_10_FONT_ID, x + w - vw, y, value, true, EpdFontFamily::BOLD);
  for (int px = x; px < x + w; px += 3) r.drawPixel(px, y + STAT_ROW - 8, true);
}
}  // namespace

void LibraryActivity::drawDetailCard() {
  const Item* item = itemAt(focus);
  if (!item) return;
  const library_ui::Book& book = model.book(item->book);
  const int screenW = renderer.getScreenWidth();
  art::dim(renderer, 0, 0, screenW, renderer.getScreenHeight(), BACKDROP_DIM);

  // Sized to its content and centred, so the view stays visible around it.
  const int x = gutter();
  const int w = screenW - 2 * gutter();
  const int h = PAD + COVER_HEIGHT + 22 + 4 * STAT_ROW + 12 + ACTION_HEIGHT + PAD;
  const int y = (contentTop() + contentBottom() - h) / 2;
  renderer.fillRectDither(x + 5, y + 5, w, h, Color::DarkGray);
  renderer.fillRect(x, y, w, h, false);
  renderer.drawRect(x, y, w, h, 2, true);

  const int coverW = book_thumbs::generatedWidth(COVER_HEIGHT);
  drawThumb(*item, COVER_HEIGHT, x + PAD, y + PAD, coverW);

  // Title block beside the cover.
  const int tx = x + PAD + coverW + PAD;
  const int textW = x + w - PAD - tx;
  int ty = y + PAD;
  const auto lines = renderer.wrappedText(NOTOSERIF_16_FONT_ID, item->title.c_str(), textW, 3, EpdFontFamily::BOLD);
  for (const auto& line : lines) {
    renderer.drawText(NOTOSERIF_16_FONT_ID, tx, ty, line.c_str(), true, EpdFontFamily::BOLD);
    ty += renderer.getLineHeight(NOTOSERIF_16_FONT_ID);
  }
  if (!item->author.empty()) {
    const auto authorLines = renderer.wrappedText(UI_10_FONT_ID, item->author.c_str(), textW, 2);
    ty += 4;
    for (const auto& line : authorLines) {
      renderer.drawText(UI_10_FONT_ID, tx, ty, line.c_str());
      ty += renderer.getLineHeight(UI_10_FONT_ID);
    }
  }
  const char* status = nullptr;
  if (book.archived()) {
    status = tr(STR_LIB_FILTER_ARCHIVED);
  } else if (book.finished()) {
    status = tr(STR_LIB_FILTER_FINISHED);
  } else if (book.reading()) {
    status = tr(STR_LIB_FILTER_READING);
  }
  if (status) {
    const int sw = renderer.getTextWidth(SMALL_FONT_ID, status, EpdFontFamily::BOLD) + 16;
    constexpr int PILL = 24;
    const int sy = std::min(ty + 8, y + PAD + COVER_HEIGHT - PILL);
    renderer.fillRoundedRect(tx, sy, sw, PILL, PILL / 2, Color::Black);
    renderer.drawText(SMALL_FONT_ID, tx + 8, sy + (PILL - renderer.getLineHeight(SMALL_FONT_ID)) / 2, status, false,
                      EpdFontFamily::BOLD);
  }

  // Reading history.
  const int sx = x + PAD;
  const int sw = w - 2 * PAD;
  int sy = y + PAD + COVER_HEIGHT + 26;
  char value[40];
  renderer.drawText(UI_10_FONT_ID, sx, sy, tr(STR_LIB_PROGRESS));
  if (book.progress >= 0 || book.finished()) {
    const int pct = book.finished() && book.progress < 100 ? 100 : book.progress;
    snprintf(value, sizeof(value), "%d%%", pct);
    const int vw = renderer.getTextWidth(UI_10_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, sx + sw - vw, sy, value, true, EpdFontFamily::BOLD);
    art::drawProgressBar(renderer, sx + 110, sy + 7, sw - 110 - vw - 16, 8, pct);
  } else {
    const char* none = (book.flags & library_ui::Book::OPENED) ? "-" : tr(STR_LIB_NOT_STARTED);
    const int vw = renderer.getTextWidth(UI_10_FONT_ID, none, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, sx + sw - vw, sy, none, true, EpdFontFamily::BOLD);
  }
  for (int px = sx; px < sx + sw; px += 3) renderer.drawPixel(px, sy + STAT_ROW - 8, true);
  sy += STAT_ROW;
  art::formatDuration(value, sizeof(value), book.readSeconds);
  statRow(renderer, sx, sy, sw, tr(STR_LIB_TIME_READ), value[0] ? value : "-");
  sy += STAT_ROW;
  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(book.sessions));
  statRow(renderer, sx, sy, sw, tr(STR_LIB_SESSIONS), book.sessions ? value : "-");
  sy += STAT_ROW;
  art::formatLastRead(value, sizeof(value), book.lastRead);
  statRow(renderer, sx, sy, sw, tr(STR_LIB_LAST_READ), value);

  // Actions.
  // "Finished" carries a checkbox showing the mark it toggles.
  const char* labels[ACTION_COUNT] = {tr(STR_OPEN), tr(STR_LIB_FILTER_FINISHED),
                                      book.archived() ? tr(STR_LIB_UNARCHIVE) : tr(STR_LIB_ARCHIVE)};
  constexpr int GAP = 10;
  constexpr int BOX = 14;
  const int aw = (sw - GAP * (ACTION_COUNT - 1)) / ACTION_COUNT;
  const int ay = y + h - PAD - ACTION_HEIGHT;
  for (int i = 0; i < ACTION_COUNT; i++) {
    const int ax = sx + i * (aw + GAP);
    const bool selected = i == cardAction;
    if (selected) {
      renderer.fillRoundedRect(ax, ay, aw, ACTION_HEIGHT, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(ax, ay, aw, ACTION_HEIGHT, 1, 10, true);
    }
    const int box = i == ACTION_FINISHED ? BOX + 8 : 0;
    const std::string label = renderer.truncatedText(UI_10_FONT_ID, labels[i], aw - 12 - box, EpdFontFamily::BOLD);
    const int lw = renderer.getTextWidth(UI_10_FONT_ID, label.c_str(), EpdFontFamily::BOLD);
    const int lx = ax + (aw - lw - box) / 2 + box;
    if (box) {
      const int bx = lx - box;
      const int by = ay + (ACTION_HEIGHT - BOX) / 2;
      renderer.drawRect(bx, by, BOX, BOX, 2, !selected);
      if (book.flags & library_ui::Book::MARKED_FINISHED) {
        renderer.drawLine(bx + 3, by + 7, bx + 6, by + 10, 2, !selected);
        renderer.drawLine(bx + 6, by + 10, bx + 11, by + 3, 2, !selected);
      }
    }
    renderer.drawText(UI_10_FONT_ID, lx, ay + (ACTION_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      label.c_str(), !selected, EpdFontFamily::BOLD);
  }
}
