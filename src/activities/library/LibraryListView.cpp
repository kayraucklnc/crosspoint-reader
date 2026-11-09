// Drawing half of LibraryListActivity: layout, header, sort tabs, rows and the
// footer, plus the row-only repaint used when the selection moves on a page.
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "LibraryListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/headerIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "fontIds.h"

using library_ui::Sort;

namespace fui = freeink::ui;

namespace {
constexpr int SIDE_PADDING = 12;
constexpr int16_t ICON_SIZE = 32;
constexpr int16_t TEXT_GAP = 10;
constexpr int16_t MIN_ROW_GAP = 6;
constexpr int16_t LINE_GAP = 2;
constexpr int16_t BAR_HEIGHT = 5;
constexpr int16_t BAR_GAP = 3;
constexpr int16_t CHECK_SIZE = 18;
constexpr int16_t SCROLL_RESERVE = 6;  // scroll indicator plus air
constexpr int16_t TOUCH_TAB_HEIGHT = 50;
constexpr int16_t TAB_PILL_PAD = 6;
constexpr int16_t INDICATOR_W = 8;
constexpr int16_t INDICATOR_GAP = 4;

const char* sortLabel(const int sort) {
  switch (static_cast<Sort>(sort)) {
    case Sort::Title:
      return tr(STR_LIBRARY_TAB_TITLE);
    case Sort::Author:
      return tr(STR_LIBRARY_TAB_AUTHOR);
    case Sort::Progress:
      return tr(STR_LIB_SORT_PROGRESS);
    case Sort::Time:
      return tr(STR_LIB_TAB_TIME);
    case Sort::Length:
      return tr(STR_LIB_TAB_LENGTH);
    case Sort::Recent:
    default:
      return tr(STR_LIBRARY_TAB_RECENT);
  }
}

fui::Paint paperOf(const fui::BoxStyle& style) {
  return style.background.kind == fui::PaintKind::None ? fui::Paint::solid(fui::Color::White) : style.background;
}
}  // namespace

LibraryListActivity::Layout LibraryListActivity::computeLayout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = app.theme();
  const bool touch = mappedInput.hasTouch();
  const fui::Rect safe = uiTarget.deviceContext().safeRect();
  Layout l{};
  l.headerTop = static_cast<int16_t>(metrics.topPadding);
  l.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  l.tabTop = static_cast<int16_t>(l.headerTop + l.headerHeight);
  const int16_t tabLine = uiTarget.lineHeight(metrics.tabPillFullSlot ? theme.bodyText.font : theme.smallText.font);
  const int16_t tabPreferred = touch ? TOUCH_TAB_HEIGHT : static_cast<int16_t>(metrics.tabBarHeight);
  l.tabHeight = degraded ? 0 : std::max<int16_t>(tabPreferred, static_cast<int16_t>(tabLine + 10));
  l.listTop = static_cast<int16_t>(l.tabTop + l.tabHeight + metrics.verticalSpacing);
  // The position readout owns the line above the hints.
  const int readoutReserved = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  l.listBottom = static_cast<int16_t>(renderer.getScreenHeight() - metrics.buttonHintsHeight - readoutReserved);
  const int16_t inset = std::max<int16_t>(theme.listInset, 0);
  l.left = static_cast<int16_t>(safe.x + inset);
  l.width = static_cast<int16_t>(safe.width - 2 * inset - SCROLL_RESERVE);
  const int16_t padY = std::max<int16_t>(touch ? theme.listTouchRowPaddingY : theme.listRowPaddingY, 4);
  l.rowHeight = static_cast<int16_t>(uiTarget.lineHeight(theme.bodyText.font) + LINE_GAP +
                                     uiTarget.lineHeight(theme.smallText.font) + 2 * padY);
  if (touch) l.rowHeight = std::max(l.rowHeight, theme.listTouchMinRowHeight);
  l.rowGap = std::max<int16_t>(theme.listRowGap, MIN_ROW_GAP);
  l.rowsPerPage = std::max(1, (l.listBottom - l.listTop + l.rowGap) / (l.rowHeight + l.rowGap));
  return l;
}

fui::Rect LibraryListActivity::rowRect(const int slot) const {
  return fui::Rect{layout.left, static_cast<int16_t>(layout.listTop + slot * (layout.rowHeight + layout.rowGap)),
                   layout.width, layout.rowHeight};
}

void LibraryListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<LibraryListActivity*>(user)->buildScreen(screen);
}

const char* LibraryListActivity::headerTitle() const { return headerLabel.c_str(); }

void LibraryListActivity::buildHeader(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::HeaderProps header;
  header.title = headerTitle();
  header.titleText = theme.titleText;
  header.titleText.align = theme.headerTitleAlign;
  header.sidePadding = theme.headerSidePadding;
  header.minTouchSize = theme.minTouchSize;
  header.styles = theme.popup;
  if (header.styles.normal.border.kind == fui::PaintKind::None && theme.headerUnderline > 0) {
    header.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    header.styles.normal.borderWidth = theme.headerUnderline;
  }
  header.trailingStyles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
  header.borderEdges = fui::EdgeBottom;
  GUI.applyHeaderStatus(renderer, header);
  if (mappedInput.hasTouch()) {
    header.leadingIcon = fui::bitmapFromIcon(icon_header_back_32);
    header.leadingAction = ACTION_BACK;
    header.trailingAdjacentIcon = fui::bitmapFromIcon(icon_library_32);
    header.trailingAdjacentAction = ACTION_MENU;
  }
  if (!degraded) {
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
  }
  const auto frameRect = screen.frame().screen();
  // headerBand fits the band to the board's bezel and rounded corners.
  const fui::Rect band = BaseTheme::headerBand(
      renderer, fui::Rect{frameRect.x, layout.headerTop, frameRect.width, layout.headerHeight}, header);
  fui::header(screen.frame(), band, header);
}

// The sort band in the tab-bar language (active tab: light pill with an
// underline on the rule, or the theme's dark pill), but with slots sized to
// their labels so six sorts fit a portrait screen. When even that overflows
// (large UI scale, long translations) the slots fall back to equal widths and
// the labels truncate.
void LibraryListActivity::buildTabs(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const bool darkPill = metrics.tabPillFullSlot;
  fui::TextStyle text = darkPill ? theme.bodyText : theme.smallText;
  text.maxLines = 1;
  const auto frameRect = screen.frame().screen();
  const fui::Rect band{frameRect.x, layout.tabTop, frameRect.width, layout.tabHeight};
  const int16_t side = static_cast<int16_t>(metrics.contentSidePadding);
  const int inner = band.width - 2 * side;

  int16_t widths[library_ui::SORT_COUNT];
  int total = 0;
  for (int i = 0; i < library_ui::SORT_COUNT; i++) {
    widths[i] = target.measureText(text.font, sortLabel(i), text).width;
    if (i == static_cast<int>(sort)) widths[i] = static_cast<int16_t>(widths[i] + INDICATOR_GAP + INDICATOR_W);
    total += widths[i];
  }
  const bool fits = total + library_ui::SORT_COUNT * 2 * TAB_PILL_PAD <= inner;
  const int spare = fits ? (inner - total) / library_ui::SORT_COUNT : 0;

  int16_t x = static_cast<int16_t>(band.x + side);
  const int16_t slotH = static_cast<int16_t>(band.height - 1);
  for (int i = 0; i < library_ui::SORT_COUNT; i++) {
    const bool last = i == library_ui::SORT_COUNT - 1;
    int16_t slotW = static_cast<int16_t>(fits ? widths[i] + spare : inner / library_ui::SORT_COUNT);
    if (last) slotW = static_cast<int16_t>(band.x + side + inner - x);
    const int16_t labelW = static_cast<int16_t>(std::min<int>(widths[i], slotW - 2 * TAB_PILL_PAD));
    const int16_t labelX = static_cast<int16_t>(x + (slotW - labelW) / 2);
    const bool active = i == static_cast<int>(sort);
    fui::Paint ink = fui::Paint::solid(fui::Color::Black);
    if (active) {
      const fui::Rect pill{static_cast<int16_t>(labelX - TAB_PILL_PAD), static_cast<int16_t>(band.y + 4),
                           static_cast<int16_t>(labelW + 2 * TAB_PILL_PAD), static_cast<int16_t>(slotH - 4)};
      if (darkPill) {
        target.fill(pill, fui::Paint::dither(fui::Color::DarkGray), theme.listRowRadius);
        ink = fui::Paint::solid(fui::Color::White);
      } else {
        target.fill(pill, fui::Paint::dither(fui::Color::LightGray));
        target.fill(fui::Rect{pill.x, static_cast<int16_t>(pill.bottom() - 2), pill.width, 2}, ink);
      }
    }
    const int16_t textW = static_cast<int16_t>(active ? labelW - INDICATOR_GAP - INDICATOR_W : labelW);
    target.text(fui::Rect{labelX, band.y, textW, slotH}, sortLabel(i), fui::textStyleWithForeground(text, ink));
    if (active) {
      // Direction arrow: down = descending, up = ascending.
      const int16_t ax = static_cast<int16_t>(labelX + textW + INDICATOR_GAP);
      const int16_t ay = static_cast<int16_t>(band.y + slotH / 2);
      const int16_t half = INDICATOR_W / 2;
      if (sortDescending()) {
        target.triangle(fui::Point{ax, static_cast<int16_t>(ay - 2)},
                        fui::Point{static_cast<int16_t>(ax + INDICATOR_W), static_cast<int16_t>(ay - 2)},
                        fui::Point{static_cast<int16_t>(ax + half), static_cast<int16_t>(ay + half - 1)}, ink);
      } else {
        target.triangle(fui::Point{ax, static_cast<int16_t>(ay + 2)},
                        fui::Point{static_cast<int16_t>(ax + INDICATOR_W), static_cast<int16_t>(ay + 2)},
                        fui::Point{static_cast<int16_t>(ax + half), static_cast<int16_t>(ay - half + 1)}, ink);
      }
    }
    screen.frame().hit(fui::Rect{x, band.y, slotW, slotH}, ACTION_TAB, static_cast<int16_t>(i), fui::InputTouch);
    x = static_cast<int16_t>(x + slotW);
  }
  target.fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - 1), band.width, 1},
              fui::Paint::solid(fui::Color::Black));
}

