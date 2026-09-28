#include "IncomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "IncomeChart.h"
#include "IncomeGraphics.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "autorefresh/AutoRefreshUi.h"
#include "components/SleepSnapshot.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/IncomeCalendar.h"
#include "network/WifiAutoConnect.h"

namespace {
constexpr int MARGIN = 28;
constexpr unsigned long HOLD_MS = 700;
// Closed months are fetched only once slides have settled this long.
constexpr unsigned long FILL_DELAY_MS = 900;
// Fast refreshes ghost; every so many a half refresh cleans the panel.
constexpr int FAST_FRAMES_PER_HALF = 12;
// A backfill never runs longer than a year of months plus a few.
constexpr int MAX_BACKFILL_MONTHS = 14;
// A timer wake has a 45 s budget: it fills a few closed months per wake, the rest on later wakes.
constexpr int BACKGROUND_BACKFILL_MONTHS = 3;

// Hero label for the bucket holding today.
const char* presentLabel(const IncomeRange range) {
  switch (range) {
    case IncomeRange::MONTH_WEEKS:
      return tr(STR_INCOME_THIS_WEEK);
    case IncomeRange::YEAR_MONTHS:
      return tr(STR_INCOME_THIS_MONTH);
    default:
      return tr(STR_INCOME_TODAY);
  }
}

// Label of the period total while the view is at the present.
const char* totalLabel(const IncomeRange range) {
  switch (range) {
    case IncomeRange::LAST_7_DAYS:
      return tr(STR_INCOME_LAST_7_DAYS);
    case IncomeRange::MONTH_WEEKS:
      return tr(STR_INCOME_THIS_MONTH);
    case IncomeRange::YEAR_MONTHS:
      return tr(STR_INCOME_THIS_YEAR);
    default:
      return tr(STR_INCOME_LAST_5_DAYS);
  }
}

bool viewComplete(const IncomeView& view) {
  for (int i = 0; i < view.count; i++) {
    if (view.state[i] == IncomeView::State::MISSING) return false;
  }
  return true;
}

const char* resultMessage(const StripeClient::Result result) {
  switch (result) {
    case StripeClient::Result::NO_TIME:
      return tr(STR_INCOME_ERR_TIME);
    case StripeClient::Result::LOW_MEMORY:
      return tr(STR_INCOME_ERR_MEMORY);
    case StripeClient::Result::INVALID_KEY:
      return tr(STR_INCOME_ERR_INVALID_KEY);
    case StripeClient::Result::FORBIDDEN:
      return tr(STR_INCOME_ERR_FORBIDDEN);
    default:
      return tr(STR_INCOME_ERR_STRIPE);
  }
}
}  // namespace

void IncomeActivity::onEnter() {
  Activity::onEnter();
  // Every visit starts at the present period.
  offset = 0;
  // Show the last cached numbers immediately; the refresh redraws when it lands.
  hasSummary = StripeClient::loadCache(amountMode(), income);
  savedOrientation = renderer.getOrientation();
  renderer.setOrientation(SETTINGS.incomeLayout == 0 ? GfxRenderer::Orientation::LandscapeCounterClockwise
                                                     : GfxRenderer::Orientation::Portrait);
  if (background == BackgroundMode::RenderOnly) {
    state = State::READY;
    finishBackground(hasSummary);
    return;
  }
  startFetch(true);
}

void IncomeActivity::finishBackground(const bool succeeded) {
  if (background == BackgroundMode::Off || backgroundState != BackgroundRefresh::Pending) return;
  backgroundState = succeeded ? BackgroundRefresh::Succeeded : BackgroundRefresh::Failed;
}

void IncomeActivity::leaveBackgroundRefresh() {
  background = BackgroundMode::Off;
  requestUpdate();
}

StripeClient::Amount IncomeActivity::amountMode() {
  return SETTINGS.incomeAmount == 1 ? StripeClient::Amount::NET : StripeClient::Amount::GROSS;
}

IncomeRange IncomeActivity::range() {
  return SETTINGS.incomeRange < static_cast<uint8_t>(IncomeRange::COUNT)
             ? static_cast<IncomeRange>(SETTINGS.incomeRange)
             : IncomeRange::LAST_5_DAYS;
}

