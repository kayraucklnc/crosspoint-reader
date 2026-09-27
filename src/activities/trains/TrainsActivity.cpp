#include "TrainsActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/income/IncomeGraphics.h"
#include "activities/network/WifiSelectionActivity.h"
#include "autorefresh/AutoRefreshUi.h"
#include "components/SleepSnapshot.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkTime.h"
#include "network/WifiAutoConnect.h"

namespace {
constexpr int MARGIN = 26;
constexpr int TRACKING = 3;
constexpr int ROW_HEIGHT = 34;
constexpr int STRIP_HEIGHT = 44;
constexpr int REFRESH_AFTER_SECONDS = 45;  // re-showing a board younger than this skips the network
// A timer-wake refresh reloads the ~170 KB-per-line notices feed only this often.
constexpr time_t BACKGROUND_ALERTS_SECONDS = 3 * 3600;
constexpr int AUTO_CHIP_GAP = 12;
constexpr int BADGE_ROW = 40;  // extra header row when the asleep badge moves under the route
constexpr int ALERT_LINE = 22;  // line pitch of the hero's alert text

void upper(const char* in, char* out, const size_t size) {
  size_t i = 0;
  for (; in[i] && i + 1 < size; i++) out[i] = (in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i];
  out[i] = '\0';
}

// Title-case "PIOLTELLO LIMITO" -> "Pioltello Limito" for calmer rows.
void titleCase(const char* in, char* out, const size_t size) {
  size_t i = 0;
  bool wordStart = true;
  for (; in[i] && i + 1 < size; i++) {
    const char c = in[i];
    out[i] = (!wordStart && c >= 'A' && c <= 'Z') ? c + 32 : c;
    wordStart = c == ' ' || c == '-' || c == '.' || c == '\'' || c == '(';
  }
  out[i] = '\0';
}

constexpr long STALE_SECONDS = 30 * 60;  // older boards show times only, no countdown
constexpr long MAX_COUNTDOWN = 24 * 3600;
constexpr long GONE_SECONDS = 60;  // a train this far past its (delayed) time has left

long secondsUntil(const trenord::Departure& d, const time_t now) {
  const int delay = d.delayMin == trenord::DELAY_UNKNOWN ? 0 : d.delayMin;
  return static_cast<long>(d.scheduled + delay * 60 - now);
}

bool departed(const trenord::Departure& d, const time_t now) {
  return now != 0 && secondsUntil(d, now) < -GONE_SECONDS;
}

// Minutes to departure, or -1 when it can't be trusted: unknown clock, a stale
// board, or an implausible gap (a wrong clock shows up as days of "minutes").
int countdownMinutes(const trenord::Departure& d, const time_t now, const time_t fetchedAt) {
  if (now == 0 || now - fetchedAt > STALE_SECONDS || fetchedAt - now > 600) return -1;
  const long seconds = secondsUntil(d, now);
  if (seconds > MAX_COUNTDOWN || seconds < -5 * 60) return -1;
  return seconds <= 0 ? 0 : static_cast<int>((seconds + 59) / 60);
}

// A train with any alert never reads "on time": its delay, or what the alert says it is.
void delayText(const trenord::Departure& d, char* out, const size_t size) {
  using trenord::Disruption;
  if (d.cancelled()) {
    snprintf(out, size, "%s", tr(STR_TRAINS_CANCELLED));
  } else if (d.delayMin != trenord::DELAY_UNKNOWN && d.delayMin > 0) {
    snprintf(out, size, "+%d %s", d.delayMin, tr(STR_TRAINS_MIN));
  } else if (d.disruption == Disruption::PARTIAL) {
    snprintf(out, size, "%s", tr(STR_TRAINS_LIMITED));
  } else if (d.disruption == Disruption::BUS) {
    snprintf(out, size, "%s", tr(STR_TRAINS_BUS));
  } else if (d.disruption != Disruption::NONE || d.delayMin == trenord::DELAY_UNKNOWN) {
    out[0] = '\0';
  } else {
    snprintf(out, size, "%s", tr(STR_TRAINS_ON_TIME));
  }
}

// The train's own alert as one line: "! Limited: cancelled Treviglio-Pioltello Limito (Forza Maggiore)".
// Empty when the train has none.
void disruptionText(const trenord::Departure& d, char* out, const size_t size) {
  using trenord::Disruption;
  out[0] = '\0';
  if (d.disruption == Disruption::NONE) return;
  // Cancelled stretches name stations in capitals; other alerts are sentences.
  char note[sizeof(d.note)];
  if (d.disruption == Disruption::PARTIAL || d.disruption == Disruption::CANCELLED) {
    titleCase(d.note, note, sizeof(note));
  } else {
    snprintf(note, sizeof(note), "%s", d.note);
  }
  const char* sep = note[0] ? ": " : "";
  switch (d.disruption) {
    case Disruption::PARTIAL:
      snprintf(out, size, note[0] ? "! %s: %s %s" : "! %s", tr(STR_TRAINS_LIMITED), tr(STR_TRAINS_STRETCH_CANCELLED),
               note);
      break;
    case Disruption::CANCELLED:
      if (note[0]) snprintf(out, size, "! %s%s%s", tr(STR_TRAINS_CANCELLED), sep, note);
      break;
    case Disruption::BUS:
      snprintf(out, size, "! %s%s%s", tr(STR_TRAINS_BUS), sep, note);
      break;
    default:
      if (note[0]) snprintf(out, size, "! %s", note);
      break;
  }
}

// Some services (Malpensa Express) come without number or headsign.
void trainLabel(const trenord::Departure& d, char* out, const size_t size) {
  snprintf(out, size, "%s%s%s", d.category, d.number[0] ? " " : "", d.number);
}

void directionLabel(const trenord::Departure& d, char* out, const size_t size) {
  if (d.direction[0]) {
    titleCase(d.direction, out, size);
  } else if (strcmp(d.category, "MXP") == 0) {
    snprintf(out, size, "%s", tr(STR_TRAINS_MALPENSA));
  } else {
    out[0] = '\0';
  }
}

void clockText(const time_t utc, char* out, const size_t size) {
  struct tm rome;
  trenord::romeTime(utc, rome);
  snprintf(out, size, "%02d:%02d", rome.tm_hour, rome.tm_min);
}
}  // namespace