void LibraryListActivity::buildScreen(UiScreen& screen) {
  // Row styles follow the theme's list look (invert, light pill, marker).
  const fui::ListProps resolved = screen.resolveListProps(fui::ListProps{});
  rowStyles = resolved.rowStyles;
  if (resolved.rowRadius > 0) fui::setStyleRadius(rowStyles, resolved.rowRadius);
  titleText = resolved.labelText;
  titleText.maxLines = 1;
  detailText = resolved.subtitleText;
  detailText.maxLines = 1;
  marker = resolved.selectionMarker;
  rowSidePadding = resolved.sidePadding;

  buildHeader(screen);
  if (!degraded) buildTabs(screen);

  auto& target = screen.target();
  const fui::Rect list{layout.left, layout.listTop, layout.width,
                       static_cast<int16_t>(layout.listBottom - layout.listTop)};
  if (loadFailed || model.count() == 0) {
    const char* message = tr(STR_LIB_EMPTY_FILTER);
    if (loadFailed || model.total() == 0) {
      message = tr(STR_LIBRARY_EMPTY);
    } else if (!query.empty()) {
      message = tr(STR_LIBRARY_NO_RESULTS);
    }
    fui::TextStyle style = screen.theme().smallText;
    style.align = fui::TextAlign::Center;
    style.maxLines = 3;
    target.text(list, message, style);
    return;
  }

  for (int slot = 0; slot < static_cast<int>(rows.size()); slot++) {
    const int position = pageStart + slot;
    drawRow(target, position, position == paintSelected);
    screen.frame().hit(rowRect(slot), ACTION_ROW, static_cast<int16_t>(position),
                       fui::InputTouch | fui::InputLongPress);
  }
  const auto& theme = screen.theme();
  const fui::Rect track{layout.left, layout.listTop, static_cast<int16_t>(layout.width + SCROLL_RESERVE), list.height};
  fui::drawListScrollIndicator(target, track, static_cast<uint32_t>(model.count()),
                               static_cast<uint32_t>(layout.rowsPerPage), static_cast<uint32_t>(pageStart),
                               theme.listScrollWidth, theme.listScrollSide);
}

