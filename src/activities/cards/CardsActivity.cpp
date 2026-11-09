#include "CardsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "cards/CardStore.h"
#include "cards/CardsRender.h"
#include "components/UITheme.h"

void CardsActivity::onEnter() {
  Activity::onEnter();
  CARD_STORE.ensureLoaded();
  CARD_STORE.setScreenSize(renderer.getScreenWidth(), renderer.getScreenHeight());
  CARD_STORE.takeShowRequest();  // drop a request left from an earlier session
  seenRevision = CARD_STORE.revision();
  lastPageCount = pageCount();
  page = std::clamp(page, 0, lastPageCount - 1);
  lastDrawnPage = -1;
  requestUpdate();
}

void CardsActivity::onExit() {
  Activity::onExit();
  const bool usedWifi = hotspot.wasUsed();
  hotspot.stop();
  // WiFi fragments the heap; like File Transfer, leave through a quick restart
  // (skipped when the device is going to sleep instead).
  if (usedWifi) silentRestart();
}

int CardsActivity::pageCount() const { return static_cast<int>(CARD_STORE.count()) + 1; }

bool CardsActivity::onEditorPage() const { return page >= pageCount() - 1; }

void CardsActivity::turn(const int direction) {
  const int count = pageCount();
  page = (page + count + direction) % count;
  requestUpdate();
}

void CardsActivity::startHotspotIfNeeded() {
  if (!onEditorPage() || hotspot.running() || hotspotFailed) return;
  requestUpdateAndWait();  // show "Starting hotspot…" before the blocking start
  hotspotFailed = !hotspot.start(renderer);
  requestUpdate();
}

void CardsActivity::loop() {
  if (hotspot.running()) hotspot.service();

  // Edits from the phone: redraw, keeping the page on a card that still exists.
  const uint32_t revision = CARD_STORE.revision();
  if (revision != seenRevision) {
    const bool wasOnEditor = page >= lastPageCount - 1;
    seenRevision = revision;
    lastPageCount = pageCount();
    page = wasOnEditor ? lastPageCount - 1 : std::min(page, lastPageCount - 1);
    lastDrawnPage = -1;
    requestUpdate();
  }
  const int shown = CARD_STORE.takeShowRequest();
  if (shown >= 0 && shown < pageCount() - 1) {
    page = shown;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
    turn(+1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
    turn(-1);
  } else if (onEditorPage() && hotspotFailed && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    hotspotFailed = false;
  }
  startHotspotIfNeeded();
}

void CardsActivity::drawPage() {
  renderer.clearScreen();
  cards::render::PageInfo info;
  info.index = page;
  info.count = pageCount() - 1;
  info.hotspot = hotspot.running();
  cards::Card card;
  if (!onEditorPage() && CARD_STORE.cardAt(page, card)) {
    cards::render::drawCard(renderer, card, info);
    return;
  }
  const std::string url = hotspot.running() ? hotspot.editorUrl() : std::string();
  cards::render::drawEditorPage(renderer, info, url, hotspotFailed);
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), hotspotFailed ? tr(STR_RETRY) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CardsActivity::render(RenderLock&&) {
  drawPage();
  // A new page gets a clean single-pass refresh; updates to the same page are fast.
  const bool newPage = page != lastDrawnPage;
  lastDrawnPage = page;
  renderer.displayBuffer(newPage ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void CardsActivity::renderSleepSnapshot() {
  hotspot.stop();  // the hotspot mark should not stay on the sleeping card
  drawPage();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
