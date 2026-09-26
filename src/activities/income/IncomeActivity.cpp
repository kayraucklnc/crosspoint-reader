#include "IncomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "IncomeChart.h"
#include "IncomeGraphics.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "WifiCredentialStore.h"
#include "network/WifiAutoConnect.h"
#include "components/SleepSnapshot.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 28;
}  // namespace

void IncomeActivity::onEnter() {
  Activity::onEnter();
  // Show the last cached numbers immediately; the refresh redraws when it lands.
  hasSummary = StripeClient::loadCachedSummary(amountMode(), summary);
  savedOrientation = renderer.getOrientation();
  renderer.setOrientation(SETTINGS.incomeLayout == 0 ? GfxRenderer::Orientation::LandscapeCounterClockwise
                                                     : GfxRenderer::Orientation::Portrait);
  startFetch();
}

StripeClient::Amount IncomeActivity::amountMode() {
  return SETTINGS.incomeAmount == 1 ? StripeClient::Amount::NET : StripeClient::Amount::GROSS;
}

void IncomeActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(savedOrientation);
  // Like other WiFi activities, reboot back home to reclaim heap the WiFi/TLS stack fragmented.
  if (wifiStartedHere) {
    stopWifi();
    silentRestart();
  }
}

void IncomeActivity::stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  delay(30);
  WiFi.mode(WIFI_OFF);
}

void IncomeActivity::startFetch() {
  std::string key;
  if (!StripeClient::loadKey(key)) {
    state = State::NO_KEY;
    requestUpdate();
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    state = State::LOADING;
  } else {
    state = State::CONNECTING;
    wifiStartedHere = true;
  }
  requestUpdate();
}

void IncomeActivity::runConnect() {
  {
    // Saved credentials load lazily; SD shares the SPI bus with the display.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  switch (wifi_auto_connect::connect()) {
    case wifi_auto_connect::Result::CONNECTED:
      state = State::LOADING;
      requestUpdate();
      return;
    case wifi_auto_connect::Result::NO_SAVED_NETWORKS:
    case wifi_auto_connect::Result::FAILED:
      // Only now show the picker, without auto-connect (that just failed).
      state = State::PICKING;
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                             [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
      return;
  }
}

void IncomeActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    state = State::ERROR;
    errorMessage = tr(STR_INCOME_ERR_WIFI_JOIN);
    requestUpdate();
    return;
  }
  state = State::LOADING;
  requestUpdate();
}

void IncomeActivity::runFetch() {
  if (WiFi.status() != WL_CONNECTED) {
    state = State::ERROR;
    errorMessage = tr(STR_INCOME_ERR_WIFI);
    requestUpdate();
    return;
  }

  IncomeSummary fresh;
  const StripeClient::Result result = StripeClient::fetchIncome(amountMode(), fresh);
  if (wifiStartedHere) stopWifi();

  switch (result) {
    case StripeClient::Result::OK:
      summary = fresh;
      hasSummary = true;
      state = State::READY;
      break;
    case StripeClient::Result::NO_KEY:
      state = State::NO_KEY;
      break;
    case StripeClient::Result::NO_TIME:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_TIME);
      break;
    case StripeClient::Result::LOW_MEMORY:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_MEMORY);
      break;
    case StripeClient::Result::INVALID_KEY:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_INVALID_KEY);
      break;
    case StripeClient::Result::FORBIDDEN:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_FORBIDDEN);
      break;
    case StripeClient::Result::HTTP_ERROR:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_STRIPE);
      break;
  }
  requestUpdate();
}

void IncomeActivity::loop() {
  // Going to sleep: no more network steps or input once the snapshot is up.
  if (frozen) return;
  // Each step shows its status before blocking on the network.
  if (state == State::CONNECTING) {
    requestUpdateAndWait();
    runConnect();
    return;
  }
  if (state == State::LOADING) {
    requestUpdateAndWait();
    runFetch();
    return;
  }
  if (state == State::PICKING) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && state != State::NO_KEY) {
    startFetch();
  }
}

void IncomeActivity::render(RenderLock&&) {
  if (frozen) return;  // a queued live frame must not replace the sleep snapshot
  renderer.clearScreen();
  if (state == State::NO_KEY) {
    renderMessage(tr(STR_INCOME_NO_KEY), tr(STR_INCOME_NO_KEY_HINT));
  } else {
    renderDashboard(renderer.getScreenWidth(), renderer.getScreenHeight());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), state == State::NO_KEY ? "" : tr(STR_INCOME_REFRESH), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Status steps only change a few pixels; after the first frame a fast refresh avoids the full-screen flash.
  renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  firstRenderDone = true;
}

void IncomeActivity::renderSleepSnapshot() {
  frozen = true;
  renderer.clearScreen();
  renderDashboard(renderer.getScreenWidth(), renderer.getScreenHeight());
  sleep_snapshot::drawFrame(renderer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void IncomeActivity::renderMessage(const char* title, const char* detail) const {
  const int midY = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, title, true, EpdFontFamily::BOLD);
  if (detail[0] != '\0') renderer.drawCenteredText(UI_10_FONT_ID, midY + 12, detail);
}

void IncomeActivity::renderDashboard(const int width, const int height) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Button hints sit along the physical bottom edge: the right side in landscape.
  const bool landscape = width > height;
  const int hints = frozen ? 0 : metrics.buttonHintsHeight;  // the snapshot shows no button hints
  const int contentWidth = (landscape ? width - hints : width) - MARGIN * 2;
  const int contentBottom = (landscape ? height : height - hints) - MARGIN;

  const int heroBottom = renderHero(MARGIN, MARGIN, contentWidth, landscape);
  const int chartY = heroBottom + (landscape ? MARGIN / 2 : MARGIN * 2);
  renderChart(MARGIN, chartY, contentWidth, contentBottom - chartY);
}

