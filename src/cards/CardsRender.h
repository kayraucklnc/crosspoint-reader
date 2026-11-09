#pragma once

#include <string>

#include "CardStore.h"

class GfxRenderer;

// Drawing for the Cards screen. Every function draws into the frame buffer
// only; the caller clears the screen first and displays the result.
namespace cards::render {

// Where the page sits among the cards, for the status band.
struct PageInfo {
  int index = 0;         // 0-based card number
  int count = 0;         // saved cards
  bool hotspot = false;  // the editing network is up
};

// A QR code as large as fits a `maxSide` square centred on `centerX`, top at
// `y`, in whole-pixel modules. Returns the drawn side, 0 when the text is too
// long for any QR version.
int drawQr(const GfxRenderer& renderer, const std::string& text, int centerX, int y, int maxSide);

// A full-screen card plus the status band (lock and page count).
void drawCard(const GfxRenderer& renderer, const Card& card, const PageInfo& page);

// The last page: QR codes to join the hotspot and open the editor.
// `editorUrl` is empty while the hotspot is starting or after it failed.
void drawEditorPage(const GfxRenderer& renderer, const PageInfo& page, const std::string& editorUrl, bool failed);

}  // namespace cards::render