void TrainsActivity::onEnter() {
  Activity::onEnter();
  savedOrientation = renderer.getOrientation();
  renderer.setOrientation(SETTINGS.incomeLayout == 0 ? GfxRenderer::Orientation::LandscapeCounterClockwise
                                                     : GfxRenderer::Orientation::Portrait);
  cache.load();
  config.load();
  buildStrip();
  if (initialRoute != DEFAULT_SLEEP_ROUTE) {
    const int origin = initialRoute >> 8;
    const int towards = initialRoute & 0xFF;
    if (origin < trenord::STATION_COUNT && towards < trenord::STATION_COUNT) {
      showBoard(origin, towards);
      return;
    }
  }
  showDefault();
}

uint16_t TrainsActivity::sleepSnapshotRoute() const {
  if (!viewingReturn()) return DEFAULT_SLEEP_ROUTE;
  return static_cast<uint16_t>((static_cast<uint8_t>(board.origin) << 8) | static_cast<uint8_t>(board.towards));
}

void TrainsActivity::finishBackground(const bool succeeded) {
  if (background == BackgroundMode::Off || backgroundState != BackgroundRefresh::Pending) return;
  backgroundState = succeeded ? BackgroundRefresh::Succeeded : BackgroundRefresh::Failed;
}

void TrainsActivity::leaveBackgroundRefresh() {
  background = BackgroundMode::Off;
  requestUpdate();
}

void TrainsActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(savedOrientation);
  if (wifiStartedHere) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    silentRestart();  // reclaim heap the WiFi/TLS stack fragmented, like other network screens
  }
}

void TrainsActivity::buildStrip() {
  // Physical line order, so moving the cursor walks along the line. With favorites
  // set (web app), only those stations appear; the default origin always does.
  bool hasFavorites = false;
  for (const int8_t f : config.favorites) hasFavorites = hasFavorites || f >= 0;
  stripCount = 0;
  for (const int8_t station : trenord::LINE_ORDER) {
    const bool favorite =
        std::find(std::begin(config.favorites), std::end(config.favorites), station) != std::end(config.favorites);
    if (station == config.origin || !hasFavorites || favorite) strip[stripCount++] = station;
    if (station == config.origin) cursor = stripCount - 1;
  }
}

