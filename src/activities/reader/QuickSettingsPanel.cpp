#include "QuickSettingsPanel.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "activities/income/IncomeGraphics.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int PAD = 12;
constexpr int CHIP_GAP = 8;
constexpr int RADIUS = 8;
constexpr int CHEVRON_ROOM = 12;  // chevron/"A" cue width at each end of a stepper
constexpr int SHADOW_H = 6;

constexpr StrId ROW_LABELS[] = {StrId::STR_SIZE,       StrId::STR_FONT,      StrId::STR_LINE_SPACING,
                                StrId::STR_QS_MARGINS, StrId::STR_ALIGNMENT, StrId::STR_HYPHENATION,
                                StrId::STR_QS_REFRESH};
constexpr StrId LINK_LABELS[] = {StrId::STR_QS_CHAPTERS, StrId::STR_BOOKMARKS,     StrId::STR_LOOKUP,
                                 StrId::STR_QS_STATS,    StrId::STR_GO_TO_PERCENT, StrId::STR_QS_ALL_OPTIONS};
constexpr StrId SPACING_LABELS[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId ALIGN_LABELS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                  StrId::STR_BOOK_S_STYLE};
constexpr StrId REFRESH_LABELS[] = {StrId::STR_PAGES_1,  StrId::STR_PAGES_5,  StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                                    StrId::STR_PAGES_20, StrId::STR_PAGES_30, StrId::STR_STATE_OFF};
static_assert(std::size(ROW_LABELS) == static_cast<size_t>(QuickSettingsPanel::Row::Count), "row labels");
static_assert(std::size(LINK_LABELS) == static_cast<size_t>(QuickSettingsPanel::Link::Count), "link labels");
static_assert(std::size(SPACING_LABELS) == CrossPointSettings::LINE_COMPRESSION_COUNT, "spacing labels");
static_assert(std::size(ALIGN_LABELS) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
static_assert(std::size(REFRESH_LABELS) == CrossPointSettings::REFRESH_FREQUENCY_COUNT, "refresh labels");

constexpr int MARGIN_STEPS = (CrossPointSettings::SCREEN_MARGIN_MAX - CrossPointSettings::SCREEN_MARGIN_MIN) /
                                 CrossPointSettings::SCREEN_MARGIN_STEP +
                             1;

int familyCount(const SdCardFontRegistry* registry) {
  return CrossPointSettings::BUILTIN_FONT_COUNT + (registry ? static_cast<int>(registry->getFamilyCount()) : 0);
}

// Index into [built-ins..., SD families...] of the active reader family.
int currentFamilyIndex(const SdCardFontRegistry* registry) {
  if (SETTINGS.sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + static_cast<int>(i);
      }
    }
  }
  return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
}

// Small solid triangle pointing left (dir < 0) or right.
void drawChevron(const GfxRenderer& renderer, const int cx, const int cy, const int dir, const bool black) {
  constexpr int H = 6;
  const int xs[3] = {cx - dir * 3, cx + dir * 3, cx - dir * 3};
  const int ys[3] = {cy - H, cy, cy + H};
  renderer.fillPolygon(xs, ys, 3, black);
}

void drawDot(const GfxRenderer& renderer, const int x, const int y, const int d, const bool filled, const bool black) {
  if (filled) {
    renderer.fillRoundedRect(x, y, d, d, d / 2, black ? Color::Black : Color::White);
  } else {
    renderer.drawRoundedRect(x, y, d, d, 1, d / 2, black);
  }
}
}  // namespace

// --- Geometry ------------------------------------------------------------------

QuickSettingsPanel::Layout QuickSettingsPanel::layout(const GfxRenderer& renderer) const {
  const Rect safe = [&renderer] {
    const auto r = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    return Rect{r.x, r.y, r.width, r.height};
  }();
  Layout l{};
  const bool landscape = safe.w > safe.h;
  l.columns = landscape ? 2 : 1;
  l.rowH = landscape ? 34 : 33;
  l.chipCols = safe.w >= 700 ? LINK_COUNT : 3;
  l.chipH = landscape ? 34 : 32;
  const int rowLines = (ROW_COUNT + l.columns - 1) / l.columns;
  const int chipLines = (LINK_COUNT + l.chipCols - 1) / l.chipCols;
  const int headerH = 8 + renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) + 6;
  const int height = headerH + rowLines * l.rowH + PAD + chipLines * (l.chipH + CHIP_GAP) + PAD;
  l.sheet = Rect{safe.x, safe.y + safe.h - height, safe.w, height};
  l.left = safe.x + PAD;
  l.headerY = l.sheet.y + 8;
  l.rowsY = l.sheet.y + headerH;
  l.colW = (safe.w - PAD * 2 - (l.columns - 1) * PAD) / l.columns;
  l.chipsY = l.rowsY + rowLines * l.rowH + PAD;
  l.chipW = (safe.w - PAD * 2 - (l.chipCols - 1) * CHIP_GAP) / l.chipCols;
  return l;
}