// Title and detail on the left; on the right the reading state: a percentage
// over a thin bar while reading, a check mark once finished, nothing before the
// book was opened.
void LibraryListActivity::drawRow(fui::DrawTarget& target, const int position, const bool isSelected) const {
  const int slot = position - pageStart;
  if (slot < 0 || slot >= static_cast<int>(rows.size())) return;
  const Row& row = rows[static_cast<size_t>(slot)];
  const library_ui::Book& book = model.book(row.book);
  const fui::Rect rect = rowRect(slot);
  const fui::BoxStyle& style = rowStyles.resolve(isSelected ? fui::StateSelected : fui::StateNormal);

  // A row repaint must first erase whatever selection fill it had.
  target.fill(rect, fui::Paint::solid(fui::Color::White));
  target.fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != fui::PaintKind::None && style.borderWidth > 0) {
    target.stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  const fui::Paint ink = style.foreground;
  const fui::Rect content = rect.inset(fui::Insets{0, rowSidePadding, 0, rowSidePadding});

  const fui::BitmapRef icon = listIconFor(UITheme::getFileIcon(row.fileName), ICON_SIZE);
  if (icon) {
    target.bitmap(
        fui::Rect{content.x, static_cast<int16_t>(content.y + (content.height - ICON_SIZE) / 2), ICON_SIZE, ICON_SIZE},
        icon, fui::BitmapMode::Contain, ink);
  }
  const int16_t textX = static_cast<int16_t>(content.x + ICON_SIZE + TEXT_GAP);
  const int16_t titleH = target.lineHeight(titleText.font);
  const int16_t detailH = target.lineHeight(detailText.font);
  const int16_t top = static_cast<int16_t>(content.y + (content.height - titleH - LINE_GAP - detailH) / 2);
  const int16_t detailTop = static_cast<int16_t>(top + titleH + LINE_GAP);

  // The state column shares the title line only (the percentage in the detail
  // font leaves room for the bar under it), so the detail line keeps the width.
  int16_t titleRight = content.right();
  if (book.finished() || book.progress >= 0) {
    const int16_t columnW = target.measureText(detailText.font, "100%", detailText).width;
    const int16_t columnX = static_cast<int16_t>(content.right() - columnW);
    // Percentage raised to the title's cap line, bar at the foot of the title line.
    const int16_t percentTop = static_cast<int16_t>(top - BAR_GAP);
    const int16_t barY = static_cast<int16_t>(std::max<int>(top + titleH - BAR_HEIGHT, percentTop + detailH + BAR_GAP));
    titleRight = static_cast<int16_t>(columnX - TEXT_GAP);
    drawProgress(target, fui::Rect{columnX, percentTop, columnW, detailH},
                 fui::Rect{columnX, barY, columnW, BAR_HEIGHT}, book, ink, paperOf(style));
  }
  target.text(fui::Rect{textX, top, static_cast<int16_t>(std::max(0, titleRight - textX)), titleH}, row.title.c_str(),
              fui::textStyleWithForeground(titleText, ink));
  if (!row.detail.empty()) {
    target.text(fui::Rect{textX, detailTop, static_cast<int16_t>(std::max(0, content.right() - textX)), detailH},
                row.detail.c_str(), fui::textStyleWithForeground(detailText, ink));
  }

  if (isSelected && marker == fui::SelectionMarker::Underline) {
    target.fill(fui::Rect{content.x, static_cast<int16_t>(rect.bottom() - 2), content.width, 2}, ink);
  } else if (isSelected && marker == fui::SelectionMarker::Triangle) {
    const int16_t midY = static_cast<int16_t>(rect.y + rect.height / 2);
    const int16_t x = static_cast<int16_t>(rect.x + 1);
    target.triangle(fui::Point{x, static_cast<int16_t>(midY - 6)}, fui::Point{x, static_cast<int16_t>(midY + 6)},
                    fui::Point{static_cast<int16_t>(x + 6), midY}, ink);
  }
}

