#include "SketchActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "PlayCommon.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"
#include "util/WallpaperStore.h"

namespace {
constexpr const char* PACK_PATH = "/.crosspoint/avatars/parts.bin";
constexpr int HEADER_HEIGHT = 58;
constexpr int INDEX_ENTRY = 16;
constexpr unsigned long SAVE_HOLD_MS = 800;
constexpr int PLACEHOLDER_SIZE = 300;
constexpr int PACK_HEADER = 9;  // "AVT2" + u16 w + u16 h + u8 categories

uint32_t readU32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
int readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | p[1] << 8); }
int readU16(const uint8_t* p) { return p[0] | p[1] << 8; }
}  // namespace

void SketchActivity::onEnter() {
  Activity::onEnter();
  rng = static_cast<uint32_t>(millis()) * 2654435761u | 1u;
  packReady = loadPack();
  if (!packReady) LOG_ERR("SKETCH", "Avatar pack missing or invalid: %s", PACK_PATH);
  randomize();
}

// Header layout is documented in scripts/gen_avatars.py.
bool SketchActivity::loadPack() {
  HalFile file;
  if (!Storage.exists(PACK_PATH) || !Storage.openFileForRead("SKETCH", PACK_PATH, file)) return false;
  uint8_t header[PACK_HEADER];
  if (file.read(header, PACK_HEADER) != PACK_HEADER || memcmp(header, "AVT2", 4) != 0) return false;
  boxW = static_cast<uint16_t>(readU16(header + 4));
  boxH = static_cast<uint16_t>(readU16(header + 6));
  categoryCount = header[8];
  if (categoryCount == 0 || categoryCount > MAX_CATEGORIES) return false;
  for (int c = 0; c < categoryCount; c++) {
    uint8_t meta[4];
    if (file.read(meta, 4) != 4 || meta[0] == 0) return false;
    counts[c] = meta[0];
    randomPercent[c] = meta[1];
    hasNone[c] = meta[2] & 1;
    char name[MAX_NAME];
    if (meta[3] >= MAX_NAME || file.read(name, meta[3]) != meta[3]) return false;
    snprintf(names[c], MAX_NAME, "%.*s", static_cast<int>(meta[3]), name);
  }
  if (file.read(uiOrder, categoryCount) != categoryCount) return false;
  for (int c = 0; c < categoryCount; c++) {
    if (uiOrder[c] >= categoryCount) return false;
  }
  indexBase = static_cast<uint16_t>(file.position());
  return true;
}

void SketchActivity::randomize() {
  for (int i = 0; i < categoryCount; i++) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const int first = hasNone[i] ? 1 : 0;
    const bool wear = (rng >> 24) % 100 < randomPercent[i] && counts[i] > first;
    choice[i] = wear ? static_cast<uint8_t>(first + rng % (counts[i] - first)) : 0;
  }
  requestUpdate();
}

int SketchActivity::category() const { return uiOrder[part]; }

void SketchActivity::cycle(const int delta) {
  const int c = category();
  choice[c] = static_cast<uint8_t>((choice[c] + counts[c] + delta) % counts[c]);
  requestUpdate();
}

void SketchActivity::loop() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    finish();
    return;
  }
  if (!packReady) return;
  RenderLock lock(*this);
  if (mappedInput.wasLongPressed(B::Confirm, SAVE_HOLD_MS)) {
    saveRequested = true;
    requestUpdate();
    return;
  }
  const char* previousToast = toast;
  toast = nullptr;
  if (mappedInput.wasReleased(B::Confirm)) randomize();
  if (mappedInput.wasPressed(B::Left)) cycle(-1);
  if (mappedInput.wasPressed(B::Right)) cycle(+1);
  if (mappedInput.wasPressed(B::Up)) {
    part = static_cast<uint8_t>((part + categoryCount - 1) % categoryCount);
    requestUpdate();
  }
  if (mappedInput.wasPressed(B::Down)) {
    part = static_cast<uint8_t>((part + 1) % categoryCount);
    requestUpdate();
  }
  if (previousToast && !toast) toast = previousToast;  // keep "Saved" until something changes
}

