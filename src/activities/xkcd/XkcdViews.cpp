#include <Arduino.h>
#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "XkcdActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SIDE = 8;
constexpr int ALT_PADDING = 16;
}  // namespace

// A stick figure in the xkcd manner.
void XkcdActivity::drawMenuIcon(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int cx = x + size / 2;
  const int head = size / 7;
  const int headY = y + size / 5;
  renderer.drawRoundedRect(cx - head, headY - head, 2 * head, 2 * head, 3, head, true);
  const int neck = headY + head;
  const int hip = y + size * 2 / 3;
  renderer.drawLine(cx, neck, cx, hip, 3, true);
  renderer.drawLine(cx, neck + size / 10, cx - size / 4, neck + size / 4, 3, true);
  renderer.drawLine(cx, neck + size / 10, cx + size / 4, neck + size / 16, 3, true);
  renderer.drawLine(cx, hip, cx - size / 5, y + size - 4, 3, true);
  renderer.drawLine(cx, hip, cx + size / 5, y + size - 4, 3, true);
}

void XkcdActivity::render(RenderLock&&) {
  if (checking) {
    // Over the frame already on the panel: one quick refresh.
    char text[48];
    if (fetchingNum > 0) {
      snprintf(text, sizeof(text), tr(STR_XKCD_DOWNLOADING), fetchingNum);
    } else {
      snprintf(text, sizeof(text), "%s", tr(STR_XKCD_CHECKING));
    }
    GUI.drawPopup(renderer, text);
    return;
  }
  const auto orientation =
      haveComic && landscape ? GfxRenderer::Orientation::LandscapeCounterClockwise : GfxRenderer::Orientation::Portrait;
  renderer.setOrientation(orientation);
  renderer.clearScreen();
  if (haveComic) {
    drawComic();
  } else {
    drawMessage();
  }
  drawHints();
  // A turned layout replaces everything on the panel, so it gets the cleaner refresh.
  const bool turned = orientation != drawnOrientation || firstFrame;
  drawnOrientation = orientation;
  firstFrame = false;
  renderer.displayBuffer(turned ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

// Header text on the right: the last check's outcome, else the comic's date.
const char* XkcdActivity::statusText(char* buffer, const size_t size) const {
  switch (netError) {
    case NetError::NoWifi:
      return tr(STR_XKCD_ERR_WIFI);
    case NetError::LowMemory:
      return tr(STR_XKCD_ERR_MEMORY);
    case NetError::Server:
      snprintf(buffer, size, tr(STR_XKCD_ERR_SERVER), netStatus);
      return buffer;
    case NetError::Network:
      return tr(STR_XKCD_OFFLINE);
    case NetError::SdCard:
      return tr(STR_XKCD_ERR_STORAGE);
    case NetError::BadData:
      return tr(STR_XKCD_ERR_DATA);
    case NetError::None:
      break;
  }
  if (checkResult == 0) return tr(STR_XKCD_UP_TO_DATE);
  if (checkResult > 0) {
    snprintf(buffer, size, tr(STR_XKCD_NEW_COUNT), checkResult);
    return buffer;
  }
  if (comic.year == 0) return "";
  snprintf(buffer, size, "%04u-%02u-%02u", comic.year, comic.month, comic.day);
  return buffer;
}

void XkcdActivity::drawComic() {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = area.x + SIDE;
  const int right = area.x + area.width - SIDE;

  // Header: number and title on the left, date (or check status) on the right.
  char statusBuffer[48];
  const char* status = statusText(statusBuffer, sizeof(statusBuffer));
  char number[12];
  snprintf(number, sizeof(number), "#%u", comic.num);
  const int statusWidth = status[0] ? renderer.getTextWidth(UI_10_FONT_ID, status) : 0;
  const int numberWidth = renderer.getTextWidth(UI_12_FONT_ID, number, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, left, 6, number, true, EpdFontFamily::BOLD);
  const std::string title = renderer.truncatedText(
      UI_12_FONT_ID, comic.title.c_str(), right - left - numberWidth - statusWidth - 28, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, left + numberWidth + 10, 6, title.c_str());
  if (status[0]) renderer.drawText(UI_10_FONT_ID, right - statusWidth, 10, status);
  renderer.fillRect(left, HEADER_HEIGHT - 2, right - left, 1, true);

  const int top = HEADER_HEIGHT + 4;
  const int bottom = area.y + area.height - 4;
  drawImage(top, bottom);
  if (showAlt) drawAltText(top, bottom);
}

void XkcdActivity::drawImage(const int top, const int bottom) {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = area.x + SIDE;
  const int boxW = area.width - 2 * SIDE;
  const int boxH = bottom - top;
  const unsigned long started = millis();

  if (comic.fromPack) {
    // Pack images are pre-sized to this box; centre them.
    const XkcdPack::Entry& entry = comic.packEntry;
    const int x = left + std::max(0, (boxW - entry.width) / 2);
    const int y = top + std::max(0, (boxH - entry.height) / 2);
    if (!pack.draw(entry, renderer, x, y)) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + 20, tr(STR_XKCD_NO_IMAGE));
      return;
    }
    LOG_DBG("XKCD", "Pack #%u drawn in %lu ms (%u bytes)", comic.num, millis() - started,
            static_cast<unsigned>(entry.dataSize));
    return;
  }

  if (comic.imagePath.empty() || comic.width <= 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, top + 20, tr(STR_XKCD_NO_IMAGE));
    if (!showAlt) drawAltText(top + 60, bottom);
    return;
  }
  const float scale =
      std::min({static_cast<float>(boxW) / comic.width, static_cast<float>(boxH) / comic.height, MAX_UPSCALE});
  const int w = std::max(1, static_cast<int>(comic.width * scale));
  const int h = std::max(1, static_cast<int>(comic.height * scale));
  RenderConfig config{left + (boxW - w) / 2, top + (boxH - h) / 2, w, h};
  config.useGrayscale = false;
  config.useExactDimensions = true;
  const bool png = comic.imagePath.compare(comic.imagePath.size() - 4, 4, ".png") == 0;
  bool drawn;
  if (png) {
    PngToFramebufferConverter decoder;
    drawn = decoder.decodeToFramebuffer(comic.imagePath, renderer, config);
  } else {
    JpegToFramebufferConverter decoder;
    drawn = decoder.decodeToFramebuffer(comic.imagePath, renderer, config);
  }
  if (!drawn) {
    LOG_ERR("XKCD", "Decode failed for #%u (heap %u free, %u max block)", comic.num,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    renderer.drawCenteredText(UI_10_FONT_ID, top + 20, tr(STR_XKCD_NO_IMAGE));
  }
}

