#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <I18n.h>

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
  if (loading) {
    // Over the frame already on the panel: one quick refresh.
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
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

void XkcdActivity::drawComic() {
  const Rect area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = area.x + SIDE;
  const int right = area.x + area.width - SIDE;

  // Header: number and title on the left, date on the right.
  char date[16] = "";
  if (comic.year > 0) snprintf(date, sizeof(date), "%04u-%02u-%02u", comic.year, comic.month, comic.day);
  char number[12];
  snprintf(number, sizeof(number), "#%u", comic.num);
  const int dateWidth = date[0] ? renderer.getTextWidth(UI_10_FONT_ID, date) : 0;
  const int numberWidth = renderer.getTextWidth(UI_12_FONT_ID, number, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, left, 6, number, true, EpdFontFamily::BOLD);
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, comic.title.c_str(),
                                                   right - left - numberWidth - dateWidth - 28, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, left + numberWidth + 10, 6, title.c_str());
  if (date[0]) renderer.drawText(UI_10_FONT_ID, right - dateWidth, 10, offline ? tr(STR_XKCD_OFFLINE_SHORT) : date);
  renderer.fillRect(left, HEADER_HEIGHT - 2, right - left, 1, true);

  const int top = HEADER_HEIGHT + 4;
  const int bottom = area.y + area.height - 4;
  if (comic.imagePath.empty() || comic.width <= 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, top + 20, tr(STR_XKCD_NO_IMAGE));
    drawAltText(top + 60, bottom);
    return;
  }

  const int boxW = right - left;
  const int boxH = bottom - top;
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
  if (!drawn) renderer.drawCenteredText(UI_10_FONT_ID, top + 20, tr(STR_XKCD_NO_IMAGE));
  if (showAlt) drawAltText(top, bottom);
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
  if (failed) {
    renderer.drawCenteredText(UI_12_FONT_ID, mid - 30, tr(STR_XKCD_OFFLINE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, mid + 6, tr(STR_XKCD_RETRY_HINT));
  }
}

void XkcdActivity::drawHints() const {
  if (!haveComic) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), failed ? tr(STR_RETRY) : "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const char* prev = comic.num > 1 ? tr(STR_XKCD_PREV) : "";
  const char* next = latest == 0 || comic.num < latest ? tr(STR_XKCD_NEXT) : tr(STR_XKCD_RANDOM);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showAlt ? tr(STR_XKCD_COMIC) : tr(STR_XKCD_ALT), prev, next);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // The side tabs have no free edge once the layout is turned.
  if (!landscape) GUI.drawSideButtonHints(renderer, tr(STR_XKCD_RANDOM), tr(STR_XKCD_LATEST));
}
