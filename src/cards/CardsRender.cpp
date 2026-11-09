#include "CardsRender.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <qrcode.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "CardsHotspot.h"
#include "fontIds.h"

namespace cards::render {

namespace {
constexpr int MARGIN = 16;
constexpr int TEXT_GAP = 14;
constexpr int QR_MAX_VERSION = 40;
// Light-coloured modules the scanner needs around the code, in modules.
constexpr int QUIET_MODULES = 2;
constexpr int TITLE_FONT = NOTOSANS_18_FONT_ID;
constexpr int SUBTITLE_FONT = UI_12_FONT_ID;
constexpr int TITLE_LINES = 2;

// Padlock, 11 x 14: shackle over a solid body.
void drawLock(const GfxRenderer& renderer, const int x, const int y) {
  renderer.drawRoundedRect(x + 2, y, 7, 9, 2, 3, true);
  renderer.fillRect(x, y + 6, 11, 8, true);
  renderer.fillRect(x + 5, y + 9, 1, 3, false);
}

// Three rising bars: the editing hotspot is on.
void drawHotspotMark(const GfxRenderer& renderer, const int x, const int bottom) {
  for (int i = 0; i < 3; i++) renderer.fillRect(x + i * 5, bottom - 4 - i * 4, 3, 4 + i * 4, true);
}

// The top strip: hotspot mark on the left, lock and "2/5" on the right.
void drawStatusBand(const GfxRenderer& renderer, const PageInfo& page) {
  const int width = renderer.getScreenWidth();
  renderer.fillRect(0, 0, width, STATUS_BAND, false);
  const int textY = (STATUS_BAND - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  char count[16];
  snprintf(count, sizeof(count), "%d/%d", page.index + 1, page.count);
  const int countWidth = renderer.getTextWidth(SMALL_FONT_ID, count);
  const int countX = width - 8 - countWidth;
  renderer.drawText(SMALL_FONT_ID, countX, textY, count);
  drawLock(renderer, countX - 17, (STATUS_BAND - 14) / 2);
  if (page.hotspot) drawHotspotMark(renderer, 8, STATUS_BAND - 4);
}

struct TextBlock {
  std::vector<std::string> titleLines;
  std::string subtitle;
  int height = 0;
};

TextBlock layoutText(const GfxRenderer& renderer, const Card& card, const int width) {
  TextBlock block;
  if (!card.title.empty()) {
    block.titleLines = renderer.wrappedText(TITLE_FONT, card.title.c_str(), width, TITLE_LINES, EpdFontFamily::BOLD);
    block.height += static_cast<int>(block.titleLines.size()) * renderer.getLineHeight(TITLE_FONT);
  }
  if (!card.subtitle.empty()) {
    block.subtitle = renderer.truncatedText(SUBTITLE_FONT, card.subtitle.c_str(), width);
    block.height += renderer.getLineHeight(SUBTITLE_FONT) + (block.titleLines.empty() ? 0 : 4);
  }
  return block;
}

void drawText(const GfxRenderer& renderer, const TextBlock& block, int y) {
  for (const auto& line : block.titleLines) {
    renderer.drawCenteredText(TITLE_FONT, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(TITLE_FONT);
  }
  if (!block.subtitle.empty()) {
    if (!block.titleLines.empty()) y += 4;
    renderer.drawCenteredText(SUBTITLE_FONT, y, block.subtitle.c_str());
  }
}

void drawQrCard(const GfxRenderer& renderer, const Card& card) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const TextBlock text = layoutText(renderer, card, width - MARGIN * 2);
  const int textSpace = text.height > 0 ? text.height + TEXT_GAP : 0;
  const int areaTop = STATUS_BAND;
  const int areaHeight = height - areaTop - MARGIN;
  const int maxSide = std::min(width - MARGIN * 2, areaHeight - textSpace);

  // Measure first so code and caption sit together, centred in the space.
  const int side = drawQr(renderer, card.payload, width / 2, -1, maxSide);
  const int blockHeight = side + textSpace;
  const int top = areaTop + std::max(0, (areaHeight - blockHeight) / 2);
  if (side == 0 || drawQr(renderer, card.payload, width / 2, top, maxSide) == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, tr(STR_CARDS_TOO_LONG), true, EpdFontFamily::BOLD);
    return;
  }
  if (text.height > 0) drawText(renderer, text, top + side + TEXT_GAP);
}

void drawImageCard(const GfxRenderer& renderer, const Card& card) {
  HalFile file;
  const std::string path = CardStore::imagePath(card.id);
  if (Storage.openFileForRead("CARDS", path.c_str(), file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok &&
        renderer.drawBitmap(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight())) {
      return;
    }
    LOG_ERR("CARDS", "Cannot draw %s", path.c_str());
  }
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_CARDS_IMAGE_MISSING), true,
                            EpdFontFamily::BOLD);
}