void LibraryListActivity::drawProgress(fui::DrawTarget& target, const fui::Rect column, const fui::Rect bar,
                                       const library_ui::Book& book, const fui::Paint ink,
                                       const fui::Paint paper) const {
  if (book.finished()) {
    // A drawn check: the UI fonts carry no U+2713.
    const int16_t x = static_cast<int16_t>(column.right() - CHECK_SIZE);
    const int16_t y = static_cast<int16_t>(column.y + (column.height + BAR_GAP * 2 + BAR_HEIGHT - CHECK_SIZE) / 2);
    const fui::Point a{x, static_cast<int16_t>(y + CHECK_SIZE / 2)};
    const fui::Point b{static_cast<int16_t>(x + CHECK_SIZE * 3 / 8), static_cast<int16_t>(y + CHECK_SIZE - 2)};
    const fui::Point c{static_cast<int16_t>(x + CHECK_SIZE), static_cast<int16_t>(y + 1)};
    target.line(a, b, 3, ink);
    target.line(b, c, 3, ink);
    return;
  }
  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", std::max(0, static_cast<int>(book.progress)));
  fui::TextStyle style = fui::textStyleWithForeground(detailText, ink);
  style.align = fui::TextAlign::Right;
  target.text(column, percent, style);
  target.fill(bar, paper);
  target.stroke(bar, ink, 1);
  const int16_t filled = static_cast<int16_t>((bar.width - 2) * std::clamp<int>(book.progress, 0, 100) / 100);
  if (filled > 0) {
    target.fill(fui::Rect{static_cast<int16_t>(bar.x + 1), static_cast<int16_t>(bar.y + 1), filled,
                          static_cast<int16_t>(bar.height - 2)},
                ink);
  }
}

// "12/69 books" at the bottom right: the selected book's position, which stays
// stable while paging, unlike a page count. Its slot is sized for the widest
// value ("69/69 books") so a row-only repaint can clear it exactly.
int LibraryListActivity::readoutWidth() const {
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), model.count(), model.count());
  return renderer.getTextWidth(SMALL_FONT_ID, buf);
}

void LibraryListActivity::drawReadout() const {
  const int count = model.count();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
  const int slotW = readoutWidth();
  const int right = renderer.getScreenWidth() - SIDE_PADDING;
  renderer.fillRect(right - slotW, y, slotW, lineHeight, false);
  if (count <= 0) return;
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_LIBRARY_POSITION), paintSelected + 1, count);
  renderer.drawText(SMALL_FONT_ID, right - renderer.getTextWidth(SMALL_FONT_ID, buf), y, buf, true);
}

void LibraryListActivity::drawFooter() const {
  drawReadout();
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (!mappedInput.hasTouch() && !loadFailed) {
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
    const int width = renderer.getScreenWidth() - 3 * SIDE_PADDING - readoutWidth();
    const std::string help =
        renderer.truncatedText(SMALL_FONT_ID, degraded ? tr(STR_LIB_HOLD_HELP_UNSORTED) : tr(STR_LIB_HOLD_HELP), width,
                               EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, SIDE_PADDING, y, help.c_str(), true);
  }
  // Left/Right step the sort on the physical front buttons; mapLabels swaps its
  // previous/next pair on flipped layouts, so hand it the pair pre-swapped.
  const bool swapped = mappedInput.isNavDirectionSwapped();
  const char* previous = degraded ? "" : tr(STR_LIB_SORT_PREV);
  const char* next = degraded ? "" : tr(STR_LIB_SORT_NEXT);
  const auto labels =
      mappedInput.mapLabels(query.empty() ? tr(STR_HOME) : tr(STR_BACK), model.count() > 0 ? tr(STR_OPEN) : "",
                            swapped ? next : previous, swapped ? previous : next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

bool LibraryListActivity::canRepaintRows() const {
  return drawnGen == frameGen.load() && drawnSelected >= pageStart &&
         drawnSelected < pageStart + static_cast<int>(rows.size());
}

void LibraryListActivity::render(RenderLock&&) {
  if (popup.processRender(renderer, mappedInput)) {
    // The popup painted over the list; whatever closes it repaints in full.
    drawnGen = 0;
    return;
  }
  paintSelected = selected;
  if (canRepaintRows() && paintSelected >= pageStart && paintSelected < pageStart + static_cast<int>(rows.size())) {
    if (paintSelected == drawnSelected) return;
    drawRow(uiTarget, drawnSelected, false);
    drawRow(uiTarget, paintSelected, true);
    drawReadout();
    renderer.displayBuffer();
    LOG_DBG("LIB", "rows repainted: %d -> %d", drawnSelected, paintSelected);
    drawnSelected = paintSelected;
    return;
  }
  const uint32_t gen = frameGen.load();
  renderer.clearScreen();
  renderUi();
  drawFooter();
  renderer.displayBuffer();
  LOG_DBG("LIB", "full frame: page %d, selected %d", pageStart, paintSelected);
  drawnGen = gen;
  drawnSelected = paintSelected;
}
