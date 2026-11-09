#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "cards/CardsHotspot.h"

// Pocket Cards: one saved card per page, full screen and free of chrome apart
// from a small lock and page count. Left/Right (and the side buttons) flip
// pages, Back leaves. The page after the last card starts the reader's own
// Wi-Fi and shows QR codes to join it and open the editor on a phone; once
// started, the network stays up while Cards is open so the phone can keep
// editing and pick the card on screen.
//
// While a card is shown it stays on the panel through sleep (Keep App on
// Sleep) and the device wakes back into it.
class CardsActivity final : public Activity {
 public:
  explicit CardsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int startIndex = 0)
      : Activity("Cards", renderer, mappedInput), page(startIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool skipLoopDelay() override { return hotspot.running(); }
  // Keep serving the phone while its editor page is open; card pages may sleep.
  bool preventAutoSleep() override { return onEditorPage() && hotspot.running(); }
  bool hasSleepSnapshot() const override { return !onEditorPage(); }
  void renderSleepSnapshot() override;
  uint16_t sleepSnapshotRoute() const override { return static_cast<uint16_t>(page); }

 private:
  cards::CardsHotspot hotspot;
  int page = 0;
  int lastDrawnPage = -1;
  uint32_t seenRevision = 0;
  int lastPageCount = 1;  // pages when the store was last seen, to tell the editor page
  bool hotspotFailed = false;

  int pageCount() const;
  bool onEditorPage() const;
  void turn(int direction);
  void startHotspotIfNeeded();
  void drawPage();
};