int qrStepLabel(const GfxRenderer& renderer, const char* label, const char* detail, int y) {
  renderer.drawCenteredText(UI_12_FONT_ID, y, label, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_10_FONT_ID, y, detail);
  return y + renderer.getLineHeight(UI_10_FONT_ID) + 8;
}
}  // namespace

int drawQr(const GfxRenderer& renderer, const std::string& text, const int centerX, const int y, const int maxSide) {
  if (text.empty() || maxSide <= 0) return 0;
  // The smallest version that holds the text gives the biggest modules.
  // Medium error correction survives a scuffed or reflective screen.
  for (int version = 1; version <= QR_MAX_VERSION; version++) {
    const uint16_t bufferSize = qrcode_getBufferSize(version);
    auto modules = makeUniqueNoThrow<uint8_t[]>(bufferSize);
    if (!modules) return 0;
    QRCode qr;
    if (qrcode_initText(&qr, modules.get(), version, ECC_MEDIUM, text.c_str()) != 0) continue;
    const int px = maxSide / (qr.size + QUIET_MODULES * 2);
    if (px < 1) return 0;
    const int side = qr.size * px;
    if (y < 0) return side;  // measuring only
    const int x0 = centerX - side / 2;
    for (uint8_t my = 0; my < qr.size; my++) {
      for (uint8_t mx = 0; mx < qr.size; mx++) {
        if (qrcode_getModule(&qr, mx, my)) renderer.fillRect(x0 + mx * px, y + my * px, px, px, true);
      }
    }
    return side;
  }
  return 0;
}

void drawCard(const GfxRenderer& renderer, const Card& card, const PageInfo& page) {
  if (card.type == CardType::Image) {
    drawImageCard(renderer, card);
  } else {
    drawQrCard(renderer, card);
  }
  drawStatusBand(renderer, page);
}

void drawEditorPage(const GfxRenderer& renderer, const PageInfo& page, const std::string& editorUrl,
                    const bool failed) {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  int y = MARGIN + 6;
  renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_CARDS_EDIT_TITLE), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  if (page.count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CARDS_EMPTY));
  }
  y += renderer.getLineHeight(UI_10_FONT_ID) + 10;

  if (editorUrl.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2, failed ? tr(STR_CARDS_HOTSPOT_FAILED) : tr(STR_CARDS_STARTING),
                              true, EpdFontFamily::BOLD);
    return;
  }

  // Two codes stacked, each with its step label: join the network, then open the editor.
  const int labelHeight = renderer.getLineHeight(UI_12_FONT_ID) + renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int hints = 48;  // button hints along the bottom
  const int qrSide = std::min(width - MARGIN * 2, (height - y - hints - labelHeight * 2 - MARGIN) / 2);

  const std::string wifi = std::string("WIFI:T:nopass;S:") + CardsHotspot::SSID + ";;";
  y = qrStepLabel(renderer, tr(STR_CARDS_STEP_WIFI), CardsHotspot::SSID, y);
  y += drawQr(renderer, wifi, width / 2, y, qrSide) + 6;
  y = qrStepLabel(renderer, tr(STR_CARDS_STEP_OPEN), editorUrl.c_str(), y);
  drawQr(renderer, editorUrl, width / 2, y, qrSide);
}

}  // namespace cards::render
