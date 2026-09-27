// Home's per-book actions: the hold-Read sheet, the reset confirmations and
// the Book info panel. Drawing helpers live in HomeActivity.cpp.
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "HomeActivity.h"
#include "LibraryMetaStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookShelfActions.h"
#include "util/ReadingStats.h"

namespace {
enum ActionRow : int { ROW_READ, ROW_REMOVE, ROW_FINISH_ARCHIVE, ROW_RESET, ROW_INFO, ACTION_ROWS };
constexpr int CHOICE_RESET = 1;  // second row of both reset confirmations; Cancel is first

const char* fileNameOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

}  // namespace

void HomeActivity::showBookActions() {
  const RecentBook& book = recentBooks[bookIndex];
  sheetPath = book.path;
  sheetTitle = book.title.empty() ? fileNameOf(book.path) : book.title;
  const char* options[ACTION_ROWS] = {tr(STR_HOME_READ), tr(STR_HOME_REMOVE), tr(STR_HOME_FINISH_ARCHIVE),
                                      tr(STR_HOME_RESET_PROGRESS), tr(STR_HOME_BOOK_INFO)};
  sheet = Sheet::Actions;
  sheetChoice = -1;
  popup.show(sheetTitle.c_str(), options, ACTION_ROWS, ROW_READ, [this](const int choice) { sheetChoice = choice; });
  requestUpdate();
}

// The popup's callback only records the choice: acting on it here, after the
// popup has closed, lets a choice open the next popup without replacing the
// callback that is still running.
void HomeActivity::handleSheetInput() {
  popup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (popup.isActive()) return;
  const Sheet done = sheet;
  const int choice = sheetChoice;
  sheet = Sheet::None;
  sheetChoice = -1;
  if (choice >= 0) onSheetChoice(done, choice);
}

void HomeActivity::onSheetChoice(const Sheet done, const int choice) {
  const char* confirm[] = {tr(STR_CANCEL), tr(STR_HOME_RESET_PROGRESS)};
  if (done == Sheet::ResetAsk || done == Sheet::ResetSure) {
    if (choice != CHOICE_RESET) return;
    if (done == Sheet::ResetAsk) {
      sheet = Sheet::ResetSure;
      popup.show(tr(STR_HOME_RESET_SURE), tr(STR_HOME_RESET_WARNING), confirm, 2, 0,
                 [this](const int c) { sheetChoice = c; });
      requestUpdate();
      return;
    }
    if (!book_shelf::resetProgress(sheetPath)) LOG_ERR("HOME", "Progress reset incomplete: %s", sheetPath.c_str());
    loadCarousel();
    requestUpdate();
    return;
  }

  switch (choice) {
    case ROW_READ:
      activityManager.goToReader(sheetPath);
      return;
    case ROW_REMOVE:
      book_shelf::removeFromHome(sheetPath);
      loadCarousel();
      break;
    case ROW_FINISH_ARCHIVE:
      book_shelf::markFinishedAndArchive(sheetPath);
      loadCarousel();
      break;
    case ROW_RESET:
      sheet = Sheet::ResetAsk;
      popup.show(tr(STR_HOME_RESET_ASK), sheetTitle.c_str(), confirm, 2, 0, [this](const int c) { sheetChoice = c; });
      break;
    case ROW_INFO:
      openBookInfo();
      return;
    default:
      break;
  }
  requestUpdate();
}

void HomeActivity::openBookInfo() {
  const RecentBook& book = recentBooks[bookIndex];
  info = InfoText{};
  info.title = sheetTitle;
  info.author = book.author;
  info.file = fileNameOf(book.path);

  reading_stats::BookStats stats;
  const bool hasStats = reading_stats::getBook(book.path.c_str(), stats);
  const uint8_t flags = LIBRARY_META.flagsFor(book.path);
  const int progress = bookProgress(bookIndex);
  const bool finished = !(flags & LibraryMetaStore::UNFINISHED) &&
                        ((flags & LibraryMetaStore::FINISHED) || (hasStats && stats.finished) || progress >= 100);

  char buf[48];
  if (progress >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", progress);
    info.progress = buf;
  } else {
    info.progress = tr(STR_HOME_INFO_NOT_STARTED);
  }
  if (hasStats && stats.sec > 0) {
    reading_stats::formatDuration(stats.sec, buf, sizeof(buf));
    info.timeRead = buf;
  } else {
    info.timeRead = "-";
  }
  if (finished) {
    info.status = tr(STR_LIB_FILTER_FINISHED);
  } else {
    info.status = progress >= 0 ? tr(STR_LIB_FILTER_READING) : tr(STR_HOME_INFO_NOT_STARTED);
  }
  infoOpen = true;
  requestUpdate();
}

void HomeActivity::handleInfoInput() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    infoOpen = false;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    infoOpen = false;
    activityManager.goToReader(sheetPath);
  }
}

// A card over Home: title and author, then label/value rows.
void HomeActivity::renderInfoPanel() const {
  constexpr int SIDE = 36;
  constexpr int PAD = 22;
  constexpr int ROW = 34;
  const int screenWidth = renderer.getScreenWidth();
  const int width = screenWidth - SIDE * 2;
  const int inner = width - PAD * 2;
  const bool hasAuthor = !info.author.empty();
  const int headerHeight = hasAuthor ? 64 : 36;
  const int height = PAD + headerHeight + 14 + ROW * 4 + PAD;
  const int top = (renderer.getScreenHeight() - height) / 2 - 20;

  renderer.fillRoundedRect(SIDE, top, width, height, 14, Color::White);
  renderer.drawRoundedRect(SIDE, top, width, height, 2, 14, true);

  char line[96];
  int y = top + PAD;
  fitText(renderer, UI_12_FONT_ID, info.title.c_str(), EpdFontFamily::BOLD, inner, line, sizeof(line));
  renderer.drawCenteredText(UI_12_FONT_ID, y, line, true, EpdFontFamily::BOLD);
  if (hasAuthor) {
    fitText(renderer, UI_10_FONT_ID, info.author.c_str(), EpdFontFamily::REGULAR, inner, line, sizeof(line));
    renderer.drawCenteredText(UI_10_FONT_ID, y + 32, line);
  }
  y += headerHeight;
  renderer.drawLine(SIDE + PAD, y, SIDE + width - PAD, y, 1, true);
  y += 14;

  const struct {
    const char* label;
    const std::string& value;
  } rows[] = {{tr(STR_HOME_INFO_PROGRESS), info.progress},
              {tr(STR_HOME_INFO_TIME), info.timeRead},
              {tr(STR_HOME_INFO_STATUS), info.status},
              {tr(STR_HOME_INFO_FILE), info.file}};
  for (const auto& row : rows) {
    renderer.drawText(UI_10_FONT_ID, SIDE + PAD, y, row.label);
    const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, row.label);
    fitText(renderer, UI_10_FONT_ID, row.value.c_str(), EpdFontFamily::BOLD, inner - labelWidth - 16, line,
            sizeof(line));
    const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, line, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, SIDE + width - PAD - valueWidth, y, line, true, EpdFontFamily::BOLD);
    y += ROW;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_HOME_READ), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