// Shows the cached board for the route at once, then refreshes it unless it is fresh.
void TrainsActivity::showBoard(const int origin, const int towards) {
  requestedOrigin = origin;
  requestedTowards = towards;
  scroll = 0;
  const trenord::Board* cached = nullptr;
  const bool isDefault = origin == config.origin && towards == config.towards;
  if (isDefault && cache.hasOutbound && cache.outbound.origin == origin && cache.outbound.towards == towards) {
    cached = &cache.outbound;
  } else if (!isDefault && cache.hasInbound && cache.inbound.origin == origin && cache.inbound.towards == towards) {
    cached = &cache.inbound;
  }
  hasBoard = cached != nullptr;
  if (cached) {
    board = *cached;
  } else {
    board = trenord::Board{};
    board.origin = static_cast<int8_t>(origin);
    board.towards = static_cast<int8_t>(towards);
  }
  const time_t now = network_time::current();
  if (cached && now != 0 && now >= cached->fetchedAt && now - cached->fetchedAt < REFRESH_AFTER_SECONDS) {
    state = State::READY;
    finishBackground(true);
    requestUpdate();
    return;
  }
  startRefresh();
}

void TrainsActivity::startRefresh() {
  if (background == BackgroundMode::RenderOnly) {
    state = State::READY;
    finishBackground(hasBoard);
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

void TrainsActivity::runConnect() {
  {
    // Saved credentials load lazily; SD shares the SPI bus with the display.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
  }
  if (wifi_auto_connect::connect() == wifi_auto_connect::Result::CONNECTED) {
    state = State::LOADING;
    requestUpdate();
    return;
  }
  if (background != BackgroundMode::Off) {
    // Nobody is looking: keep the cached board and let the next wake retry.
    state = State::ERROR;
    errorMessage = tr(STR_INCOME_ERR_WIFI_JOIN);
    finishBackground(false);
    return;
  }
  state = State::PICKING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void TrainsActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    state = State::ERROR;
    errorMessage = tr(STR_INCOME_ERR_WIFI_JOIN);
  } else {
    state = State::LOADING;
  }
  requestUpdate();
}

void TrainsActivity::runFetch() {
  // Straight into the on-screen board (already this route's): fetchBoard only writes it on success.
  const trenord::Result result = trenord::fetchBoard(requestedOrigin, requestedTowards, board);
  switch (result) {
    case trenord::Result::OK:
      hasBoard = true;
      scroll = 0;
      state = State::READY;
      if (requestedOrigin == config.origin && requestedTowards == config.towards) {
        cache.outbound = board;
        cache.hasOutbound = true;
      } else {
        cache.inbound = board;
        cache.hasInbound = true;
        cache.markRecent(requestedOrigin);
      }
      {
        RenderLock lock(*this);
        cache.save();
      }
      // Line notices change slowly and the feed is ~170 KB per line: refresh at most every 30 min
      // (every few hours from a timer wake, which should stay short).
      if (board.fetchedAt - cache.alerts.fetchedAt >=
          (background != BackgroundMode::Off ? BACKGROUND_ALERTS_SECONDS : trenord_alerts::REFRESH_SECONDS)) {
        state = State::LOADING_ALERTS;
      } else {
        finishBackground(true);
      }
      break;
    case trenord::Result::NO_TIME:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_TIME);
      break;
    case trenord::Result::LOW_MEMORY:
      state = State::ERROR;
      errorMessage = tr(STR_INCOME_ERR_MEMORY);
      break;
    case trenord::Result::HTTP_ERROR:
      state = State::ERROR;
      errorMessage = tr(STR_TRAINS_ERR_FETCH);
      break;
    case trenord::Result::BAD_RESPONSE:
      state = State::ERROR;
      errorMessage = tr(STR_TRAINS_ERR_DATA);
      break;
  }
  if (result != trenord::Result::OK) finishBackground(false);
  requestUpdate();
}

