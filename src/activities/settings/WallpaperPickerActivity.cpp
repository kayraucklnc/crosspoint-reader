#include "WallpaperPickerActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/WallpaperStore.h"

namespace {
constexpr int MARGIN = 24;
}

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

void WallpaperPickerActivity::choose() {
  if (names.empty()) return;
  {
    RenderLock lock(*this);
    wallpaper::State state = wallpaper::loadState();
    state.fixedName = names[index];
    wallpaper::saveState(state);
    SETTINGS.wallpaperMode = CrossPointSettings::WALLPAPER_FIXED;
    SETTINGS.saveToFile();
  }
  fixedName = names[index];
  requestUpdate();
}

void WallpaperPickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (names.empty()) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    choose();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
             mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    index = (index + static_cast<int>(names.size()) - 1) % static_cast<int>(names.size());
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
             mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    index = (index + 1) % static_cast<int>(names.size());
    requestUpdate();
  }
}

void WallpaperPickerActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_WALLPAPER));
  const int top = metrics.topPadding + metrics.headerHeight + 8;
  const int bottom = height - metrics.buttonHintsHeight - 64;

  if (names.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, (top + bottom) / 2, tr(STR_WALLPAPER_EMPTY), true, EpdFontFamily::BOLD);
  } else {
    // Preview keeps the screen's aspect ratio so it reads as a miniature of the sleep screen.
    const int previewHeight = bottom - top;
    const int previewWidth = previewHeight * width / height;
    const int previewX = (width - previewWidth) / 2;
    const std::string path = std::string(wallpaper::DIR) + "/" + names[index];
    HalFile file;
    bool drawn = false;
    if (Storage.openFileForRead("WALL", path, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const float scale = std::min(static_cast<float>(previewWidth) / bitmap.getWidth(),
                                     static_cast<float>(previewHeight) / bitmap.getHeight());
        const int w = static_cast<int>(bitmap.getWidth() * scale);
        const int h = static_cast<int>(bitmap.getHeight() * scale);
        drawn = renderer.drawBitmap(bitmap, previewX + (previewWidth - w) / 2, top + (previewHeight - h) / 2, w, h);
      }
    }
    renderer.drawRect(previewX - 3, top - 3, previewWidth + 6, previewHeight + 6, 2, true);
    if (!drawn) renderer.drawCenteredText(UI_10_FONT_ID, top + previewHeight / 2, tr(STR_WALLPAPER_UNREADABLE));

    char caption[160];
    const bool current = names[index] == fixedName && SETTINGS.wallpaperMode == CrossPointSettings::WALLPAPER_FIXED;
    snprintf(caption, sizeof(caption), "%s%s", names[index].c_str(), current ? "  *" : "");
    renderer.drawCenteredText(UI_12_FONT_ID, bottom + 12, caption, true, EpdFontFamily::BOLD);
    snprintf(caption, sizeof(caption), "%d / %d", index + 1, static_cast<int>(names.size()));
    renderer.drawCenteredText(SMALL_FONT_ID, bottom + 40, caption);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_WALLPAPER_SET), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  firstRenderDone = true;
}