void IncomeActivity::onExit() {
  Activity::onExit();
  rangePopup.dismiss();
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

void IncomeActivity::startFetch(const bool includeCurrent) {
  std::string key;
  if (!StripeClient::loadKey(key)) {
    state = State::NO_KEY;
    finishBackground(false);
    requestUpdate();
    return;
  }
  // Closed months hang off the last fetch's day, so a first fetch always includes this month.
  refreshCurrent = includeCurrent || !hasSummary;
  backfillDone = 0;
  backfillTotal = 0;
  lastBackfillKey = 0;
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
      if (background != BackgroundMode::Off) {
        // Nobody is looking: keep the cached numbers and let the next wake retry.
        state = State::ERROR;
        errorMessage = tr(STR_INCOME_ERR_WIFI_JOIN);
        finishBackground(false);
        return;
      }
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
    finishBackground(false);
    requestUpdate();
    return;
  }

  if (refreshCurrent) {
    IncomeSummary fresh;
    const StripeClient::Result result = StripeClient::fetchIncome(amountMode(), fresh);
    if (result != StripeClient::Result::OK) {
      finishFetch(result);
      return;
    }
    RenderLock lock(*this);
    hasSummary = StripeClient::loadCache(amountMode(), income);
    if (!hasSummary) {
      // The SD write failed: show the fresh window; this month's closed days stay unknown.
      income = IncomeCache{};
      income.lastSummary = fresh;
      income.hasSummary = true;
      struct tm day;
      income_calendar::calendarOf(fresh.fetchedAt, day);
      income.monthKey = income_calendar::monthKey(day.tm_year + 1900, day.tm_mon + 1);
      hasSummary = true;
    }
  }
  // No frame here: the backfill step draws either its progress or the finished view.
  state = State::BACKFILL;
}

void IncomeActivity::runBackfillStep() {
  // Paused while the range picker is open or slides are still coming in.
  if (rangePopup.isActive() || millis() - lastViewChangeAt < FILL_DELAY_MS) return;

  int remaining = 0;
  int key = 0;
  {
    RenderLock lock(*this);
    rebuildView();
    remaining = view.missingCount;
    // Newest first: the months nearest the present fill in first.
    if (remaining > 0) key = view.missing[remaining - 1];
  }
  if (remaining == 0 || (background != BackgroundMode::Off && backfillDone >= BACKGROUND_BACKFILL_MONTHS)) {
    finishFetch(StripeClient::Result::OK);
    return;
  }
  if (key == lastBackfillKey || backfillDone >= MAX_BACKFILL_MONTHS) {
    // Fetched but still missing (the SD write failed), or a runaway: stop rather than loop.
    LOG_ERR("INCOME", "Backfill stopped at %06d after %d month(s)", key, backfillDone);
    finishFetch(StripeClient::Result::HTTP_ERROR);
    return;
  }
  backfillTotal = backfillDone + remaining;
  requestUpdateAndWait();  // "Fetching... (month k/N)" before blocking on the network

  auto month = makeUniqueNoThrow<IncomeMonth>();
  if (!month) {
    LOG_ERR("INCOME", "OOM: month buffer");
    finishFetch(StripeClient::Result::LOW_MEMORY);
    return;
  }
  const StripeClient::Result result = StripeClient::fetchMonth(amountMode(), key / 100, key % 100, *month);
  LOG_INF("INCOME", "Backfill %06d: %d request(s)", key, StripeClient::lastRequestCount());
  if (result != StripeClient::Result::OK) {
    // Unattended, the present period has already refreshed; a later wake retries the month.
    finishFetch(background != BackgroundMode::Off ? StripeClient::Result::OK : result);
    return;
  }
  lastBackfillKey = key;
  backfillDone++;
  requestUpdate();
}

