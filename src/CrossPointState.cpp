#include "CrossPointState.h"

namespace {

void getLCG(uint32_t packedState, uint32_t packedCount, uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
            uint16_t& indexesTotal) {
  indexesLeft = packedCount >> 16;
  indexesTotal = packedCount & 0xffff;
  seed = packedState >> 16;
  state = packedState & 0xffff;
}

void setLCG(uint32_t& packedState, uint32_t& packedCount, uint32_t seedVal, uint32_t stateVal, uint16_t indexesLeftVal,
            uint16_t indexesTotalVal) {
  packedCount = (static_cast<uint32_t>(indexesLeftVal) << 16) | indexesTotalVal;
  packedState = (seedVal << 16) | (stateVal & 0xffff);
}

}  // namespace

void CrossPointState::getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                  uint16_t& indexesTotal) const {
  getLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft,
                                         uint16_t& indexesTotal) const {
  getLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepLcgState, sleepLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal) {
  setLCG(sleepOverlayLcgState, sleepOverlayLcgCount, seed, state, indexesLeft, indexesTotal);
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["sleepLcgCount"] = sleepLcgCount;
  doc["sleepLcgState"] = sleepLcgState;
  doc["sleepOverlayLcgCount"] = sleepOverlayLcgCount;
  doc["sleepOverlayLcgState"] = sleepOverlayLcgState;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["lastSleepApp"] = lastSleepApp;
  doc["showBootScreen"] = showBootScreen;
  doc["lastSleepRoute"] = lastSleepRoute;
  doc["autoRefreshArmed"] = autoRefreshArmed;
  doc["autoRefreshWakeAt"] = autoRefreshWakeAt;
  doc["autoRefreshFailures"] = autoRefreshFailures;
  doc["autoRefreshStale"] = autoRefreshStale;
  doc["autoRefreshLowBattery"] = autoRefreshLowBattery;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";

  sleepLcgCount = doc["sleepLcgCount"] | static_cast<uint32_t>(0);
  sleepLcgState = doc["sleepLcgState"] | static_cast<uint32_t>(0);

  sleepOverlayLcgCount = doc["sleepOverlayLcgCount"] | static_cast<uint32_t>(0);
  sleepOverlayLcgState = doc["sleepOverlayLcgState"] | static_cast<uint32_t>(0);

  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  lastSleepApp = doc["lastSleepApp"] | static_cast<uint8_t>(SLEEP_APP_NONE);
  if (lastSleepApp > SLEEP_APP_TRAINS) lastSleepApp = SLEEP_APP_NONE;
  showBootScreen = doc["showBootScreen"] | true;
  lastSleepRoute = doc["lastSleepRoute"] | static_cast<uint16_t>(0xFFFF);
  autoRefreshArmed = doc["autoRefreshArmed"] | false;
  autoRefreshWakeAt = doc["autoRefreshWakeAt"] | static_cast<uint32_t>(0);
  autoRefreshFailures = doc["autoRefreshFailures"] | static_cast<uint8_t>(0);
  autoRefreshStale = doc["autoRefreshStale"] | false;
  autoRefreshLowBattery = doc["autoRefreshLowBattery"] | false;
  return true;
}