void TrainsActivity::runAlerts() {
  // S-lines on this board (e.g. S5, S6); other categories have no line page.
  char codes[2][8] = {};
  const char* list[2];
  int count = 0;
  for (int i = 0; i < board.count && count < 2; i++) {
    const char* c = board.departures[i].category;
    if (c[0] != 'S' || c[1] < '0' || c[1] > '9') continue;
    bool dup = false;
    for (int j = 0; j < count; j++) dup = dup || strcmp(codes[j], c) == 0;
    if (dup) continue;
    snprintf(codes[count], sizeof(codes[count]), "%s", c);
    list[count] = codes[count];
    count++;
  }
  trenord_alerts::Alerts fresh;
  if (count > 0 && trenord_alerts::fetch(list, count, network_time::current(), fresh)) {
    cache.alerts = fresh;
    RenderLock lock(*this);
    cache.save();
  }
  state = State::READY;
  finishBackground(true);  // the board itself is fresh; notices are best effort
  requestUpdate();
}

void TrainsActivity::loop() {
  // Going to sleep: no more network steps or input once the snapshot is up.
  if (frozen) return;
  // Each network step shows its status before blocking.
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
  if (state == State::LOADING_ALERTS) {
    requestUpdateAndWait();
    runAlerts();
    return;
  }
  if (state == State::PICKING) return;
  if (background != BackgroundMode::Off) return;  // unattended: input wakes the device (main.cpp)

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (viewingReturn()) {
      buildStrip();  // puts the cursor back on the default origin
      showDefault();
    } else {
      finish();
    }
    return;
  }
  if (onAutoChip()) {
    // On release: the picker confirms on Confirm's release and would take this one.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openAutoRefreshPicker();
      return;
    }
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const int station = strip[cursor];
    if (station == config.origin) {
      showDefault();
    } else {
      showBoard(station, config.home);
    }
    return;
  }
  // The strip only moves a cursor; nothing is fetched until Confirm.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && cursor > 0) {
    cursor--;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) && cursor < stripCount) {
    cursor++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) && scroll + 2 < board.count) {
    scroll++;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Up) && scroll > 0) {
    scroll--;
    requestUpdate();
  }
}

void TrainsActivity::openAutoRefreshPicker() {
  auto picker = auto_refresh_ui::makeIntervalPicker(renderer, mappedInput, CrossPointState::SLEEP_APP_TRAINS);
  if (!picker) return;
  startActivityForResult(std::move(picker), [](const ActivityResult& result) {
    if (result.isCancelled) return;
    SETTINGS.trainsAutoRefresh = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
    SETTINGS.saveToFile();
  });
}

void TrainsActivity::render(RenderLock&&) {
  // A queued live frame must not replace the sleep snapshot, and a timer-wake
  // refresh draws nothing until that snapshot.
  if (frozen || background != BackgroundMode::Off) return;
  drawPage();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // After the first frame only small regions change; a fast refresh avoids the full-screen flash.
  renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  firstRenderDone = true;
}