void IncomeActivity::finishFetch(const StripeClient::Result result) {
  if (wifiStartedHere) stopWifi();
  if (result == StripeClient::Result::OK) {
    state = State::READY;
  } else if (result == StripeClient::Result::NO_KEY) {
    state = State::NO_KEY;
  } else {
    state = State::ERROR;
    errorMessage = resultMessage(result);
  }
  finishBackground(result == StripeClient::Result::OK);
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
  if (background != BackgroundMode::Off) {
    // Unattended: input wakes the device (main.cpp); closed months still fill in.
    if (state == State::BACKFILL) runBackfillStep();
    return;
  }

  if (rangePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (state == State::NO_KEY) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startFetch(true);
    return;
  }
  if (handleSideButtons()) return;
  // On release, so the picker does not also see this press as a step.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    openAutoRefreshPicker();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    toggleAmount();
    return;
  }

  if (state == State::BACKFILL) {
    runBackfillStep();
  } else if (fillPending && millis() - lastViewChangeAt >= FILL_DELAY_MS) {
    checkFill();
  }
}

bool IncomeActivity::handleSideButtons() {
  using Button = MappedInputManager::Button;
  // A hold fires at the threshold; its release is swallowed globally.
  if (mappedInput.wasLongPressed(Button::Up, HOLD_MS) || mappedInput.wasLongPressed(Button::Down, HOLD_MS)) {
    openRangePicker();
    return true;
  }
  if (mappedInput.wasReleased(Button::Up)) {
    slide(1);
    return true;
  }
  if (mappedInput.wasReleased(Button::Down)) {
    slide(-1);
    return true;
  }
  return false;
}

void IncomeActivity::slide(const int periods) {
  if (!hasSummary) return;
  const int next = std::clamp(offset + periods, 0, maxIncomeOffset(range()));
  if (next == offset) return;
  offset = next;
  lastViewChangeAt = millis();
  fillPending = true;
  requestUpdate();
}

void IncomeActivity::checkFill() {
  fillPending = false;
  if (state != State::READY && state != State::ERROR) return;
  bool missing = false;
  {
    RenderLock lock(*this);
    rebuildView();
    missing = view.missingCount > 0;
  }
  // Cached periods never touch the network; only closed months not on the card do.
  if (missing) startFetch(false);
}

void IncomeActivity::openRangePicker() {
  static constexpr StrId OPTIONS[] = {StrId::STR_INCOME_RANGE_5_DAYS, StrId::STR_INCOME_RANGE_7_DAYS,
                                      StrId::STR_INCOME_RANGE_WEEKS, StrId::STR_INCOME_RANGE_MONTHS};
  rangePopup.show(StrId::STR_INCOME_RANGE, OPTIONS, static_cast<int>(IncomeRange::COUNT), static_cast<int>(range()),
                  [this](const int choice) {
                    if (choice != static_cast<int>(range())) {
                      SETTINGS.incomeRange = static_cast<uint8_t>(choice);
                      SETTINGS.saveToFile();
                    }
                    offset = 0;
                    lastViewChangeAt = millis();
                    fillPending = true;
                  });
  requestUpdate();
}

void IncomeActivity::openAutoRefreshPicker() {
  auto picker = auto_refresh_ui::makeIntervalPicker(renderer, mappedInput, CrossPointState::SLEEP_APP_INCOME);
  if (!picker) return;
  startActivityForResult(std::move(picker), [](const ActivityResult& result) {
    if (result.isCancelled) return;
    SETTINGS.incomeAutoRefresh = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
    SETTINGS.saveToFile();
  });
}

void IncomeActivity::toggleAmount() {
  SETTINGS.incomeAmount = SETTINGS.incomeAmount == 1 ? 0 : 1;
  SETTINGS.saveToFile();
  {
    // The other mode's cached numbers show at once; the refresh replaces them.
    RenderLock lock(*this);
    hasSummary = StripeClient::loadCache(amountMode(), income);
  }
  startFetch(true);
}

void IncomeActivity::loadMonth(void* self, const int year, const int month, IncomeMonth& out) {
  static_cast<const IncomeActivity*>(self)->composeMonth(year, month, out);
}

void IncomeActivity::composeMonth(const int year, const int month, IncomeMonth& out) const {
  const int key = income_calendar::monthKey(year, month);
  if (key == income.monthKey) {
    for (int day = 1; day <= IncomeMonth::MAX_DAYS; day++) {
      if (income.isKnown(day)) out.set(day, income.closedCents[day], income.closedNonUsd[day]);
    }
  } else if (key < income.monthKey) {
    income_history::load(key, amountMode() == StripeClient::Amount::NET, StripeClient::monthMode(amountMode()), out);
  }
  // The five-day window fills whatever days the month does not have yet.
  const IncomeSummary& summary = income.lastSummary;
  for (int i = 0; i < IncomeSummary::DAY_COUNT; i++) {
    struct tm day;
    income_calendar::calendarOf(summary.dayStart[i] + 3600, day);
    if (day.tm_year + 1900 != year || day.tm_mon + 1 != month || out.isKnown(day.tm_mday)) continue;
    out.set(day.tm_mday, summary.cents[i], summary.nonUsd[i]);
  }
}

