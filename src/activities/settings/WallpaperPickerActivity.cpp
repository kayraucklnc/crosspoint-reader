#include "WallpaperPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WallpaperStore.h"

namespace {
constexpr int THUMB_GAP = 16;
constexpr int ROW_TEXT_INSET = 10;
constexpr int CHECK_WIDTH = 24;
constexpr int COUNTER_HEIGHT = 28;
constexpr int CAPTION_GAP = 10;

const char* modeLabel(const uint8_t mode) {
  switch (mode) {
    case CrossPointSettings::WALLPAPER_CYCLE:
      return tr(STR_WALLPAPER_CYCLE);
    case CrossPointSettings::WALLPAPER_FIXED:
      return tr(STR_WALLPAPER_FIXED);
    default:
      return tr(STR_WALLPAPER_RANDOM);
  }
}

void drawCheck(const GfxRenderer& renderer, const int cx, const int cy) {
  renderer.drawLine(cx - 8, cy, cx - 3, cy + 6, 3, true);
  renderer.drawLine(cx - 3, cy + 6, cx + 9, cy - 7, 3, true);
}
}  // namespace

void WallpaperPickerActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    names = wallpaper::list();
    fixedName = wallpaper::loadState().fixedName;
  }
  const auto it = std::find(names.begin(), names.end(), fixedName);
  index = it == names.end() ? 0 : static_cast<int>(it - names.begin());
  requestUpdate();
}

void WallpaperPickerActivity::onExit() {
  // ActivityManager already holds the render lock here.
  thumbs = wallpaper::ThumbLoader();
  Activity::onExit();
}

void WallpaperPickerActivity::move(const int delta) {
  const int count = static_cast<int>(names.size());
  index = ((index.load() + delta) % count + count) % count;
  requestUpdate();
}

void WallpaperPickerActivity::choose() {
  {
    RenderLock lock(*this);
    wallpaper::State state = wallpaper::loadState();
    state.fixedName = names[index.load()];
    wallpaper::saveState(state);
    SETTINGS.wallpaperMode = CrossPointSettings::WALLPAPER_FIXED;
    SETTINGS.saveToFile();
    fixedName = names[index.load()];
  }
  requestUpdate();
}

void WallpaperPickerActivity::cycleMode() {
  {
    RenderLock lock(*this);
    SETTINGS.wallpaperMode = static_cast<uint8_t>((SETTINGS.wallpaperMode + 1) % 3);
    SETTINGS.saveToFile();
  }
  requestUpdate();
}

void WallpaperPickerActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasPressed(Button::Back)) {
    finish();
    return;
  }
  if (names.empty()) return;
  if (mappedInput.wasPressed(Button::Confirm)) {
    choose();
  } else if (mappedInput.wasPressed(Button::Left)) {
    preview = !preview.load();
    requestUpdate();
  } else if (mappedInput.wasPressed(Button::Right)) {
    cycleMode();
  }
  nav.onPressAndContinuous({Button::Up}, [this] { move(-1); });
  nav.onPressAndContinuous({Button::Down}, [this] { move(1); });
}

WallpaperPickerActivity::Layout WallpaperPickerActivity::layout(const bool withPreview) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing / 2;
  Layout lay{};
  lay.top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing / 2;
  lay.rowHeight = metrics.listRowHeight;
  lay.rows = std::max(1, (bottom - lay.top - COUNTER_HEIGHT) / lay.rowHeight);
  lay.listX = metrics.contentSidePadding / 2;
  // Half the screen in each direction keeps the thumbnail in the screen's aspect ratio.
  lay.thumbWidth = std::min(wallpaper::THUMB_MAX_W, width / 2);
  lay.thumbHeight = std::min(wallpaper::THUMB_MAX_H, renderer.getScreenHeight() / 2);
  lay.thumbX = width - metrics.contentSidePadding - lay.thumbWidth;
  const int listRight = withPreview ? lay.thumbX - THUMB_GAP : width - metrics.contentSidePadding / 2;
  lay.listWidth = listRight - lay.listX;
  return lay;
}

