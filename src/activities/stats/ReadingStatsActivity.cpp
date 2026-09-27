#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/income/IncomeGraphics.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MARGIN = 20;
constexpr int TRACKING = 3;
constexpr int SECTION_GAP = 18;
constexpr int LABEL_H = 26;        // tracked section label + gap
constexpr int BAR_CAP = 3;         // solid top edge of a dithered bar
constexpr int BAR_LEVEL = 5;       // Bayer level of a regular bar body (of 16)
constexpr int VALUE_LABEL_H = 20;  // room above the tallest bar for its value
constexpr int AXIS_LABEL_H = 22;   // room under the baseline for the axis label

constexpr StrId WEEKDAY_SHORT[7] = {StrId::STR_WD_MON, StrId::STR_WD_TUE, StrId::STR_WD_WED, StrId::STR_WD_THU,
                                    StrId::STR_WD_FRI, StrId::STR_WD_SAT, StrId::STR_WD_SUN};
constexpr StrId WEEKDAY_PLURAL[7] = {StrId::STR_WD_MONDAYS,   StrId::STR_WD_TUESDAYS, StrId::STR_WD_WEDNESDAYS,
                                     StrId::STR_WD_THURSDAYS, StrId::STR_WD_FRIDAYS,  StrId::STR_WD_SATURDAYS,
                                     StrId::STR_WD_SUNDAYS};

// "38 min" under an hour, else whole hours: bar labels must stay narrow.
void compactDuration(const uint32_t sec, char* buf, const size_t len) {
  if (sec == 0) {
    buf[0] = '\0';
  } else if (sec < 3600) {
    snprintf(buf, len, tr(STR_DURATION_MIN), static_cast<unsigned>((sec + 30) / 60));
  } else {
    snprintf(buf, len, tr(STR_DURATION_H), static_cast<unsigned>((sec + 1800) / 3600));
  }
}

// "h:mm" for the pixel-font hero numbers.
void clockDuration(const uint32_t sec, char* buf, const size_t len) {
  const unsigned mins = sec / 60;
  snprintf(buf, len, "%u:%02u", mins / 60, mins % 60);
}

// Recent-books title when known, else the file name without its extension.
void bookTitle(const char* path, char* buf, const size_t len) {
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path == path && !book.title.empty()) {
      snprintf(buf, len, "%s", book.title.c_str());
      return;
    }
  }
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  const char* dot = strrchr(name, '.');
  const int n = dot && dot > name ? static_cast<int>(dot - name) : static_cast<int>(strlen(name));
  snprintf(buf, len, "%.*s", n, name);
}

struct BarChart {
  const uint32_t* values;
  int count;
  int highlight;  // drawn solid; -1 = none
  const char* const* labels;
};

void drawBarChart(const GfxRenderer& renderer, const BarChart& chart, const int x, const int y, const int width,
                  const int height) {
  const int baseline = y + height - AXIS_LABEL_H;
  const int plotTop = y + VALUE_LABEL_H;
  const int plotHeight = baseline - plotTop;
  if (plotHeight <= 8 || chart.count <= 0) return;
  const uint32_t maxValue = std::max<uint32_t>(1, *std::max_element(chart.values, chart.values + chart.count));
  const int slot = width / chart.count;
  const int barWidth = std::max(6, slot * 3 / 5);

  for (int i = 0; i < chart.count; i++) {
    const int cx = x + slot * i + slot / 2;
    const int barX = cx - barWidth / 2;
    const int barHeight = static_cast<int>(static_cast<uint64_t>(chart.values[i]) * plotHeight / maxValue);
    const int barTop = baseline - barHeight;
    const bool hot = i == chart.highlight;
    if (barHeight > 0) {
      if (hot) {
        renderer.fillRect(barX, barTop, barWidth, barHeight, true);
      } else {
        income_gfx::fillDither(renderer, barX, barTop, barWidth, barHeight, BAR_LEVEL);
        renderer.fillRect(barX, barTop, barWidth, std::min(BAR_CAP, barHeight), true);
      }
      char value[16];
      compactDuration(chart.values[i], value, sizeof(value));
      const auto style = hot ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      int valueWidth = renderer.getTextWidth(SMALL_FONT_ID, value, style);
      if (valueWidth > slot - 6) {
        // Crowded: drop the unit, the axis title already implies it.
        char* space = strchr(value, ' ');
        if (space) *space = '\0';
        valueWidth = renderer.getTextWidth(SMALL_FONT_ID, value, style);
      }
      if (valueWidth <= slot) {
        const int valueY = barTop - renderer.getLineHeight(SMALL_FONT_ID) - 3;
        renderer.drawText(SMALL_FONT_ID, cx - valueWidth / 2, valueY, value, true, style);
      }
    } else {
      // A faint stub keeps empty slots readable as "zero", not "missing".
      income_gfx::fillDither(renderer, barX, baseline - 2, barWidth, 2, 4);
    }
    const auto style = hot ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int labelWidth = renderer.getTextWidth(SMALL_FONT_ID, chart.labels[i], style);
    renderer.drawText(SMALL_FONT_ID, cx - labelWidth / 2, baseline + 6, chart.labels[i], true, style);
  }
  renderer.fillRect(x, baseline, width, 2, true);
}
}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  summary = makeUniqueNoThrow<reading_stats::Summary>();
  if (!summary) {
    LOG_ERR("RSA", "OOM: stats summary");
  } else if (!reading_stats::loadSummary(*summary)) {
    summary.reset();
  }
  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();
  summary.reset();
}