void IncomeActivity::rebuildView() {
  if (!hasSummary) {
    view = IncomeView{};
    return;
  }
  // One read: the loop may slide while this frame draws, and every part of it must agree.
  viewOffset = offset;
  buildIncomeView(range(), viewOffset, income.lastSummary.fetchedAt, &IncomeActivity::loadMonth, this, scratch, view);
}

void IncomeActivity::render(RenderLock&&) {
  // A queued live frame must not replace the sleep snapshot, and a timer-wake
  // refresh draws nothing until that snapshot.
  if (frozen || background != BackgroundMode::Off) return;
  rebuildView();
  renderer.clearScreen();
  if (state == State::NO_KEY) {
    renderMessage(tr(STR_INCOME_NO_KEY), tr(STR_INCOME_NO_KEY_HINT));
  } else {
    renderDashboard(renderer.getScreenWidth(), renderer.getScreenHeight());
  }
  // Status steps only change a few pixels; after the first frame fast refreshes
  // avoid the full-screen flash, with a periodic half refresh against ghosting.
  const bool half = !firstRenderDone || framesSinceHalfRefresh >= FAST_FRAMES_PER_HALF;
  framesSinceHalfRefresh = half ? 0 : framesSinceHalfRefresh + 1;
  firstRenderDone = true;
  if (rangePopup.isActive()) {
    rangePopup.render(renderer);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(half ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    return;
  }
  const bool controls = state != State::NO_KEY;
  char autoHint[24];
  auto_refresh_ui::formatHint(CrossPointState::SLEEP_APP_INCOME, autoHint, sizeof(autoHint));
  // Left/Right are actions here, not directions: label them where they physically are.
  const auto labels = mappedInput.mapFrontLabels(
      tr(STR_BACK), controls ? tr(STR_INCOME_REFRESH) : "", controls ? autoHint : "",
      controls ? (SETTINGS.incomeAmount == 1 ? tr(STR_INCOME_NET) : tr(STR_INCOME_GROSS)) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(half ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
}

void IncomeActivity::renderSleepSnapshot() {
  frozen = true;
  rangePopup.dismiss();
  // The snapshot, like the timer-wake refreshes that redraw it, shows the present period.
  offset = 0;
  rebuildView();
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
  const bool present = viewOffset == 0;
  const bool complete = viewComplete(view);

  // Present: the bucket holding today. A past period: its total, under the period's name.
  char heroLabel[24];
  if (present) {
    snprintf(heroLabel, sizeof(heroLabel), "%s", presentLabel(range()));
  } else {
    snprintf(heroLabel, sizeof(heroLabel), "%s", view.period);
    for (char* p = heroLabel; *p; p++) *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
  }
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, heroLabel, TRACKING);

  char hero[24];
  const bool heroKnown = present ? view.current >= 0 && view.state[view.current] == IncomeView::State::DATA : complete;
  if (hasSummary && heroKnown) {
    income_gfx::formatUsd(present ? view.cents[view.current] : view.total, hero, sizeof(hero));
  } else {
    snprintf(hero, sizeof(hero), "$--");  // placeholder until the data lands
  }
  const int heroY = y + 34;
  // Landscape leaves room for the total block on the right; the 5x7 font is 7 blocks tall.
  const int maxAmountWidth = landscape ? width * 3 / 5 : width;
  int pixelSize = 14;
  while (pixelSize > 4 && income_gfx::pixelTextWidth(hero, pixelSize) > maxAmountWidth) pixelSize--;
  income_gfx::drawPixelText(renderer, x, heroY, hero, pixelSize);
  const int heroBottom = heroY + pixelSize * 7 + pixelSize / 2;

  // Landscape: the period total right-aligned beside the hero. Portrait: stacked below it.
  const int rightEdge = x + width;
  const int blockY = landscape ? y : heroBottom + 12;
  const auto alignX = [&](const int w) { return landscape ? rightEdge - w : x; };
  int statusY = blockY;
  if (present) {
    char total[24];
    if (hasSummary && complete) {
      income_gfx::formatUsd(view.total, total, sizeof(total));
    } else {
      snprintf(total, sizeof(total), "--");
    }
    const char* label = totalLabel(range());
    const int labelWidth = income_gfx::trackedLabelWidth(renderer, UI_10_FONT_ID, label, TRACKING);
    const int totalWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, total, EpdFontFamily::BOLD);
    income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, alignX(labelWidth), blockY, label, TRACKING);
    renderer.drawText(NOTOSANS_18_FONT_ID, alignX(totalWidth), blockY + 26, total, true, EpdFontFamily::BOLD);
    statusY = blockY + 26 + renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 6;
  }
  // Asleep, the badge may use everything right of the actual amount: its
  // auto-refresh notes then stay on one line.
  const int heroWidth = frozen ? income_gfx::pixelTextWidth(hero, pixelSize) : maxAmountWidth;
  const int statusWidth = landscape ? width - heroWidth - 24 : width;
  const int statusBottom = renderStatus(landscape ? rightEdge - statusWidth : x, statusY, statusWidth, landscape);
  return std::max(heroBottom, statusBottom);
}