QuickSettingsPanel::Rect QuickSettingsPanel::rowRect(const Layout& l, const int row) const {
  // Landscape fills the first column top to bottom, then the second.
  const int rowLines = (ROW_COUNT + l.columns - 1) / l.columns;
  const int col = row / rowLines;
  const int line = row % rowLines;
  return Rect{l.left + col * (l.colW + PAD), l.rowsY + line * l.rowH, l.colW, l.rowH - 4};
}

QuickSettingsPanel::Rect QuickSettingsPanel::chipRect(const Layout& l, const int chip) const {
  const int col = chip % l.chipCols;
  const int line = chip / l.chipCols;
  return Rect{l.left + col * (l.chipW + CHIP_GAP), l.chipsY + line * (l.chipH + CHIP_GAP), l.chipW, l.chipH};
}

// --- Values --------------------------------------------------------------------

void QuickSettingsPanel::stepFontSize(const int direction) {
  const auto sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName);
  const uint8_t current = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
  const auto it = std::find(sizes.begin(), sizes.end(), current);
  const int index = static_cast<int>(it - sizes.begin()) + direction;
  SETTINGS.fontPointSize = sizes[std::clamp(index, 0, static_cast<int>(sizes.size()) - 1)];
}

void QuickSettingsPanel::stepFontFace(const int direction) {
  const int count = familyCount(registry);
  const int next = (currentFamilyIndex(registry) + direction + count) % count;
  if (next < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = static_cast<uint8_t>(next);
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  const auto& family = registry->getFamilies()[next - CrossPointSettings::BUILTIN_FONT_COUNT];
  snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", family.name.c_str());
}

QuickSettingsPanel::Event QuickSettingsPanel::step(const int direction) {
  changed = true;
  const auto clampStep = [direction](uint8_t& value, const int count) {
    value = static_cast<uint8_t>(std::clamp(static_cast<int>(value) + direction, 0, count - 1));
  };
  switch (static_cast<Row>(focus)) {
    case Row::FontSize:
      stepFontSize(direction);
      return Event::Relayout;
    case Row::FontFace:
      stepFontFace(direction);
      return Event::Relayout;
    case Row::LineSpacing:
      clampStep(SETTINGS.lineSpacing, CrossPointSettings::LINE_COMPRESSION_COUNT);
      return Event::Relayout;
    case Row::Margins:
      SETTINGS.screenMargin =
          static_cast<uint8_t>(std::clamp(SETTINGS.screenMargin + direction * CrossPointSettings::SCREEN_MARGIN_STEP,
                                          static_cast<int>(CrossPointSettings::SCREEN_MARGIN_MIN),
                                          static_cast<int>(CrossPointSettings::SCREEN_MARGIN_MAX)));
      return Event::Relayout;
    case Row::Alignment:
      SETTINGS.paragraphAlignment = static_cast<uint8_t>(
          (SETTINGS.paragraphAlignment + direction + CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT) %
          CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT);
      return Event::Relayout;
    case Row::Hyphenation:
      SETTINGS.hyphenationEnabled = direction > 0 ? 1 : 0;
      return Event::Relayout;
    case Row::Refresh:
      clampStep(SETTINGS.refreshFrequency, CrossPointSettings::REFRESH_FREQUENCY_COUNT);
      return Event::Repaint;
    default:
      return Event::None;
  }
}

void QuickSettingsPanel::rowSteps(const Row row, int& index, int& count) const {
  index = 0;
  count = 0;
  switch (row) {
    case Row::FontSize: {
      const auto sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName);
      const uint8_t current = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      index = static_cast<int>(std::find(sizes.begin(), sizes.end(), current) - sizes.begin());
      count = static_cast<int>(sizes.size());
      break;
    }
    case Row::LineSpacing:
      index = SETTINGS.lineSpacing;
      count = CrossPointSettings::LINE_COMPRESSION_COUNT;
      break;
    case Row::Margins:
      index = (SETTINGS.screenMargin - CrossPointSettings::SCREEN_MARGIN_MIN) / CrossPointSettings::SCREEN_MARGIN_STEP;
      count = MARGIN_STEPS;
      break;
    case Row::Alignment:
      index = SETTINGS.paragraphAlignment;
      count = CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT;
      break;
    case Row::Refresh:
      index = SETTINGS.refreshFrequency;
      count = CrossPointSettings::REFRESH_FREQUENCY_COUNT;
      break;
    default:
      break;
  }
  index = std::clamp(index, 0, std::max(0, count - 1));
}