bool ReadingStatsActivity::handleHomeGesture() {
  finish();
  return true;
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

int ReadingStatsActivity::drawHero(const int x, const int y, const int width) const {
  const auto& s = *summary;
  // Today as the big pixel number; week, streak and total stacked beside it.
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, tr(STR_STATS_TODAY), TRACKING);
  char today[12];
  clockDuration(s.todaySec, today, sizeof(today));
  const int heroWidth = width / 2;
  const int rightX = x + heroWidth + 8;
  const int rightWidth = width - heroWidth - 8;
  // A wide right column puts each label and value on one line (portrait),
  // which keeps the hero short and leaves the charts more height.
  const bool inlineRows = rightWidth >= 230;
  int pixelSize = inlineRows ? 7 : 9;
  while (pixelSize > 4 && income_gfx::pixelTextWidth(today, pixelSize) > heroWidth - 12) pixelSize--;
  income_gfx::drawPixelText(renderer, x, y + 26, today, pixelSize);
  const int bottom = y + 26 + pixelSize * 7 + pixelSize / 2;

  char value[24];
  int rowY = y;
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  const auto row = [&](const char* label, const char* text) {
    if (inlineRows) {
      const int labelOffset = (valueH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, rightX, rowY + labelOffset, label, TRACKING);
      const int w = renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, rightX + rightWidth - w, rowY, text, true, EpdFontFamily::BOLD);
      rowY += valueH + 8;
      return;
    }
    income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, rightX, rowY, label, TRACKING);
    const std::string fitted = renderer.truncatedText(UI_12_FONT_ID, text, rightWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rightX, rowY + 18, fitted.c_str(), true, EpdFontFamily::BOLD);
    rowY += 18 + valueH + 6;
  };
  reading_stats::formatDuration(s.weekSec, value, sizeof(value));
  row(tr(STR_STATS_THIS_WEEK), value);
  if (s.streakDays == 1) {
    snprintf(value, sizeof(value), "%s", tr(STR_STATS_ONE_DAY));
  } else {
    snprintf(value, sizeof(value), tr(STR_STATS_DAYS), static_cast<unsigned>(s.streakDays));
  }
  row(tr(STR_STATS_STREAK), value);
  reading_stats::formatDuration(s.totalSec, value, sizeof(value));
  row(tr(STR_STATS_TOTAL), value);
  return std::max(bottom, rowY);
}

int ReadingStatsActivity::drawInsight(const int x, const int y, const int width) const {
  const auto& s = *summary;
  int lineY = y;
  char line[96];
  if (!s.clockValid) {
    snprintf(line, sizeof(line), "%s", tr(STR_STATS_NO_CLOCK));
  } else if (s.bestWeekday >= 0) {
    snprintf(line, sizeof(line), tr(STR_STATS_MOST_ON), I18N.get(WEEKDAY_PLURAL[s.bestWeekday]));
  } else {
    line[0] = '\0';
  }
  if (line[0] != '\0') {
    // Solid square bullet, like a chart legend swatch; a narrow column steps down a font size.
    const int font =
        renderer.getTextWidth(UI_12_FONT_ID, line, EpdFontFamily::BOLD) <= width - 18 ? UI_12_FONT_ID : UI_10_FONT_ID;
    const int lineHeight = renderer.getLineHeight(font);
    renderer.fillRect(x, lineY + lineHeight / 2 - 5, 10, 10, true);
    const std::string fitted = renderer.truncatedText(font, line, width - 18, EpdFontFamily::BOLD);
    renderer.drawText(font, x + 18, lineY, fitted.c_str(), true, EpdFontFamily::BOLD);
    lineY += lineHeight + 2;
  }
  snprintf(line, sizeof(line), tr(STR_STATS_BOOKS_LINE), static_cast<unsigned>(s.booksStarted),
           static_cast<unsigned>(s.booksFinished));
  renderer.drawText(SMALL_FONT_ID, x + 18, lineY, line);
  return lineY + renderer.getLineHeight(SMALL_FONT_ID);
}

void ReadingStatsActivity::drawWeekdayChart(const int x, const int y, const int width, const int height) const {
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, tr(STR_STATS_BY_WEEKDAY), TRACKING);
  const char* labels[7];
  for (int i = 0; i < 7; i++) labels[i] = I18N.get(WEEKDAY_SHORT[i]);
  const BarChart chart{summary->weekdayAvgSec, 7, summary->bestWeekday, labels};
  drawBarChart(renderer, chart, x, y + LABEL_H, width, height - LABEL_H);
}