int IncomeActivity::renderStatus(const int x, const int y, const int width, const bool alignRight) const {
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const auto drawLine = [&](const char* text, const int lineY) {
    const int w = renderer.getTextWidth(SMALL_FONT_ID, text);
    renderer.drawText(SMALL_FONT_ID, alignRight ? x + width - w : x, lineY, text);
  };
  const IncomeSummary& summary = income.lastSummary;

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
  if (state == State::BACKFILL) {
    char progress[48];
    if (backfillTotal > 0) {
      snprintf(progress, sizeof(progress), tr(STR_INCOME_FETCHING_MONTH), backfillDone + 1, backfillTotal);
    } else {
      snprintf(progress, sizeof(progress), "%s", tr(STR_INCOME_FETCHING));
    }
    return income_gfx::drawStepStatus(renderer, x, y, width, alignRight, 1, progress);
  }

  if (state == State::ERROR) {
    char lines[2][96];
    const int count = income_gfx::wrapText(renderer, SMALL_FONT_ID, errorMessage, width, lines, 2);
    for (int i = 0; i < count; i++) drawLine(lines[i], y + i * lineHeight);
    return y + count * lineHeight;
  }

  if (!hasSummary) return y;
  char clock[8];
  char updated[64];
  struct tm local;
  localtime_r(&summary.fetchedAt, &local);
  strftime(clock, sizeof(clock), "%H:%M", &local);
  // At the present the period's dates lead the line; a past period already has them as its label.
  if (viewOffset == 0) {
    snprintf(updated, sizeof(updated), "%s \xC2\xB7 %s %s", view.period, tr(STR_INCOME_UPDATED), clock);
  } else {
    snprintf(updated, sizeof(updated), "%s %s", tr(STR_INCOME_UPDATED), clock);
  }
  drawLine(updated, y);
  if (view.nonUsd <= 0) return y + lineHeight;
  // The key could not read balance transactions, so charges in other currencies are missing.
  char note[96];
  snprintf(note, sizeof(note), tr(STR_INCOME_NON_USD_SKIPPED), view.nonUsd);
  char lines[2][96];
  const int count = income_gfx::wrapText(renderer, SMALL_FONT_ID, note, width, lines, 2);
  for (int i = 0; i < count; i++) drawLine(lines[i], y + (i + 1) * lineHeight);
  return y + (count + 1) * lineHeight;
}

void IncomeActivity::renderChart(const int x, const int y, const int width, const int height) const {
  if (!hasSummary) {
    // Skeleton: a baseline and a faint flat line where the chart will appear.
    const int baseline = y + height - 30;
    renderer.fillRect(x + 32, baseline, width - 64, 2, true);
    income_gfx::fillDither(renderer, x + 32, baseline - height / 3, width - 64, 2, 4);
    return;
  }
  drawIncomeChart(renderer, view, x, y, width, height);
}