int IncomeActivity::renderHero(const int x, const int y, const int width, const bool landscape) const {
  constexpr int TRACKING = 3;
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, tr(STR_INCOME_TODAY), TRACKING);

  char today[24];
  if (hasSummary) {
    income_gfx::formatUsd(summary.cents[0], today, sizeof(today));
  } else {
    snprintf(today, sizeof(today), "$--");  // placeholder until the first fetch lands
  }
  const int todayY = y + 34;
  // Landscape leaves room for the month block on the right; the 5x7 font is 7 blocks tall.
  const int maxAmountWidth = landscape ? width * 3 / 5 : width;
  int pixelSize = 14;
  while (pixelSize > 4 && income_gfx::pixelTextWidth(today, pixelSize) > maxAmountWidth) pixelSize--;
  income_gfx::drawPixelText(renderer, x, todayY, today, pixelSize);
  const int todayBottom = todayY + pixelSize * 7 + pixelSize / 2;

  char month[24];
  if (hasSummary) {
    income_gfx::formatUsd(summary.monthCents, month, sizeof(month));
  } else {
    snprintf(month, sizeof(month), "--");
  }

  // Landscape: month total right-aligned beside the hero. Portrait: stacked below it.
  const int monthLabelWidth = income_gfx::trackedLabelWidth(renderer, UI_10_FONT_ID, tr(STR_INCOME_THIS_MONTH), TRACKING);
  const int monthWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, month, EpdFontFamily::BOLD);
  const int rightEdge = x + width;
  const int blockY = landscape ? y : todayBottom + 12;
  const auto alignX = [&](const int w) { return landscape ? rightEdge - w : x; };

  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, alignX(monthLabelWidth), blockY, tr(STR_INCOME_THIS_MONTH),
                               TRACKING);
  renderer.drawText(NOTOSANS_18_FONT_ID, alignX(monthWidth), blockY + 26, month, true, EpdFontFamily::BOLD);
  const int statusY = blockY + 26 + renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 6;
  const int statusWidth = landscape ? width - maxAmountWidth - 24 : width;
  const int statusBottom = renderStatus(landscape ? rightEdge - statusWidth : x, statusY, statusWidth, landscape);
  return std::max(todayBottom, statusBottom);
}

int IncomeActivity::renderStatus(const int x, const int y, const int width, const bool alignRight) const {
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const auto drawLine = [&](const char* text, const int lineY) {
    const int w = renderer.getTextWidth(SMALL_FONT_ID, text);
    renderer.drawText(SMALL_FONT_ID, alignRight ? x + width - w : x, lineY, text);
  };

  if (frozen) {
    char clock[8];
    struct tm local;
    localtime_r(&summary.fetchedAt, &local);
    strftime(clock, sizeof(clock), "%H:%M", &local);
    return sleep_snapshot::drawBadge(renderer, x, y, width, alignRight, clock);
  }

  if (state == State::CONNECTING || state == State::LOADING || state == State::PICKING) {
    const bool fetching = state == State::LOADING;
    return income_gfx::drawStepStatus(renderer, x, y, width, alignRight, fetching ? 1 : 0,
                                      fetching ? tr(STR_INCOME_FETCHING) : tr(STR_INCOME_CONNECTING));
  }

  if (state == State::ERROR) {
    char lines[2][96];
    const int count = income_gfx::wrapText(renderer, SMALL_FONT_ID, errorMessage, width, lines, 2);
    for (int i = 0; i < count; i++) drawLine(lines[i], y + i * lineHeight);
    return y + count * lineHeight;
  }

  if (!hasSummary) return y;
  char clock[8];
  char updated[48];
  struct tm local;
  localtime_r(&summary.fetchedAt, &local);
  strftime(clock, sizeof(clock), "%H:%M", &local);
  snprintf(updated, sizeof(updated), "%s %s", tr(STR_INCOME_UPDATED), clock);
  drawLine(updated, y);
  return y + lineHeight;
}


void IncomeActivity::renderChart(const int x, const int y, const int width, const int height) const {
  if (!hasSummary) {
    // Skeleton: a baseline and a faint flat line where the chart will appear.
    const int baseline = y + height - 30;
    renderer.fillRect(x + 32, baseline, width - 64, 2, true);
    income_gfx::fillDither(renderer, x + 32, baseline - height / 3, width - 64, 2, 4);
    return;
  }
  IncomeChartData data;
  data.count = IncomeSummary::DAY_COUNT;
  for (int i = 0; i < data.count; i++) {
    const int day = data.count - 1 - i;  // oldest first
    data.cents[i] = summary.cents[day];
    data.dayStart[i] = summary.dayStart[day];
  }
  drawIncomeChart(renderer, data, x, y, width, height);
}