void ReadingStatsActivity::drawWeeksChart(const int x, const int y, const int width, const int height) const {
  constexpr int WEEKS = reading_stats::Summary::WEEKS;
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, tr(STR_STATS_LAST_WEEKS), TRACKING);
  // Axis labels: each week's Monday as day/month.
  char labelBuf[WEEKS][8];
  const char* labels[WEEKS];
  const int32_t today = reading_stats::today();
  for (int i = 0; i < WEEKS; i++) {
    labelBuf[i][0] = '\0';
    if (today != INT32_MIN) {
      const int32_t monday = today - reading_stats::weekdayMondayFirst(today) - 7 * (WEEKS - 1 - i);
      const time_t t = static_cast<time_t>(monday) * 86400;
      struct tm utc;
      gmtime_r(&t, &utc);
      snprintf(labelBuf[i], sizeof(labelBuf[i]), "%d/%d", utc.tm_mday, utc.tm_mon + 1);
    }
    labels[i] = labelBuf[i];
  }
  const BarChart chart{summary->weekTotals, WEEKS, WEEKS - 1, labels};
  drawBarChart(renderer, chart, x, y + LABEL_H, width, height - LABEL_H);
}

void ReadingStatsActivity::drawTopBooks(const int x, const int y, const int width, const int height) const {
  const auto& s = *summary;
  if (s.topCount == 0 || height < LABEL_H + 30) return;
  income_gfx::drawTrackedLabel(renderer, UI_10_FONT_ID, x, y, tr(STR_STATS_TOP_BOOKS), TRACKING);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowHeight = lineHeight + 12;
  const int rows = std::min<int>(s.topCount, (height - LABEL_H) / rowHeight);
  const uint32_t maxSec = std::max<uint32_t>(1, s.top[0].sec);
  int rowY = y + LABEL_H + 4;
  for (int i = 0; i < rows; i++) {
    const auto& book = s.top[i];
    char time[24];
    reading_stats::formatDuration(book.sec, time, sizeof(time));
    const int timeWidth = renderer.getTextWidth(UI_10_FONT_ID, time, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, x + width - timeWidth, rowY, time, true, EpdFontFamily::BOLD);

    char title[128];
    bookTitle(book.path, title, sizeof(title));
    const int checkWidth = book.finished ? 18 : 0;
    const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, title, width - timeWidth - 16 - checkWidth);
    renderer.drawText(UI_10_FONT_ID, x + checkWidth, rowY, fitted.c_str());
    if (book.finished) {
      // Finished marker: a bold check mark.
      const int cy = rowY + lineHeight / 2;
      renderer.drawLine(x, cy, x + 4, cy + 4, 3, true);
      renderer.drawLine(x + 4, cy + 4, x + 12, cy - 5, 3, true);
    }
    // Proportional dithered track under the title.
    const int barY = rowY + lineHeight + 2;
    const int barWidth = static_cast<int>(static_cast<uint64_t>(book.sec) * (width - timeWidth - 16) / maxSec);
    income_gfx::fillDither(renderer, x, barY, std::max(2, barWidth), 4, i == 0 ? 16 : 8);
    rowY += rowHeight;
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_READING_STATS));

  const int top = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int left = safe.x + MARGIN;
  const int width = safe.width - MARGIN * 2;
  const int bottom = safe.y + safe.height - MARGIN / 2;

  if (!summary || (summary->totalSec == 0 && summary->weekTotals[reading_stats::Summary::WEEKS - 1] == 0)) {
    renderer.drawCenteredText(UI_12_FONT_ID, (top + bottom) / 2 - 10, tr(STR_STATS_EMPTY), true, EpdFontFamily::BOLD);
  } else if (safe.width > safe.height) {
    // Landscape: numbers and books on the left, charts on the right.
    const int colGap = 28;
    const int leftWidth = (width - colGap) * 9 / 20;
    const int rightX = left + leftWidth + colGap;
    const int rightWidth = width - leftWidth - colGap;
    int y = drawHero(left, top, leftWidth) + SECTION_GAP;
    y = drawInsight(left, y, leftWidth) + SECTION_GAP;
    drawTopBooks(left, y, leftWidth, bottom - y);
    const int chartHeight = (bottom - top - SECTION_GAP) / 2;
    drawWeekdayChart(rightX, top, rightWidth, chartHeight);
    drawWeeksChart(rightX, top + chartHeight + SECTION_GAP, rightWidth, chartHeight);
  } else {
    int y = drawHero(left, top, width) + SECTION_GAP;
    y = drawInsight(left, y, width) + SECTION_GAP;
    // Leave the most-read list room for three books.
    const int booksHeight = LABEL_H + 4 + 3 * (renderer.getLineHeight(UI_10_FONT_ID) + 12);
    const int chartHeight = std::min(150, (bottom - y - booksHeight - 2 * SECTION_GAP) / 2);
    drawWeekdayChart(left, y, width, chartHeight);
    y += chartHeight + SECTION_GAP;
    drawWeeksChart(left, y, width, chartHeight);
    y += chartHeight + SECTION_GAP;
    drawTopBooks(left, y, width, bottom - y);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