void TrainsActivity::renderSleepSnapshot() {
  frozen = true;
  drawPage();
  sleep_snapshot::drawFrame(renderer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void TrainsActivity::drawPage() {
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool landscape = width > height;
  const int hints = frozen ? 0 : metrics.buttonHintsHeight;
  const int contentWidth = (landscape ? width - hints : width) - MARGIN * 2;
  const int contentBottom = landscape ? height : height - hints;
  const time_t now = network_time::current();  // 0 when the clock is unknown
  renderer.clearScreen();

  const int headerBottom = renderHeader(MARGIN, MARGIN - 6, contentWidth);
  // A stacked asleep badge pushes the hero down rather than running into it.
  const int heroTop = std::max(MARGIN + (contentWidth >= 600 ? 34 : (frozen ? 64 : 56)), headerBottom + 12);
  const int strikeHeight = renderStrike(MARGIN, heroTop - 6, contentWidth, now);
  const int heroBottom = renderHero(MARGIN, heroTop + strikeHeight, contentWidth, now);
  if (frozen) {
    // No strip to pick from while asleep; the list takes its room.
    const int listBottom = contentBottom - MARGIN;
    renderList(MARGIN, heroBottom + 20, contentWidth, listBottom - heroBottom - 20, now);
    return;
  }
  const int stripY = contentBottom - STRIP_HEIGHT - 10;
  renderList(MARGIN, heroBottom + 20, contentWidth, stripY - heroBottom - 30, now);
  renderStrip(stripY, contentWidth);
}

void TrainsActivity::routeLabel(char* out, const size_t size) const {
  char from[24];
  char to[24];
  upper(trenord::STATIONS[board.origin].label, from, sizeof(from));
  upper(trenord::STATIONS[board.towards].label, to, sizeof(to));
  snprintf(out, size, "%s  >  %s", from, to);
}

bool TrainsActivity::badgeBelowRoute(const int width) const {
  if (!frozen || width < 600) return false;
  char route[64];
  routeLabel(route, sizeof(route));
  char clock[8];
  clockText(board.fetchedAt, clock, sizeof(clock));
  const int routeWidth = income_gfx::trackedLabelWidth(renderer, UI_10_FONT_ID, route, TRACKING) + 24;
  return routeWidth + sleep_snapshot::inlineBadgeWidth(renderer, clock) > width;
}

int TrainsActivity::renderHeader(const int x, const int y, const int width) const {
  char route[64];
  routeLabel(route, sizeof(route));
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, route, TRACKING);
  // Portrait has no room beside the route; the status gets its own line.
  const bool wide = width >= 600;
  if (badgeBelowRoute(width)) {
    // Auto-refresh notes make the badge too long to share the route's line.
    return renderStatus(x, y + BADGE_ROW + 2, width);
  }
  if (wide && frozen) {
    // The asleep badge may only use what the route leaves free (it stacks when that is too narrow).
    const int routeWidth = income_gfx::trackedLabelWidth(renderer, UI_10_FONT_ID, route, TRACKING) + 24;
    return renderStatus(x + routeWidth, y + 2, width - routeWidth);
  }
  if (wide) return renderStatus(x, y + 2, width);
  return renderStatus(x, y + (frozen ? 30 : 26), width);  // the asleep badge is taller than a text line
}

int TrainsActivity::renderStatus(const int x, const int y, const int width) const {
  if (frozen) {
    char clock[8];
    clockText(board.fetchedAt, clock, sizeof(clock));
    return sleep_snapshot::drawBadge(renderer, x, y - 4, width, true, clock);
  }
  if (state == State::CONNECTING || state == State::LOADING || state == State::LOADING_ALERTS ||
      state == State::PICKING) {
    const bool fetching = state == State::LOADING || state == State::LOADING_ALERTS;
    income_gfx::drawStepStatus(renderer, x, y, width, true, fetching ? 1 : 0,
                               fetching ? tr(STR_INCOME_FETCHING) : tr(STR_INCOME_CONNECTING));
    return y;
  }
  char text[96];
  if (state == State::ERROR) {
    snprintf(text, sizeof(text), "%s", errorMessage);
  } else if (hasBoard) {
    char clock[8];
    clockText(board.fetchedAt, clock, sizeof(clock));
    snprintf(text, sizeof(text), "%s %s", tr(STR_INCOME_UPDATED), clock);
  } else {
    return y;
  }
  char lines[1][96];
  income_gfx::wrapText(renderer, SMALL_FONT_ID, text, width / 2, lines, 1);
  const int w = renderer.getTextWidth(SMALL_FONT_ID, lines[0]);
  renderer.drawText(SMALL_FONT_ID, x + width - w, y, lines[0]);
  return y;
}

// Index of the first departure that has not left yet (departed ones stay hidden).
int TrainsActivity::firstUpcoming(const time_t now) const {
  int i = 0;
  while (i < board.count && departed(board.departures[i], now)) i++;
  return i;
}

int TrainsActivity::renderStrike(const int x, const int y, const int width, const time_t now) const {
  const trenord_alerts::Strike& strike = cache.alerts.strike;
  if (now == 0 || strike.start == 0 || strike.end <= now) return 0;
  struct tm today;
  struct tm day;
  trenord::romeTime(now, today);
  trenord::romeTime(strike.start, day);
  char from[8];
  char to[8];
  clockText(strike.start, from, sizeof(from));
  clockText(strike.end, to, sizeof(to));
  const bool sameDay = today.tm_year == day.tm_year && today.tm_yday == day.tm_yday;
  const bool active = strike.start <= now || sameDay;
  if (!active && strike.start - now > 3 * 86400) return 0;

  char line[96];
  if (active) {
    snprintf(line, sizeof(line), "%s  %s-%s", tr(STR_TRAINS_STRIKE_TODAY), from, to);
    renderer.fillRect(x, y, width, 34, true);
    renderer.drawText(UI_12_FONT_ID, x + 12, y + 4, line, false, EpdFontFamily::BOLD);
    return 44;
  }
  char date[24];
  strftime(date, sizeof(date), "%a %d %b", &day);
  snprintf(line, sizeof(line), "%s %s  %s-%s", tr(STR_TRAINS_STRIKE), date, from, to);
  renderer.drawText(UI_10_FONT_ID, x, y, line, true, EpdFontFamily::BOLD);
  return 28;
}

int TrainsActivity::renderHero(const int x, const int y, const int width, const time_t now) const {
  constexpr int PIXEL = 13;
  const int heroHeight = PIXEL * 7 + PIXEL / 2;
  if (!hasBoard || board.count == 0) {
    income_gfx::drawPixelText(renderer, x, y, "--", PIXEL);
    const char* message = hasBoard ? tr(STR_TRAINS_NONE) : "";
    renderer.drawText(UI_12_FONT_ID, x + income_gfx::pixelTextWidth("--", PIXEL) + 28, y + 24, message, true,
                      EpdFontFamily::BOLD);
    return y + heroHeight;
  }

  const int firstLive = firstUpcoming(now);
  if (firstLive >= board.count) {
    income_gfx::drawPixelText(renderer, x, y, "--", PIXEL);
    renderer.drawText(UI_12_FONT_ID, x + income_gfx::pixelTextWidth("--", PIXEL) + 28, y + 24, tr(STR_TRAINS_NONE),
                      true, EpdFontFamily::BOLD);
    return y + heroHeight;
  }
  const trenord::Departure& next = board.departures[firstLive];
  const bool wide = width >= 600;
  char clock[8];
  clockText(next.scheduled, clock, sizeof(clock));
  char delay[24];
  delayText(next, delay, sizeof(delay));

  // Asleep, the hero is the departure time: a countdown would go stale on the panel.
  char big[8];
  int pixel = PIXEL;
  if (frozen) {
    snprintf(big, sizeof(big), "%s", clock);
    if (wide) pixel = 10;  // leaves the detail block its usual width
  } else {
    const int countdown = countdownMinutes(next, now, board.fetchedAt);
    snprintf(big, sizeof(big), countdown < 0 ? "--" : "%d", countdown);
  }
  income_gfx::drawPixelText(renderer, x, y, big, pixel);
  const int bigWidth = income_gfx::pixelTextWidth(big, pixel);
  if (!frozen) {
    renderer.drawText(UI_12_FONT_ID, x + bigWidth + 14, y + heroHeight - 34, tr(STR_TRAINS_MIN), true,
                      EpdFontFamily::BOLD);
  }

  // Detail block beside the hero number, or under it on narrow screens.
  const int detailX = wide ? x + (frozen ? bigWidth + 48 : std::max(bigWidth + 80, width / 3)) : x;
  const int detailY = wide ? y : y + heroHeight + 14;
  char line[64];
  if (frozen) {
    snprintf(line, sizeof(line), "%s", delay);
  } else {
    snprintf(line, sizeof(line), "%s  %s", clock, delay);
  }
  // Asleep with no delay reported, the train line moves up into the empty first line.
  const int lift = line[0] ? 0 : 40;
  renderer.drawText(NOTOSANS_18_FONT_ID, detailX, detailY - 4, line, true, EpdFontFamily::BOLD);

  char train[20];
  trainLabel(next, train, sizeof(train));
  char direction[28];
  directionLabel(next, direction, sizeof(direction));
  snprintf(line, sizeof(line), direction[0] ? "%s  >  %s" : "%s", train, direction);
  renderer.drawText(UI_12_FONT_ID, detailX, detailY + 36 - lift, line);
  const trenord_alerts::Notice* notice = cache.alerts.forTrain(next.number);
  char alert[96];
  disruptionText(next, alert, sizeof(alert));
  int alertLines = 0;
  if (alert[0]) {
    // The train's own alert gets two lines: the cancelled stretch and its reason should both show.
    char lines[2][96];
    alertLines =
        income_gfx::wrapText(renderer, UI_10_FONT_ID, alert, width - (detailX - x), lines, 2, EpdFontFamily::BOLD);
    for (int i = 0; i < alertLines; i++) {
      renderer.drawText(UI_10_FONT_ID, detailX, detailY + 66 - lift + i * ALERT_LINE, lines[i], true,
                        EpdFontFamily::BOLD);
    }
  } else if (notice) {
    // The notice's own words, cut to one line: "! Il treno 24556 ... non è ancora partito ..."
    // Skip the "Il treno 24556 (TREVIGLIO 16:10 - VARESE 18:21)" preamble; the hero already names the train.
    const char* text = notice->text;
    const char* close = strchr(text, ')');
    if (strncmp(text, "Il treno ", 9) == 0 && close && close - text < 70) text = close + 1;
    while (*text == ' ') text++;
    char reason[96];
    snprintf(reason, sizeof(reason), "! %s", text);
    char lines[1][96];
    income_gfx::wrapText(renderer, UI_10_FONT_ID, reason, width - (detailX - x), lines, 1, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, detailX, detailY + 66 - lift, lines[0], true, EpdFontFamily::BOLD);
  } else if (next.platform[0]) {
    snprintf(line, sizeof(line), "%s %s", tr(STR_TRAINS_PLATFORM), next.platform);
    renderer.drawText(UI_10_FONT_ID, detailX, detailY + 66 - lift, line, true, EpdFontFamily::BOLD);
  }
  if (next.cancelled()) {
    // Strike the hero number so a cancelled train never reads as catchable.
    renderer.fillRect(x - 4, y + (pixel * 7) / 2 - 2, bigWidth + 8, 5, true);
  }
  const int extra = alertLines > 1 ? (alertLines - 1) * ALERT_LINE : 0;
  if (!wide) return detailY + 90 + extra;
  return std::max(y + heroHeight, detailY + 88 - lift + extra);
}

void TrainsActivity::renderList(const int x, const int y, const int width, const int height, const time_t now) const {
  if (!hasBoard) return;
  const int rows = std::max(0, height / ROW_HEIGHT);
  const int first = firstUpcoming(now) + 1 + scroll;
  // Column positions: time, countdown, delay, train, destination, platform.
  // Narrow (portrait) screens drop the countdown column; the hero carries it.
  // The sleep snapshot drops it too: times and delays stay true, countdowns don't.
  const bool wide = width >= 600;
  const bool countdowns = wide && !frozen;
  const int colIn = x + 70;
  const int colDelay = countdowns ? x + 160 : x + (wide ? 80 : 70);
  const int colTrain = countdowns ? x + 270 : x + (wide ? 200 : 160);
  const int colDest = countdowns ? x + 380 : x + (wide ? 320 : 250);
  for (int r = 0; r < rows && first + r < board.count; r++) {
    const trenord::Departure& d = board.departures[first + r];
    const int rowY = y + r * ROW_HEIGHT;
    if (r > 0) income_gfx::fillDither(renderer, x, rowY - 6, width, 1, 6);

    char text[48];
    clockText(d.scheduled, text, sizeof(text));
    renderer.drawText(UI_12_FONT_ID, x, rowY, text, true, EpdFontFamily::BOLD);
    if (d.cancelled()) {
      // Struck through, like the hero, so a cancelled row never reads as catchable.
      const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
      renderer.fillRect(x - 2, rowY + renderer.getLineHeight(UI_12_FONT_ID) / 2 - 1, w + 4, 3, true);
    }
    if (countdowns) {
      const int countdown = countdownMinutes(d, now, board.fetchedAt);
      if (countdown < 0) {
        snprintf(text, sizeof(text), "-");
      } else {
        snprintf(text, sizeof(text), "%d %s", countdown, tr(STR_TRAINS_MIN));
      }
      renderer.drawText(UI_12_FONT_ID, colIn, rowY, text);
    }
    delayText(d, text, sizeof(text));
    if (!d.cancelled() && (d.disruption != trenord::Disruption::NONE || cache.alerts.forTrain(d.number))) {
      // The train has an alert (its own or a line notice naming it): flag it without crowding the row.
      snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s!", text[0] ? " " : "");
    }
    renderer.drawText(UI_10_FONT_ID, colDelay, rowY + 2, text, true, EpdFontFamily::BOLD);
    trainLabel(d, text, sizeof(text));
    renderer.drawText(UI_10_FONT_ID, colTrain, rowY + 2, text);
    char direction[28];
    directionLabel(d, direction, sizeof(direction));
    renderer.drawText(UI_10_FONT_ID, colDest, rowY + 2, direction);
    if (d.platform[0]) {
      snprintf(text, sizeof(text), "%s %s", tr(STR_TRAINS_PLATFORM_SHORT), d.platform);
      const int w = renderer.getTextWidth(UI_10_FONT_ID, text);
      renderer.drawText(UI_10_FONT_ID, x + width - w, rowY + 2, text);
    }
  }
  // Scroll hint when more departures sit below.
  if (first + rows < board.count) {
    const int hintY = y + rows * ROW_HEIGHT - 12;
    for (int i = 0; i < 3; i++) renderer.fillRect(x + width / 2 - 14 + i * 12, hintY, 4, 4, true);
  }
}

int TrainsActivity::renderAutoChip(const int y, const int right) const {
  char label[24];
  auto_refresh_ui::formatHint(CrossPointState::SLEEP_APP_TRAINS, label, sizeof(label));
  const int w = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) + 18;
  const int x = right - w;
  const bool selected = onAutoChip();
  if (selected) {
    renderer.fillRoundedRect(x, y, w, STRIP_HEIGHT - 8, 8, Color::Black);
  } else {
    renderer.drawRoundedRect(x, y, w, STRIP_HEIGHT - 8, 1, 8, true);
  }
  renderer.drawText(UI_10_FONT_ID, x + 9, y + 8, label, !selected, EpdFontFamily::BOLD);
  return w;
}