void QuickSettingsPanel::rowValue(const Row row, char* buf, const int len) const {
  switch (row) {
    case Row::FontSize:
      snprintf(buf, len, "%u pt", static_cast<unsigned>(SETTINGS.fontPointSize));
      return;
    case Row::FontFace:
      if (SETTINGS.sdFontFamilyName[0] != '\0') {
        snprintf(buf, len, "%s", SETTINGS.sdFontFamilyName);
      } else {
        snprintf(buf, len, "%s",
                 SETTINGS.fontFamily == CrossPointSettings::NOTOSANS ? tr(STR_NOTO_SANS) : tr(STR_NOTO_SERIF));
      }
      return;
    case Row::LineSpacing:
      snprintf(buf, len, "%s", I18N.get(SPACING_LABELS[SETTINGS.lineSpacing % std::size(SPACING_LABELS)]));
      return;
    case Row::Margins:
      snprintf(buf, len, "%u px", static_cast<unsigned>(SETTINGS.screenMargin));
      return;
    case Row::Alignment:
      snprintf(buf, len, "%s", I18N.get(ALIGN_LABELS[SETTINGS.paragraphAlignment % std::size(ALIGN_LABELS)]));
      return;
    case Row::Hyphenation:
      snprintf(buf, len, "%s", SETTINGS.hyphenationEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
      return;
    case Row::Refresh:
      snprintf(buf, len, "%s", I18N.get(REFRESH_LABELS[SETTINGS.refreshFrequency % std::size(REFRESH_LABELS)]));
      return;
    default:
      buf[0] = '\0';
  }
}

// --- Input ---------------------------------------------------------------------

QuickSettingsPanel::Event QuickSettingsPanel::handleInput(const MappedInputManager& input,
                                                          const GfxRenderer& renderer) {
  using Button = MappedInputManager::Button;
  if (input.wasReleased(Button::Back)) return Event::Close;
  if (input.wasReleased(Button::Confirm)) {
    if (!onChip()) return Event::Close;
    pendingLink = static_cast<Link>(focus - ROW_COUNT);
    return Event::OpenLink;
  }
  if (input.wasPressed(Button::Up)) {
    focus = (focus + FOCUS_COUNT - 1) % FOCUS_COUNT;
    return Event::Repaint;
  }
  if (input.wasPressed(Button::Down)) {
    focus = (focus + 1) % FOCUS_COUNT;
    return Event::Repaint;
  }
  const bool left = input.wasPressed(Button::Left);
  const bool right = input.wasPressed(Button::Right);
  if (left || right) {
    const int direction = right ? 1 : -1;
    if (onChip()) {
      focus = ROW_COUNT + (focus - ROW_COUNT + direction + LINK_COUNT) % LINK_COUNT;
      return Event::Repaint;
    }
    return step(direction);
  }

  int tx = 0;
  int ty = 0;
  if (input.hasTouch() && input.wasScreenTapped(tx, ty)) {
    const Layout l = layout(renderer);
    if (ty < l.sheet.y) return Event::Close;
    for (int i = 0; i < LINK_COUNT; i++) {
      const Rect r = chipRect(l, i);
      if (tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h) {
        focus = ROW_COUNT + i;
        pendingLink = static_cast<Link>(i);
        return Event::OpenLink;
      }
    }
    for (int i = 0; i < ROW_COUNT; i++) {
      const Rect r = rowRect(l, i);
      if (tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h) {
        focus = i;
        return step(tx < r.x + r.w / 2 ? -1 : 1);
      }
    }
  }
  return Event::None;
}

// --- Drawing -------------------------------------------------------------------

void QuickSettingsPanel::drawRow(const GfxRenderer& renderer, const Rect& r, const Row row, const bool focused) const {
  const bool ink = !focused;  // text/marks colour: white on the focused (black) row
  if (focused) renderer.fillRoundedRect(r.x, r.y, r.w, r.h, RADIUS, Color::Black);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int textY = r.y + (r.h - lineH) / 2;
  // Label column sized to the widest label, capped at 40% of the row.
  int labelColW = 0;
  for (const StrId id : ROW_LABELS) {
    labelColW = std::max(labelColW, renderer.getTextWidth(UI_10_FONT_ID, I18N.get(id), EpdFontFamily::BOLD));
  }
  labelColW = std::min(labelColW, r.w * 40 / 100);
  const std::string label = renderer.truncatedText(UI_10_FONT_ID, I18N.get(ROW_LABELS[static_cast<int>(row)]),
                                                   labelColW, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, r.x + 10, textY, label.c_str(), ink, EpdFontFamily::BOLD);

  char value[40];
  rowValue(row, value, sizeof(value));
  const int valueRight = r.x + r.w - 10;
  const int valueMaxW = r.w * 27 / 100;
  if (row != Row::FontFace) {
    const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, value, valueMaxW);
    const int valueW = renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str());
    renderer.drawText(UI_10_FONT_ID, valueRight - valueW, textY, fitted.c_str(), ink);
  }

  // Stepper between the label and the value: "A ●●◉○○ A" / "◂ ●●◉○ ▸".
  const int stepLeft = r.x + 10 + labelColW + 12;
  const int stepRight = valueRight - valueMaxW - 6;
  const int cy = r.y + r.h / 2;
  if (row == Row::Hyphenation) {
    // Toggle pill.
    constexpr int PILL_W = 40;
    constexpr int PILL_H = 20;
    const int px = stepLeft + (stepRight - stepLeft - PILL_W) / 2;
    const bool on = SETTINGS.hyphenationEnabled != 0;
    renderer.drawRoundedRect(px, cy - PILL_H / 2, PILL_W, PILL_H, 2, PILL_H / 2, ink);
    const int knobX = on ? px + PILL_W - PILL_H + 3 : px + 3;
    renderer.fillRoundedRect(knobX, cy - PILL_H / 2 + 3, PILL_H - 6, PILL_H - 6, (PILL_H - 6) / 2,
                             ink ? Color::Black : Color::White);
    if (on) income_gfx::fillDither(renderer, px + 5, cy - 2, PILL_W - PILL_H - 4, 4, ink ? 8 : 0);
    return;
  }

  int index;
  int count;
  rowSteps(row, index, count);
  if (row == Row::FontFace) {
    // No steps to show: the family name sits between the chevrons.
    drawChevron(renderer, stepLeft + 3, cy, -1, ink);
    drawChevron(renderer, valueRight - 3, cy, 1, ink);
    const std::string name = renderer.truncatedText(UI_10_FONT_ID, value, valueRight - stepLeft - CHEVRON_ROOM * 2);
    const int nameW = renderer.getTextWidth(UI_10_FONT_ID, name.c_str());
    renderer.drawText(UI_10_FONT_ID, stepLeft + (valueRight - stepLeft - nameW) / 2, textY, name.c_str(), ink);
    return;
  }
  if (row == Row::FontSize) {
    renderer.drawText(SMALL_FONT_ID, stepLeft, cy - renderer.getLineHeight(SMALL_FONT_ID) / 2, "A", ink);
    const int bigW = renderer.getTextWidth(UI_12_FONT_ID, "A", EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, stepRight - bigW, cy - renderer.getLineHeight(UI_12_FONT_ID) / 2, "A", ink,
                      EpdFontFamily::BOLD);
  } else {
    drawChevron(renderer, stepLeft + 3, cy, -1, ink);
    drawChevron(renderer, stepRight - 3, cy, 1, ink);
  }
  if (count <= 1) return;
  // Long scales get smaller dots so they still fit as dots.
  const int dotD = count > 5 ? 7 : 9;
  const int dotGap = count > 5 ? 4 : 5;
  const int dotsW = count * dotD + (count - 1) * dotGap;
  const int innerLeft = stepLeft + CHEVRON_ROOM;
  const int innerRight = stepRight - CHEVRON_ROOM;
  if (dotsW > innerRight - innerLeft) {
    // Too many steps for dots: a proportional track instead.
    const int trackW = innerRight - innerLeft;
    renderer.drawRect(innerLeft, cy - 3, trackW, 6, ink);
    renderer.fillRect(innerLeft, cy - 3, trackW * (index + 1) / count, 6, ink);
    return;
  }
  int x = innerLeft + (innerRight - innerLeft - dotsW) / 2;
  for (int i = 0; i < count; i++) {
    // Filled up to the current value; the current dot also gets a ring.
    drawDot(renderer, x, cy - dotD / 2, dotD, i <= index, ink);
    if (i == index) renderer.drawRoundedRect(x - 2, cy - dotD / 2 - 2, dotD + 4, dotD + 4, 1, dotD / 2 + 2, ink);
    x += dotD + dotGap;
  }
}

