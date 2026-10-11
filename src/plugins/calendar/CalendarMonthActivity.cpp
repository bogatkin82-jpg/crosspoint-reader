#include "CalendarMonthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CalendarActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace plugins::calendar {

namespace {
constexpr StrId WEEKDAY[] = {StrId::STR_CAL_WD_1, StrId::STR_CAL_WD_2, StrId::STR_CAL_WD_3, StrId::STR_CAL_WD_4,
                             StrId::STR_CAL_WD_5, StrId::STR_CAL_WD_6, StrId::STR_CAL_WD_7};
constexpr int ROWS = 6;
constexpr int COLS = 7;
constexpr size_t MAX_LIST = 8;
}  // namespace

void CalendarMonthActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

CalendarMonthActivity::Grid CalendarMonthActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const int bottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  // Grid takes up to ~60% of the free band, the rest lists the selected day.
  const int band = bottom - top - labelH;
  Grid g;
  g.cellW = (width - 2 * side) / COLS;
  g.cellH = std::max(renderer.getLineHeight(UI_12_FONT_ID) + 10, std::min(g.cellW, band * 6 / 10 / ROWS));
  g.x = (width - g.cellW * COLS) / 2;
  g.y = top + labelH;

  int y;
  unsigned m, d;
  ics::civilFromDays(selected, y, m, d);
  const int32_t first = ics::daysFromCivil(y, m, 1);
  g.firstDay = ics::mondayOf(first);
  return g;
}

void CalendarMonthActivity::move(int32_t delta) {
  selected += delta;
  requestUpdate();
}

void CalendarMonthActivity::moveMonths(int delta) {
  int y;
  unsigned m, d;
  ics::civilFromDays(selected, y, m, d);
  int mm = static_cast<int>(m) + delta;
  while (mm < 1) {
    mm += 12;
    y--;
  }
  while (mm > 12) {
    mm -= 12;
    y++;
  }
  const unsigned dim = static_cast<unsigned>(ics::daysInMonth(y, static_cast<unsigned>(mm)));
  selected = ics::daysFromCivil(y, static_cast<unsigned>(mm), std::min(d, dim));
  requestUpdate();
}

void CalendarMonthActivity::pick() {
  if (pickedDay) *pickedDay = selected;
  setResult(ActivityResult{});
  finish();
}

void CalendarMonthActivity::loop() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasReleased(B::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm)) {
    pick();
    return;
  }
  if (mappedInput.wasPressed(B::Left)) return move(-1);
  if (mappedInput.wasPressed(B::Right)) return move(1);
  if (mappedInput.wasPressed(B::Up)) return move(-7);
  if (mappedInput.wasPressed(B::Down)) return move(7);
  if (mappedInput.wasPressed(B::PageBack)) return moveMonths(-1);
  if (mappedInput.wasPressed(B::PageForward)) return moveMonths(1);

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) return moveMonths(1);
  if (swipe == MappedInputManager::SwipeDir::Right) return moveMonths(-1);

  int tx, ty;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Grid g = layout();
    if (tx >= g.x && tx < g.x + g.cellW * COLS && ty >= g.y && ty < g.y + g.cellH * ROWS) {
      const int32_t day = g.firstDay + ((ty - g.y) / g.cellH) * COLS + (tx - g.x) / g.cellW;
      if (day == selected) {
        pick();
      } else {
        selected = day;
        requestUpdate();
      }
    }
  }
}

void CalendarMonthActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  renderer.clearScreen();

  int year;
  unsigned month, dom;
  ics::civilFromDays(selected, year, month, dom);
  const std::string title = formatMonthTitle(year, month);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title.c_str());

  const Grid g = layout();
  const int labelY = g.y - renderer.getLineHeight(SMALL_FONT_ID) - 4;
  for (int c = 0; c < COLS; c++) {
    const char* wd = I18N.get(WEEKDAY[c]);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, wd);
    renderer.drawText(SMALL_FONT_ID, g.x + c * g.cellW + (g.cellW - tw) / 2, labelY, wd, true,
                      c >= 5 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
  renderer.drawLine(g.x, g.y - 2, g.x + g.cellW * COLS, g.y - 2);

  const int numH = renderer.getLineHeight(UI_12_FONT_ID);
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      const int32_t day = g.firstDay + r * COLS + c;
      int y;
      unsigned m, d;
      ics::civilFromDays(day, y, m, d);
      if (m != month) continue;  // only this month's days are drawn
      const int cx = g.x + c * g.cellW;
      const int cy = g.y + r * g.cellH;
      const bool isToday = day == today;
      if (isToday) renderer.fillRect(cx + 3, cy + 3, g.cellW - 6, g.cellH - 6);
      if (day == selected) renderer.drawRect(cx + 1, cy + 1, g.cellW - 2, g.cellH - 2, 3, true);

      char num[4];
      snprintf(num, sizeof(num), "%u", d);
      const int tw = renderer.getTextWidth(UI_12_FONT_ID, num, EpdFontFamily::BOLD);
      const int ty = cy + (g.cellH - numH) / 2 - 3;
      renderer.drawText(UI_12_FONT_ID, cx + (g.cellW - tw) / 2, ty, num, !isToday,
                        c >= 5 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      if (cal.hasEventsOn(day)) {
        const int barW = std::max(8, g.cellW / 3);
        renderer.fillRect(cx + (g.cellW - barW) / 2, ty + numH + 1, barW, 3, !isToday);
      }
    }
  }

  // Events of the selected day.
  int y = g.y + g.cellH * ROWS + metrics.verticalSpacing;
  const int side = metrics.contentSidePadding;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const std::string heading = formatDayHeading(selected, today);
  renderer.drawText(UI_10_FONT_ID, side, y, heading.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;

  ics::Occurrence occ[MAX_LIST];
  const size_t n = cal.occurrencesOn(selected, occ, MAX_LIST);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  std::string summary;
  if (n == 0) {
    renderer.drawText(UI_10_FONT_ID, side, y, tr(STR_CAL_NO_EVENTS));
  }
  for (size_t i = 0; i < n && y + lineH <= bottom; i++) {
    const ics::Event& ev = cal.events()[occ[i].event];
    std::string line = occ[i].continued || ev.startMin < 0 ? std::string(tr(STR_CAL_ALL_DAY)) : formatTime(ev.startMin);
    line += "  ";
    data.eventText(ev, &summary, nullptr);
    line += summary.empty() ? tr(STR_CAL_UNTITLED) : summary.c_str();
    const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), width - 2 * side);
    renderer.drawText(UI_10_FONT_ID, side, y, fitted.c_str());
    y += lineH;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

}  // namespace plugins::calendar
