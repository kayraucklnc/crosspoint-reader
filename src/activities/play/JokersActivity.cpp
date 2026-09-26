#include "JokersActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JokersText.h"
#include "MappedInputManager.h"

using namespace jokers;

namespace {
constexpr const char* SAVE_DIR = "/.crosspoint/games";
constexpr const char* SAVE_PATH = "/.crosspoint/games/jokers.json";
constexpr size_t SAVE_TEXT = 512;
constexpr unsigned long PLAY_HOLD_MS = 600;
constexpr unsigned long REVEAL_STEP_MS = 900;
constexpr unsigned long REVEAL_HOLD_MS = 1600;
constexpr int ACTION_COUNT = 3;       // Play, Discard, Sort
constexpr int SHOP_ACTION_COUNT = 2;  // Reroll, Next blind
constexpr int MENU_ITEMS = 5;

int popcount(uint8_t v) {
  int n = 0;
  for (; v; v &= v - 1) n++;
  return n;
}

int32_t jsonInt(const char* json, const char* key) {
  const char* p = strstr(json, key);
  return p ? static_cast<int32_t>(strtol(p + strlen(key), nullptr, 10)) : 0;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
}  // namespace

void JokersActivity::onEnter() {
  Activity::onEnter();
  if (!pack.open()) LOG_ERR("JOKERS", "Art pack missing, using vector art");
  load();
  menuCursor = 0;
  go(Screen::TITLE);
}

void JokersActivity::onExit() {
  save();
  pack.close();
  Activity::onExit();
}

void JokersActivity::go(const Screen next, const bool clean) {
  screen = next;
  redrawAll = true;
  cleanRefresh = clean;
  dirty = D_ALL;
  toast = nullptr;
  armed = -1;
  grabbed = false;
  requestUpdate();
}

void JokersActivity::mark(const uint8_t bits) {
  dirty |= bits;
  requestUpdate();
}

// ---- persistence --------------------------------------------------------------

void JokersActivity::load() {
  auto text = makeUniqueNoThrow<char[]>(SAVE_TEXT);
  if (!text) {
    LOG_ERR("JOKERS", "OOM: save buffer");
    return;
  }
  memset(text.get(), 0, SAVE_TEXT);
  if (!Storage.exists(SAVE_PATH) || Storage.readFileToBuffer(SAVE_PATH, text.get(), SAVE_TEXT - 1) == 0) return;
  const char* json = text.get();
  stats.bestAnte = jsonInt(json, "\"bestAnte\":");
  stats.bestHand = jsonInt(json, "\"bestHand\":");
  stats.runsWon = jsonInt(json, "\"won\":");
  stats.runsPlayed = jsonInt(json, "\"runs\":");
  const char* hex = strstr(json, "\"run\":\"");
  if (!hex) return;
  hex += 7;
  uint8_t bytes[SAVE_BYTES];
  size_t n = 0;
  while (n < SAVE_BYTES && hexValue(hex[0]) >= 0 && hexValue(hex[1]) >= 0) {
    bytes[n++] = static_cast<uint8_t>(hexValue(hex[0]) << 4 | hexValue(hex[1]));
    hex += 2;
  }
  hasRun = deserialize(run, bytes, n) && run.phase != Phase::OVER;
  if (!hasRun) LOG_ERR("JOKERS", "Saved run rejected (%u bytes)", static_cast<unsigned>(n));
}

void JokersActivity::save() {
  auto text = makeUniqueNoThrow<char[]>(SAVE_TEXT);
  if (!text) {
    LOG_ERR("JOKERS", "OOM: save buffer");
    return;
  }
  int len = snprintf(text.get(), SAVE_TEXT, "{\"v\":1,\"bestAnte\":%ld,\"bestHand\":%ld,\"won\":%ld,\"runs\":%ld",
                     static_cast<long>(stats.bestAnte), static_cast<long>(stats.bestHand),
                     static_cast<long>(stats.runsWon), static_cast<long>(stats.runsPlayed));
  if (hasRun) {
    uint8_t bytes[SAVE_BYTES];
    const size_t n = serialize(run, bytes, sizeof(bytes));
    len += snprintf(text.get() + len, SAVE_TEXT - len, ",\"run\":\"");
    for (size_t i = 0; i < n && len + 3 < static_cast<int>(SAVE_TEXT); i++) {
      len += snprintf(text.get() + len, SAVE_TEXT - len, "%02x", bytes[i]);
    }
    len += snprintf(text.get() + len, SAVE_TEXT - len, "\"");
  }
  snprintf(text.get() + len, SAVE_TEXT - len, "}");
  if (!Storage.exists(SAVE_DIR)) Storage.mkdir(SAVE_DIR);
  if (!Storage.writeFile(SAVE_PATH, String(text.get()))) LOG_ERR("JOKERS", "Failed to write %s", SAVE_PATH);
  saveDirty = false;
}

void JokersActivity::recordStats() {
  stats.bestAnte = std::max<int32_t>(stats.bestAnte, run.ante);
  if (run.bestHand > stats.bestHand) {
    stats.bestHand = run.bestHand;
    newRecord = true;
  }
}

// ---- flow ---------------------------------------------------------------------

void JokersActivity::newRun() {
  uint32_t seed = static_cast<uint32_t>(millis()) * 2654435761u;
#ifdef SIMULATOR
  // Deterministic runs for scripted simulator tests.
  if (const char* fixed = getenv("CROSSPOINT_SIM_JOKERS_SEED"))
    seed = static_cast<uint32_t>(strtoul(fixed, nullptr, 10));
#endif
  jokers::newRun(run, seed);
  hasRun = true;
  newRecord = false;
  stats.runsPlayed++;
  save();
  go(Screen::BLIND);
}

void JokersActivity::startBlind() {
  jokers::startBlind(run);
  selected = 0;
  row = ROW_MID;
  cursor[ROW_MID] = 0;
  cursor[ROW_TOP] = 0;
  cursor[ROW_BOTTOM] = 0;
  save();
  go(Screen::ROUND);
}

// Records the finished run; the caller picks the next screen.
void JokersActivity::endRun() {
  recordStats();
  if (run.won) stats.runsWon++;
  run.phase = Phase::OVER;
  hasRun = false;
  save();
}

void JokersActivity::playSelection() {
  if (selected == 0) {
    toast = tr(STR_JK_PICK);
    mark(D_FOOTER);
    return;
  }
  scoreBefore = run.roundScore;
  LOG_DBG("JOKERS", "play mask %02x hand %d %d %d %d %d %d %d %d", selected, run.hand[0], run.hand[1], run.hand[2],
          run.hand[3], run.hand[4], run.hand[5], run.hand[6], run.hand[7]);
  if (!playHand(run, selected, reveal)) return;
  selected = 0;
  cursor[ROW_MID] = 0;
  row = ROW_MID;
  recordStats();
  revealStep = 0;
  revealAt = millis();
  go(Screen::REVEAL, false);
}

void JokersActivity::finishReveal() {
  if (roundWon(run)) {
    payout = cashOut(run);
    // The final boss ends the run here, so quitting on the cash-out screen keeps the win.
    if (run.won) {
      endRun();
    } else {
      save();
    }
    go(Screen::CASH_OUT);
  } else if (roundLost(run)) {
    endRun();
    go(Screen::OVER);
  } else {
    save();
    go(Screen::ROUND);
  }
}

void JokersActivity::handleReveal(const bool anyPress) {
  const unsigned long now = millis();
  if (anyPress) {
    if (revealStep + 1 < REVEAL_STEPS) {
      revealStep = REVEAL_STEPS - 1;
      revealAt = now;
      go(Screen::REVEAL, false);
    } else {
      finishReveal();
    }
    return;
  }
  if (revealStep + 1 < REVEAL_STEPS) {
    if (now - revealAt >= REVEAL_STEP_MS) {
      revealStep++;
      if (revealStep == 1 && !jokersFired()) revealStep++;
      revealAt = now;
      go(Screen::REVEAL, false);
    }
  } else if (now - revealAt >= REVEAL_HOLD_MS) {
    finishReveal();
  }
}

bool JokersActivity::jokersFired() const {
  for (const auto& fx : reveal.fx) {
    if (fx.kind != Fx::NONE) return true;
  }
  return false;
}

int JokersActivity::rowLength(const uint8_t r) const {
  if (screen == Screen::SHOP) {
    if (r == ROW_TOP) return SHOP_SLOTS;
    return r == ROW_MID ? run.jokerCount : SHOP_ACTION_COUNT;
  }
  if (r == ROW_TOP) return run.jokerCount;
  return r == ROW_MID ? run.handCount : ACTION_COUNT;
}

void JokersActivity::roundAction() {
  switch (cursor[ROW_BOTTOM]) {
    case 0:
      playSelection();
      return;
    case 1:
      if (selected == 0) {
        toast = tr(STR_JK_PICK);
        mark(D_FOOTER);
        return;
      }
      LOG_DBG("JOKERS", "discard mask %02x", selected);
      if (!discardCards(run, selected)) return;
      // Back to the hand to pick again; the action row resets to Play.
      selected = 0;
      row = ROW_MID;
      cursor[ROW_MID] = 0;
      cursor[ROW_BOTTOM] = 0;
      save();
      break;
    default: {
      // Re-sort and carry the selection over by card identity.
      uint64_t chosen = 0;
      for (int i = 0; i < run.handCount; i++) {
        if (selected & (1u << i)) chosen |= 1ull << run.hand[i];
      }
      const uint8_t atCursor = run.hand[cursor[ROW_MID]];
      run.sortBySuit = !run.sortBySuit;
      sortHand(run);
      selected = 0;
      for (int i = 0; i < run.handCount; i++) {
        if ((chosen >> run.hand[i]) & 1u) selected |= static_cast<uint8_t>(1u << i);
        if (run.hand[i] == atCursor) cursor[ROW_MID] = static_cast<uint8_t>(i);
      }
      break;
    }
  }
  mark(D_ALL);
}

void JokersActivity::handleRound(const bool confirm, const bool hold, const int dx, const int dy) {
  if (hold) {
    playSelection();
    return;
  }
  if (dy != 0) {
    int next = std::clamp(row + dy, 0, 2);
    if (next == ROW_TOP && run.jokerCount == 0) next = row;
    if (next != row) {
      row = static_cast<uint8_t>(next);
      grabbed = false;
      cursor[ROW_TOP] = static_cast<uint8_t>(std::min<int>(cursor[ROW_TOP], std::max(0, run.jokerCount - 1)));
      mark(D_JOKERS | D_INFO | D_HAND | D_ACTIONS | D_HINTS | D_FOOTER);
    }
  }
  const int len = rowLength(row);
  if (dx != 0 && len > 0) {
    const int from = cursor[row];
    if (row == ROW_TOP && grabbed) {
      const int to = std::clamp(from + dx, 0, len - 1);
      moveJoker(run, from, to);
      cursor[row] = static_cast<uint8_t>(to);
    } else {
      cursor[row] = static_cast<uint8_t>((from + dx + len) % len);
    }
    mark(row == ROW_TOP ? D_JOKERS | D_INFO : (row == ROW_MID ? D_HAND | D_HINTS : D_ACTIONS | D_HINTS | D_FOOTER));
  }
  if (!confirm) return;
  toast = nullptr;
  if (row == ROW_TOP) {
    if (run.jokerCount == 0) return;
    grabbed = !grabbed;
    if (!grabbed) save();
    mark(D_JOKERS | D_HINTS);
  } else if (row == ROW_MID) {
    const uint8_t bit = static_cast<uint8_t>(1u << cursor[ROW_MID]);
    if (!(selected & bit) && popcount(selected) >= MAX_PLAY) {
      toast = tr(STR_JK_PICK);
      mark(D_FOOTER);
      return;
    }
    selected ^= bit;
    mark(D_HAND | D_INFO | D_ACTIONS | D_HINTS | D_FOOTER);
  } else {
    roundAction();
  }
}

void JokersActivity::handleShop(const bool confirm, const int dx, const int dy) {
  if (dy != 0) {
    int next = std::clamp(row + dy, 0, 2);
    if (next == ROW_MID && run.jokerCount == 0) next = std::clamp(next + dy, 0, 2);
    row = static_cast<uint8_t>(next);
    armed = -1;
    toast = nullptr;
    mark(D_ALL);
  }
  const int len = rowLength(row);
  if (dx != 0 && len > 0) {
    cursor[row] = static_cast<uint8_t>((cursor[row] + dx + len) % len);
    armed = -1;
    toast = nullptr;
    mark(D_ALL);
  }
  if (!confirm) return;
  toast = nullptr;
  const int c = cursor[row];
  if (row == ROW_TOP) {
    if (run.shop[c] == EMPTY) return;
    if (run.money < shopPrice(run, c)) {
      toast = tr(STR_JK_NO_MONEY);
    } else if (c < SHOP_JOKERS && run.jokerCount >= MAX_JOKERS) {
      toast = tr(STR_JK_SLOTS_FULL);
    } else if (buy(run, c)) {
      // Jump to the next offer still for sale, else to "Next blind".
      int next = -1;
      for (int i = 1; i <= SHOP_SLOTS && next < 0; i++) {
        if (run.shop[(c + i) % SHOP_SLOTS] != EMPTY) next = (c + i) % SHOP_SLOTS;
      }
      if (next >= 0) {
        cursor[ROW_TOP] = static_cast<uint8_t>(next);
      } else {
        row = ROW_BOTTOM;
        cursor[ROW_BOTTOM] = 1;
      }
      save();
    }
  } else if (row == ROW_MID) {
    if (c >= run.jokerCount) return;
    if (armed == c) {
      sellJoker(run, c);
      armed = -1;
      cursor[ROW_MID] = static_cast<uint8_t>(std::max(0, std::min<int>(c, run.jokerCount - 1)));
      if (run.jokerCount == 0) row = ROW_TOP;
      save();
    } else {
      armed = static_cast<int8_t>(c);
    }
  } else if (c == 0) {
    if (!reroll(run)) {
      toast = tr(STR_JK_NO_MONEY);
    } else {
      save();
    }
  } else {
    advanceBlind(run);
    recordStats();
    save();
    go(Screen::BLIND);
    return;
  }
  mark(D_ALL);
}

void JokersActivity::handleTitle(const bool confirm, const int dx) {
  const int items = hasRun ? 3 : 2;
  if (dx != 0) {
    menuCursor = static_cast<uint8_t>((menuCursor + dx + items) % items);
    armed = -1;
    mark(D_ALL);
  }
  if (!confirm) return;
  const int item = hasRun ? menuCursor : menuCursor + 1;
  if (item == 0) {
    selected = 0;
    row = run.phase == Phase::ROUND ? ROW_MID : ROW_TOP;
    for (auto& c : cursor) c = 0;
    go(run.phase == Phase::ROUND ? Screen::ROUND : (run.phase == Phase::SHOP ? Screen::SHOP : Screen::BLIND));
  } else if (item == 1) {
    // Starting over discards the saved run: ask twice.
    if (hasRun && armed != 1) {
      armed = 1;
      mark(D_ALL);
      return;
    }
    newRun();
  } else {
    underlay = Screen::TITLE;
    helpPage = 0;
    go(Screen::HELP);
  }
}

void JokersActivity::openMenu() {
  underlay = screen;
  screen = Screen::MENU;
  menuCursor = 0;
  armed = -1;
  redrawAll = false;
  cleanRefresh = false;
  requestUpdate();
}

void JokersActivity::menuSelect() {
  switch (menuCursor) {
    case 0:
      go(underlay, false);
      break;
    case 1:
      go(Screen::LEVELS);
      break;
    case 2:
      helpPage = 0;
      go(Screen::HELP);
      break;
    case 3:
      save();
      finish();
      break;
    default:
      if (armed == MENU_ITEMS - 1) {
        endRun();
        go(Screen::OVER);
      } else {
        armed = MENU_ITEMS - 1;
        requestUpdate();
      }
      break;
  }
}

void JokersActivity::loop() {
  using B = MappedInputManager::Button;
  RenderLock lock(*this);
  const bool back = mappedInput.wasReleased(B::Back);
  const bool hold = screen == Screen::ROUND && mappedInput.wasLongPressed(B::Confirm, PLAY_HOLD_MS);
  const bool confirm = mappedInput.wasReleased(B::Confirm);
  const int dx = (mappedInput.wasPressed(B::Right) ? 1 : 0) - (mappedInput.wasPressed(B::Left) ? 1 : 0);
  const int dy = (mappedInput.wasPressed(B::Down) ? 1 : 0) - (mappedInput.wasPressed(B::Up) ? 1 : 0);

  if (screen == Screen::REVEAL) {
    handleReveal(back || confirm || dx != 0 || dy != 0);
    return;
  }
  if (back) {
    switch (screen) {
      case Screen::TITLE:
        lock.unlock();
        finish();
        return;
      case Screen::OVER:
        menuCursor = 0;
        go(Screen::TITLE);
        return;
      case Screen::MENU:
        go(underlay, false);
        return;
      case Screen::HELP:
      case Screen::LEVELS:
        if (underlay == Screen::TITLE) {
          go(Screen::TITLE);
        } else {
          // Back to the menu, redrawn over its screen.
          go(Screen::MENU, false);
        }
        return;
      default:
        openMenu();
        return;
    }
  }

  switch (screen) {
    case Screen::TITLE:
      handleTitle(confirm, dx + dy);
      break;
    case Screen::BLIND:
      if (confirm) startBlind();
      break;
    case Screen::ROUND:
      handleRound(confirm, hold, dx, dy);
      break;
    case Screen::CASH_OUT:
      if (!confirm) break;
      if (run.phase == Phase::OVER) {
        go(Screen::OVER);
      } else {
        row = ROW_TOP;
        cursor[ROW_TOP] = 0;
        cursor[ROW_MID] = 0;
        cursor[ROW_BOTTOM] = 0;
        go(Screen::SHOP);
      }
      break;
    case Screen::SHOP:
      handleShop(confirm, dx, dy);
      break;
    case Screen::OVER:
      if (confirm) newRun();
      break;
    case Screen::MENU:
      if (dx + dy != 0) {
        menuCursor = static_cast<uint8_t>((menuCursor + dx + dy + MENU_ITEMS) % MENU_ITEMS);
        armed = -1;
        requestUpdate();
      }
      if (confirm) menuSelect();
      break;
    case Screen::HELP:
      if (dx != 0) {
        helpPage = static_cast<uint8_t>(std::clamp(helpPage + dx, 0, HELP_PAGES - 1));
        go(Screen::HELP, false);
      } else if (confirm) {
        if (helpPage + 1 < HELP_PAGES) {
          helpPage++;
          go(Screen::HELP, false);
        } else if (underlay == Screen::TITLE) {
          go(Screen::TITLE);
        } else {
          go(Screen::MENU, false);
        }
      }
      break;
    case Screen::LEVELS:
      if (confirm) go(Screen::MENU, false);
      break;
    case Screen::REVEAL:
      break;
  }
}