void QuickSettingsPanel::drawChip(const GfxRenderer& renderer, const Rect& r, const Link link,
                                  const bool focused) const {
  if (focused) {
    renderer.fillRoundedRect(r.x, r.y, r.w, r.h, RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(r.x, r.y, r.w, r.h, 2, RADIUS, true);
  }
  const char* label = I18N.get(LINK_LABELS[static_cast<int>(link)]);
  const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, label, r.w - 6);
  const int w = renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str());
  renderer.drawText(UI_10_FONT_ID, r.x + (r.w - w) / 2, r.y + (r.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                    fitted.c_str(), !focused);
}

void QuickSettingsPanel::drawHints(GfxRenderer& renderer, const MappedInputManager& input) const {
  const char* less = tr(STR_QS_LESS);
  const char* more = tr(STR_QS_MORE);
  if (onChip()) {
    less = tr(STR_QS_PREV);
    more = tr(STR_QS_NEXT);
  } else if (static_cast<Row>(focus) == Row::FontSize) {
    less = tr(STR_QS_SMALLER);
    more = tr(STR_QS_BIGGER);
  }
  const auto labels = input.mapLabels(tr(STR_BACK), onChip() ? tr(STR_OPEN) : tr(STR_DONE), less, more);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // The edge side buttons only line up with their hints in portrait.
  if (renderer.getOrientation() == GfxRenderer::Orientation::Portrait) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
}