// The alt text as a card over the lower part of the comic.
void XkcdActivity::drawAltText(const int top, const int bottom) const {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int x = area.x + SIDE + 6;
  const int width = area.width - 2 * (SIDE + 6);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int maxLines = std::max(1, (bottom - top - 2 * ALT_PADDING - lineHeight - 8) / lineHeight);
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, comic.alt.c_str(), width - 2 * ALT_PADDING, maxLines);
  const int boxH = 2 * ALT_PADDING + lineHeight + 8 + static_cast<int>(lines.size()) * lineHeight;
  const int y = bottom - boxH;
  renderer.fillRoundedRect(x - 3, y - 3, width + 6, boxH + 6, 12, Color::White);
  renderer.drawRoundedRect(x, y, width, boxH, 3, 10, true);
  int ty = y + ALT_PADDING;
  renderer.drawText(UI_10_FONT_ID, x + ALT_PADDING, ty, tr(STR_XKCD_ALT), true, EpdFontFamily::BOLD);
  ty += lineHeight + 8;
  for (const auto& line : lines) {
    renderer.drawText(UI_10_FONT_ID, x + ALT_PADDING, ty, line.c_str());
    ty += lineHeight;
  }
}

void XkcdActivity::drawMessage() const {
  const int mid = renderer.getScreenHeight() / 2;
  renderer.drawText(UI_12_FONT_ID, SIDE + 16, 6, "xkcd", true, EpdFontFamily::BOLD);
  if (checking) return;
  if (netError != NetError::None) {
    char buffer[48];
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, statusText(buffer, sizeof(buffer)), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 6, tr(STR_XKCD_RETRY_HINT));
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_XKCD_EMPTY), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 6, tr(STR_XKCD_EMPTY_HINT));
  }
}

void XkcdActivity::drawHints() const {
  if (!haveComic) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const bool newest = atNewest();
  const char* next = newest ? tr(STR_XKCD_RANDOM) : tr(STR_XKCD_NEXT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showAlt ? tr(STR_XKCD_COMIC) : tr(STR_XKCD_ALT),
                                            comic.num > 1 ? tr(STR_XKCD_PREV) : "", next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // The side tabs have no free edge once the layout is turned.
  if (!landscape) {
    GUI.drawSideButtonHints(renderer, tr(STR_XKCD_RANDOM), newest ? tr(STR_XKCD_CHECK_NEW) : tr(STR_XKCD_LATEST));
  }
}