void TrainsActivity::renderStrip(const int y, const int fullWidth) const {
  renderer.fillRect(MARGIN, y - 8, fullWidth, 2, true);
  const int width = fullWidth - renderAutoChip(y, MARGIN + fullWidth) - AUTO_CHIP_GAP;
  // Lay items out left to right, sliding the window so the cursor stays visible.
  int widths[MAX_STRIP];
  for (int i = 0; i < stripCount; i++) {
    widths[i] = renderer.getTextWidth(UI_10_FONT_ID, trenord::STATIONS[strip[i]].label, EpdFontFamily::BOLD) + 24;
  }
  int start = 0;
  const auto spanWidth = [&](const int from, const int to) {
    int w = 0;
    for (int i = from; i <= to; i++) w += widths[i];
    return w;
  };
  const int visibleCursor = std::min(cursor, stripCount - 1);
  while (start < visibleCursor && spanWidth(start, visibleCursor) > width) start++;

  int cx = MARGIN;
  for (int i = start; i < stripCount && cx + widths[i] <= MARGIN + width; i++) {
    const int station = strip[i];
    const bool active = station == board.origin;
    const bool selected = i == cursor;
    if (selected) {
      renderer.fillRoundedRect(cx, y, widths[i] - 6, STRIP_HEIGHT - 8, 8, Color::Black);
    } else if (active) {
      renderer.drawRoundedRect(cx, y, widths[i] - 6, STRIP_HEIGHT - 8, 2, 8, true);
    }
    renderer.drawText(UI_10_FONT_ID, cx + 9, y + 8, trenord::STATIONS[station].label, !selected,
                      active || selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    cx += widths[i];
  }
  if (start > 0) renderer.fillRect(MARGIN - 12, y + 12, 4, 12, true);
}