void QuickSettingsPanel::render(GfxRenderer& renderer, const MappedInputManager& input, const char* timeLeft) const {
  const Layout l = layout(renderer);
  const Rect& s = l.sheet;
  // Sheet: white card, a dithered shadow band and a rule separating it from the page.
  // The card spans the full width so no page text peeks out beside the button hints.
  const int screenW = renderer.getScreenWidth();
  renderer.fillRect(0, s.y, screenW, renderer.getScreenHeight() - s.y, false);
  income_gfx::fillDither(renderer, 0, s.y - SHADOW_H, screenW, SHADOW_H, 4);
  renderer.fillRect(0, s.y - 1, screenW, 2, true);

  renderer.drawText(UI_12_FONT_ID, l.left, l.headerY, tr(STR_QUICK_SETTINGS), true, EpdFontFamily::BOLD);
  if (timeLeft && timeLeft[0] != '\0') {
    const std::string fitted = renderer.truncatedText(SMALL_FONT_ID, timeLeft, s.w - PAD * 2);
    renderer.drawText(SMALL_FONT_ID, l.left, l.headerY + renderer.getLineHeight(UI_12_FONT_ID) + 1, fitted.c_str());
  }
  // Side-button cue at the header's right: "▲▼ Rows".
  const char* rowsLabel = tr(STR_QS_ROWS);
  const int rowsW = renderer.getTextWidth(SMALL_FONT_ID, rowsLabel);
  const int rowsX = s.x + s.w - PAD - rowsW;
  renderer.drawText(SMALL_FONT_ID, rowsX, l.headerY + 4, rowsLabel);
  const int arrowX = rowsX - 12;
  const int arrowY = l.headerY + 4 + renderer.getLineHeight(SMALL_FONT_ID) / 2;
  const int upX[3] = {arrowX - 5, arrowX + 5, arrowX};
  const int upY[3] = {arrowY - 2, arrowY - 2, arrowY - 8};
  const int downY[3] = {arrowY + 2, arrowY + 2, arrowY + 8};
  renderer.fillPolygon(upX, upY, 3, true);
  renderer.fillPolygon(upX, downY, 3, true);

  for (int i = 0; i < ROW_COUNT; i++) drawRow(renderer, rowRect(l, i), static_cast<Row>(i), i == focus);
  for (int i = 0; i < LINK_COUNT; i++) drawChip(renderer, chipRect(l, i), static_cast<Link>(i), focus == ROW_COUNT + i);
  drawHints(renderer, input);
}