// Streams each chosen part's RLE runs from SD straight into the framebuffer.
void SketchActivity::drawAvatar(const int x, const int y) const {
  HalFile file;
  if (!Storage.openFileForRead("SKETCH", PACK_PATH, file)) return;
  int first = 0;
  for (int c = 0; c < categoryCount; first += counts[c], c++) {
    uint8_t entry[INDEX_ENTRY];
    if (!file.seek(indexBase + (first + choice[c]) * INDEX_ENTRY) || file.read(entry, INDEX_ENTRY) != INDEX_ENTRY) {
      return;
    }
    const uint32_t offset = readU32(entry), length = readU32(entry + 4);
    const int px = x + readI16(entry + 8), py = y + readI16(entry + 10), w = readU16(entry + 12);
    if (length == 0 || w == 0 || !file.seek(offset)) continue;
    uint8_t chunk[256];
    uint32_t pos = 0;
    for (uint32_t done = 0; done < length;) {
      const int n = file.read(chunk, std::min<uint32_t>(sizeof(chunk), length - done));
      if (n <= 0) return;
      done += n;
      for (int i = 0; i < n; i++) {
        const int state = chunk[i] >> 6;
        int run = (chunk[i] & 63) + 1;
        while (run > 0) {
          const int col = pos % w, row = pos / w;
          const int span = std::min(run, w - col);
          if (state != 0) renderer.fillRect(px + col, py + row, span, 1, state == 2);
          pos += span;
          run -= span;
        }
      }
    }
  }
}

void SketchActivity::drawPlaceholder(const int x, const int y) const {
  const int r = PLACEHOLDER_SIZE / 2;
  renderer.drawRoundedRect(x, y, PLACEHOLDER_SIZE, PLACEHOLDER_SIZE, 6, r, true);
  renderer.fillRoundedRect(x + r - 60, y + r - 40, 24, 24, 12, Color::Black);
  renderer.fillRoundedRect(x + r + 36, y + r - 40, 24, 24, 12, Color::Black);
  renderer.drawLine(x + r - 50, y + r + 60, x + r + 50, y + r + 60, 6, true);
}

int SketchActivity::captionY() const {
  return renderer.getScreenHeight() - UITheme::getInstance().getMetrics().buttonHintsHeight - 34;
}

int SketchActivity::avatarY() const {
  const int height = packReady ? boxH : PLACEHOLDER_SIZE;
  return HEADER_HEIGHT + std::max(0, (captionY() - 8 - HEADER_HEIGHT - height) / 2);
}

void SketchActivity::drawHeader() const {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, HEADER_HEIGHT, false);
  renderer.drawText(UI_12_FONT_ID, 20, 16, tr(STR_PLAY_SKETCH), true, EpdFontFamily::BOLD);
  if (!packReady) return;
  const int c = category();
  char text[48];
  if (hasNone[c] && choice[c] == 0) {
    snprintf(text, sizeof(text), "%s  -", names[c]);
  } else {
    const int offset = hasNone[c] ? 0 : 1;
    snprintf(text, sizeof(text), "%s  %d/%d", names[c], choice[c] + offset, counts[c] + offset - 1);
  }
  const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, width - w - 20, 16, text, true, EpdFontFamily::BOLD);
  for (int i = 0; i < categoryCount; i++) {
    const int x = width - 20 - (categoryCount - i) * 14;
    if (i == part) {
      renderer.fillRect(x, 48, 10, 4, true);
    } else {
      renderer.fillRect(x + 3, 49, 4, 2, true);
    }
  }
}

bool SketchActivity::saveWallpaper() {
  char path[48];
  for (int i = 1; i < 1000; i++) {
    snprintf(path, sizeof(path), "%s/avatar-%03d.bmp", wallpaper::DIR, i);
    if (!Storage.exists(path)) break;
  }
  renderer.clearScreen();
  drawAvatar((renderer.getScreenWidth() - boxW) / 2, (renderer.getScreenHeight() - boxH) / 2);
  const bool ok = ScreenshotUtil::saveFramebufferAsBmp(path, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                                       renderer.getDisplayHeight());
  LOG_INF("SKETCH", "%s %s", ok ? "saved" : "failed to save", path);
  return ok;
}

void SketchActivity::render(RenderLock&& lock) {
  bool clean = fullRedraw;
  if (saveRequested) {
    saveRequested = false;
    toast = saveWallpaper() ? tr(STR_SK_SAVED) : tr(STR_SK_SAVE_FAILED);
    clean = true;
  }
  const int width = renderer.getScreenWidth();
  if (clean) {
    renderer.clearScreen();
    fullRedraw = false;
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), packReady ? tr(STR_SK_RANDOM) : "", packReady ? tr(STR_DIR_LEFT) : "",
                              packReady ? tr(STR_DIR_RIGHT) : "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.fillRect(0, HEADER_HEIGHT, width, captionY() + 30 - HEADER_HEIGHT, false);
  }
  drawHeader();
  if (packReady) {
    drawAvatar((width - boxW) / 2, avatarY());
  } else {
    drawPlaceholder((width - PLACEHOLDER_SIZE) / 2, avatarY());
  }
  const char* caption = !packReady ? tr(STR_SK_MISSING) : (toast ? toast : tr(STR_SK_SAVE_HINT));
  renderer.drawCenteredText(UI_10_FONT_ID, captionY(), caption);
  lock.unlock();
  play::FramePacer::present(renderer, clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}