void WallpaperPickerActivity::drawHeaderAndHints(const bool withPreview) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  char subtitle[64];
  snprintf(subtitle, sizeof(subtitle), "%s: %s", tr(STR_WALLPAPER_MODE), modeLabel(SETTINGS.wallpaperMode));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_WALLPAPER), subtitle);
  const auto labels =
      mappedInput.mapFrontLabels(tr(STR_BACK), tr(STR_WALLPAPER_SET),
                                 withPreview ? tr(STR_WALLPAPER_LIST) : tr(STR_PREVIEW), tr(STR_WALLPAPER_MODE));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void WallpaperPickerActivity::drawList(const Layout& lay, const int focus) const {
  const int count = static_cast<int>(names.size());
  const int first = focus / lay.rows * lay.rows;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  char label[128];
  for (int row = 0; row < lay.rows && first + row < count; row++) {
    const int i = first + row;
    const int y = lay.top + row * lay.rowHeight;
    if (i == focus) renderer.fillRectDither(lay.listX, y, lay.listWidth, lay.rowHeight, Color::LightGray);
    wallpaper::displayName(names[i], label, sizeof(label));
    const std::string text =
        renderer.truncatedText(UI_10_FONT_ID, label, lay.listWidth - ROW_TEXT_INSET * 2 - CHECK_WIDTH);
    renderer.drawText(UI_10_FONT_ID, lay.listX + ROW_TEXT_INSET, y + (lay.rowHeight - lineHeight) / 2, text.c_str());
    if (names[i] == fixedName) {
      drawCheck(renderer, lay.listX + lay.listWidth - ROW_TEXT_INSET - CHECK_WIDTH / 2, y + lay.rowHeight / 2);
    }
  }

  char counter[24];
  snprintf(counter, sizeof(counter), "%d / %d", focus + 1, count);
  const int counterWidth = renderer.getTextWidth(SMALL_FONT_ID, counter);
  renderer.drawText(SMALL_FONT_ID, lay.listX + (lay.listWidth - counterWidth) / 2,
                    lay.top + lay.rows * lay.rowHeight + (COUNTER_HEIGHT - renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                    counter);
}

void WallpaperPickerActivity::drawPreview(const Layout& lay, const int focus) {
  if (thumbIndex != focus) {
    thumbOk = thumbs.load(names[focus], lay.thumbWidth, lay.thumbHeight) != wallpaper::ThumbLoader::Result::Failed;
    thumbIndex = focus;
  }
  const int boxW = lay.thumbWidth;
  const int boxH = lay.thumbHeight;
  renderer.drawRect(lay.thumbX - 3, lay.top - 3, boxW + 6, boxH + 6, 2, true);

  if (thumbOk) {
    const int w = thumbs.width();
    const int h = thumbs.height();
    // Letterbox bars match the dark art most wallpapers use.
    if (w < boxW || h < boxH) renderer.fillRect(lay.thumbX, lay.top, boxW, boxH, true);
    const int ox = lay.thumbX + (boxW - w) / 2;
    const int oy = lay.top + (boxH - h) / 2;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) renderer.drawPixel(ox + x, oy + y, !thumbs.isWhite(x, y));
    }
  } else {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_WALLPAPER_UNREADABLE), boxW - 16, 3);
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = lay.top + (boxH - lineHeight * static_cast<int>(lines.size())) / 2;
    for (const auto& line : lines) {
      renderer.drawText(UI_10_FONT_ID, lay.thumbX + (boxW - renderer.getTextWidth(UI_10_FONT_ID, line.c_str())) / 2, y,
                        line.c_str());
      y += lineHeight;
    }
  }

  char label[128];
  wallpaper::displayName(names[focus], label, sizeof(label));
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, label, boxW, 2, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = lay.top + boxH + CAPTION_GAP;
  for (const auto& line : lines) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, lay.thumbX + (boxW - textWidth) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight;
  }
}

void WallpaperPickerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool showPreview = preview;
  const uint8_t mode = SETTINGS.wallpaperMode;
  const int count = static_cast<int>(names.size());
  const int focus = count > 0 ? std::min(index.load(), count - 1) : 0;
  const Layout lay = layout(showPreview);

  // Header and hints only change with the view or the mode; a focus move
  // repaints the content band between them.
  const bool full = !firstRenderDone || showPreview != shownPreview || mode != shownMode;
  if (full) {
    renderer.clearScreen();
    drawHeaderAndHints(showPreview);
  } else {
    const int contentTop = metrics.topPadding + metrics.headerHeight;
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(),
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - contentTop, false);
  }

  if (count == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, lay.top + lay.rows * lay.rowHeight / 2, tr(STR_WALLPAPER_EMPTY), true,
                              EpdFontFamily::BOLD);
  } else {
    drawList(lay, focus);
    if (showPreview) drawPreview(lay, focus);
  }

  renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  firstRenderDone = true;
  shownPreview = showPreview;
  shownMode = mode;
}
