#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CalendarMonthActivity.h"
#include "CalendarSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace calendar {

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr size_t MAX_AGENDA_ROWS = 150;  // ~100 B of row text each on the C3
constexpr size_t MAX_OCC_PER_DAY = 24;

constexpr StrId MONTH_NOM[] = {StrId::STR_CAL_MONTH_1, StrId::STR_CAL_MONTH_2,  StrId::STR_CAL_MONTH_3,
                               StrId::STR_CAL_MONTH_4, StrId::STR_CAL_MONTH_5,  StrId::STR_CAL_MONTH_6,
                               StrId::STR_CAL_MONTH_7, StrId::STR_CAL_MONTH_8,  StrId::STR_CAL_MONTH_9,
                               StrId::STR_CAL_MONTH_10, StrId::STR_CAL_MONTH_11, StrId::STR_CAL_MONTH_12};
constexpr StrId MONTH_GEN[] = {StrId::STR_CAL_MONTH_GEN_1,  StrId::STR_CAL_MONTH_GEN_2,  StrId::STR_CAL_MONTH_GEN_3,
                               StrId::STR_CAL_MONTH_GEN_4,  StrId::STR_CAL_MONTH_GEN_5,  StrId::STR_CAL_MONTH_GEN_6,
                               StrId::STR_CAL_MONTH_GEN_7,  StrId::STR_CAL_MONTH_GEN_8,  StrId::STR_CAL_MONTH_GEN_9,
                               StrId::STR_CAL_MONTH_GEN_10, StrId::STR_CAL_MONTH_GEN_11, StrId::STR_CAL_MONTH_GEN_12};
constexpr StrId WEEKDAY[] = {StrId::STR_CAL_WD_1, StrId::STR_CAL_WD_2, StrId::STR_CAL_WD_3, StrId::STR_CAL_WD_4,
                             StrId::STR_CAL_WD_5, StrId::STR_CAL_WD_6, StrId::STR_CAL_WD_7};
constexpr StrId TAB_NAMES[] = {StrId::STR_CAL_AGENDA, StrId::STR_CAL_TASKS};
}  // namespace

std::string formatShortDate(int32_t day) {
  int y;
  unsigned m, d;
  ics::civilFromDays(day, y, m, d);
  char buf[48];
  snprintf(buf, sizeof(buf), "%u %s", d, I18N.get(MONTH_GEN[m - 1]));
  return buf;
}

std::string formatDayHeading(int32_t day, int32_t today) {
  std::string s = I18N.get(WEEKDAY[ics::weekdayMon0(day)]);
  s += ", ";
  s += formatShortDate(day);
  if (day == today) {
    s += " - ";
    s += tr(STR_CAL_TODAY);
  } else if (day == today + 1) {
    s += " - ";
    s += tr(STR_CAL_TOMORROW);
  }
  return s;
}

std::string formatMonthTitle(int year, unsigned month) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %d", I18N.get(MONTH_NOM[month - 1]), year);
  return buf;
}

std::string formatTime(int16_t minute) {
  if (minute < 0) return tr(STR_CAL_ALL_DAY);
  char buf[12];
  const int h = minute / 60, m = minute % 60;
  if (SETTINGS.clockFormat == 1) {
    snprintf(buf, sizeof(buf), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  }
  return buf;
}

CalendarActivity::CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Config& config)
    : UiTabListActivity("Calendar", renderer, mappedInput, true), config(config) {
  tabPillMaxPad = 16;
}

void CalendarActivity::onEnter() {
  UiTabListActivity::onEnter();
  clockSet = localToday(today);
  periodStart = today;
  data = makeUniqueNoThrow<CalendarData>();
  if (!data) {
    LOG_ERR("CAL", "OOM: calendar data");
    finish();
    return;
  }
  reload();
  for (auto& n : tabNavs) n.selected = 1;

  const int hours = data->autoSyncHours();
  const int64_t now = nowUtcEpoch();
  const int64_t lastAttempt = readLastAttempt(config.dir);
  const int64_t lastTouch = lastAttempt > lastSync ? lastAttempt : lastSync;
  autoSyncPending = hours > 0 && !data->subscriptions().empty() && now > 0 &&
                    now - lastTouch >= static_cast<int64_t>(hours) * 3600;
}

void CalendarActivity::onExit() {
  // ActivityManager already holds the render lock around onExit().
  rowItems.clear();
  texts.clear();
  rows.clear();
  data.reset();
  UiTabListActivity::onExit();
}

void CalendarActivity::reload() {
  {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_CAL_LOADING));
  }
  // Parsing touches the SD card and must not block the render task.
  data->load(config, today);
  {
    RenderLock lock;
    lastSync = readLastSync(config.dir);
    rebuildRowsLocked();
  }
  requestUpdate();
}

void CalendarActivity::setPeriodStart(int32_t day) {
  periodStart = day;
  tab = Tab::Agenda;
  rebuildRows();
  auto& n = activeNav();
  n.selected = 1;
  n.followOnBuild = true;
  requestUpdate();
}

const char* CalendarActivity::tabLabel(const int index) const { return I18N.get(TAB_NAMES[index]); }

void CalendarActivity::onTabAction(const int index) {
  if (popup.isActive()) return;
  if (static_cast<int>(tab) != index) {
    tab = static_cast<Tab>(index);
    rebuildRows();
    auto& n = activeNav();
    n.selected = 0;
    n.followOnBuild = true;
    requestUpdate();
  }
  app.clearTapFlash();
}

void CalendarActivity::stepTab(const int direction) {
  const int count = static_cast<int>(Tab::Count);
  tab = static_cast<Tab>((static_cast<int>(tab) + direction + count) % count);
  rebuildRows();
  activeNav().followOnBuild = true;
  requestUpdate();
}

void CalendarActivity::rebuildRows() {
  RenderLock lock;
  rebuildRowsLocked();
}

void CalendarActivity::rebuildRowsLocked() {
  rows.clear();
  texts.clear();
  if (tab == Tab::Agenda) {
    buildAgendaRows();
  } else {
    buildTaskRows();
  }
  finishRows();
}

void CalendarActivity::buildAgendaRows() {
  const ics::Calendar& cal = data->calendar();
  const int days = data->daysAhead();
  // ListItem pointers are taken after texts is final; reserve the row cap.
  rows.reserve(MAX_AGENDA_ROWS + 2);
  texts.reserve(MAX_AGENDA_ROWS + 2);

  // Period row.
  {
    RowText t;
    t.label = formatShortDate(periodStart) + " - " + formatShortDate(periodStart + days - 1);
    t.value = tr(STR_CAL_GO_TO);
    if (!clockSet) {
      t.subtitle = tr(STR_CAL_CLOCK_NOT_SET);
    } else if (!data->subscriptions().empty()) {
      t.subtitle = tr(STR_CAL_LAST_SYNC);
      t.subtitle += ": ";
      if (lastSync > 0) {
        int32_t d;
        int16_t m;
        const time_t ts = static_cast<time_t>(lastSync);
        struct tm lt {};
        localtime_r(&ts, &lt);
        d = ics::daysFromCivil(lt.tm_year + 1900, static_cast<unsigned>(lt.tm_mon + 1), static_cast<unsigned>(lt.tm_mday));
        m = static_cast<int16_t>(lt.tm_hour * 60 + lt.tm_min);
        t.subtitle += formatShortDate(d) + ", " + formatTime(m);
      } else {
        t.subtitle += tr(STR_CAL_NEVER);
      }
    }
    if (cal.truncated()) {
      if (!t.subtitle.empty()) t.subtitle += ". ";
      t.subtitle += tr(STR_CAL_TRUNCATED);
    }
    rows.push_back({RowKind::Period, 0, periodStart});
    texts.push_back(std::move(t));
  }

  ics::Occurrence occ[MAX_OCC_PER_DAY];
  std::string summary, location;
  bool any = false;
  for (int i = 0; i < days && rows.size() < MAX_AGENDA_ROWS; i++) {
    const int32_t day = periodStart + i;
    const size_t n = cal.occurrencesOn(day, occ, MAX_OCC_PER_DAY);
    for (size_t k = 0; k < n && rows.size() < MAX_AGENDA_ROWS; k++) {
      const ics::Event& ev = cal.events()[occ[k].event];
      RowText t;
      if (k == 0) t.heading = formatDayHeading(day, today);
      data->eventText(ev, &summary, &location);
      t.label = summary.empty() ? tr(STR_CAL_UNTITLED) : summary;
      if (occ[k].continued || ev.startMin < 0) {
        t.value = tr(STR_CAL_ALL_DAY);
        t.subtitle = location;
      } else {
        t.value = formatTime(ev.startMin);
        if (ev.endMin >= 0 && (ev.endMin != ev.startMin || ev.endDay != ev.startDay)) {
          t.subtitle = formatTime(ev.startMin) + " - " + formatTime(ev.endMin);
          if (!location.empty()) t.subtitle += ", ";
        }
        t.subtitle += location;
      }
      rows.push_back({RowKind::Event, static_cast<int16_t>(occ[k].event), day});
      texts.push_back(std::move(t));
      any = true;
    }
  }
  if (!any) {
    RowText t;
    t.label = tr(STR_CAL_NO_EVENTS);
    rows.push_back({RowKind::Empty, 0, 0});
    texts.push_back(std::move(t));
  }
  if (cal.truncated() || rows.size() >= MAX_AGENDA_ROWS) {
    RowText t;
    t.label = tr(STR_CAL_TRUNCATED);
    rows.push_back({RowKind::Info, 0, 0});
    texts.push_back(std::move(t));
  }
}

void CalendarActivity::buildTaskRows() {
  const ics::Calendar& cal = data->calendar();
  const auto& todos = cal.todos();
  rows.reserve(MAX_AGENDA_ROWS + 2);
  texts.reserve(MAX_AGENDA_ROWS + 2);

  {
    RowText t;
    t.label = std::string("+ ") + tr(STR_CAL_NEW_TASK);
    rows.push_back({RowKind::AddTask, 0, 0});
    texts.push_back(std::move(t));
  }
  {
    RowText t;
    t.label = tr(STR_CAL_SHOW_DONE);
    rows.push_back({RowKind::ShowDone, 0, 0});
    texts.push_back(std::move(t));
  }
  size_t shown = 0;
  for (size_t i = 0; i < todos.size() && shown < MAX_AGENDA_ROWS; i++) {
    const ics::Todo& todo = todos[i];
    if (todo.completed && !showDone) continue;
    shown++;
    RowText t;
    t.label = *cal.str(todo.summary) ? cal.str(todo.summary) : "-";
    if (todo.dueDay != ics::NO_DAY) {
      t.subtitle = std::string(tr(STR_CAL_DUE)) + ": ";
      if (todo.dueDay == today) {
        t.subtitle += tr(STR_CAL_TODAY);
      } else if (todo.dueDay == today + 1) {
        t.subtitle += tr(STR_CAL_TOMORROW);
      } else {
        t.subtitle += formatShortDate(todo.dueDay);
      }
      if (todo.dueMin >= 0) t.subtitle += ", " + formatTime(todo.dueMin);
      if (!todo.completed && todo.dueDay < today) {
        t.subtitle += " (";
        t.subtitle += tr(STR_CAL_OVERDUE);
        t.subtitle += ")";
      }
    }
    if (todo.priority >= 1 && todo.priority <= 4) t.subtitle += t.subtitle.empty() ? "!" : "  !";
    rows.push_back({RowKind::Task, static_cast<int16_t>(i), 0});
    texts.push_back(std::move(t));
  }
  if (cal.truncated()) {
    RowText t;
    t.label = tr(STR_CAL_TRUNCATED);
    rows.push_back({RowKind::Info, 0, 0});
    texts.push_back(std::move(t));
  }
}

// Second pass: texts is final, so the ListItem pointers stay valid.
void CalendarActivity::finishRows() {
  const ics::Calendar& cal = data->calendar();
  rowItems.clear();
  rowItems.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    fui::ListItem item;
    const RowText& t = texts[i];
    item.label = t.label.c_str();
    item.value = t.value.empty() ? nullptr : t.value.c_str();
    item.subtitle = t.subtitle.empty() ? nullptr : t.subtitle.c_str();
    item.sectionHeading = t.heading.empty() ? nullptr : t.heading.c_str();
    item.actionValue = static_cast<int16_t>(i);
    switch (rows[i].kind) {
      case RowKind::Empty:
      case RowKind::Info:
        item.enabled = false;
        break;
      case RowKind::ShowDone:
        item.toggle = true;
        item.toggleChecked = showDone;
        break;
      case RowKind::Task:
        item.toggle = true;
        item.toggleChecked = cal.todos()[rows[i].index].completed;
        break;
      default:
        break;
    }
    rowItems.push_back(item);
  }
  // Keep each tab's selection inside the new row range.
  for (auto& n : tabNavs)
    if (n.selected > listCount()) n.selected = listCount();
}

void CalendarActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  buildTabBar(screen);
  if (rowItems.empty()) {
    // reload() parses outside the render lock; paint the placeholder instead
    // of an empty list if a repaint lands in that window.
    screen.centeredText(tr(STR_CAL_LOADING), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void CalendarActivity::drawFooter() {
  const char* confirm = tr(STR_SELECT);
  const int row = ringPos() - 1;
  if (ringPos() == 0) {
    confirm = tabLabel((static_cast<int>(tab) + 1) % static_cast<int>(Tab::Count));
  } else if (row >= 0 && row < static_cast<int>(rows.size()) &&
             (rows[row].kind == RowKind::Task || rows[row].kind == RowKind::ShowDone)) {
    confirm = tr(STR_TOGGLE);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

bool CalendarActivity::handleCustomInput() {
  if (autoSyncPending && !popup.isActive()) {
    autoSyncPending = false;
    startSync();
    return true;
  }
  if (deferredMessage && !popup.isActive()) {
    const char* options[] = {tr(STR_BACK)};
    popup.show(config.name.empty() ? tr(STR_CAL_TITLE) : config.name.c_str(), deferredMessage, options, 1, 0,
               [](int) {});
    deferredMessage = nullptr;
    requestUpdate();
    return true;
  }
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool CalendarActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    if (ringPos() > 0) onRowLongPress(ringPos() - 1);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // The calendar is the stack root (opened with replaceActivity), so finish()
    // would land on Home with no row selected. Highlight its own row instead.
    onGoHome(HomeMenuItem::CALENDAR);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ringPos() == 0) {
      stepTab(1);
    } else {
      activateIndex(ringPos() - 1);
    }
    return true;
  }
  return false;
}

void CalendarActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= static_cast<int>(rows.size())) return;
  app.clearTapFlash();
  const RowRef row = rows[index];
  switch (row.kind) {
    case RowKind::Period:
      showPeriodMenu();
      break;
    case RowKind::Event:
      showEventDetails(row);
      break;
    case RowKind::AddTask:
      startAddTask();
      break;
    case RowKind::ShowDone:
      showDone = !showDone;
      rebuildRows();
      requestUpdate();
      break;
    case RowKind::Task:
      toggleTask(row.index);
      break;
    default:
      break;
  }
}

void CalendarActivity::onRowLongPress(const int index) {
  if (popup.isActive() || index < 0 || index >= static_cast<int>(rows.size())) return;
  app.clearTapFlash();
  if (rows[index].kind == RowKind::Task) {
    showTaskMenu(rows[index].index);
  } else {
    activateIndex(index);
  }
}

void CalendarActivity::showPeriodMenu() {
  const char* options[] = {tr(STR_CAL_TODAY),      tr(STR_CAL_MONTH_VIEW), tr(STR_CAL_PREV_WEEK), tr(STR_CAL_NEXT_WEEK),
                           tr(STR_CAL_PREV_MONTH), tr(STR_CAL_NEXT_MONTH), tr(STR_CAL_RELOAD),  tr(STR_CAL_SYNC)};
  const int count = data->subscriptions().empty() ? 7 : 8;
  popup.show(tr(STR_CAL_GO_TO), options, count, 0, [this](int idx) {
    int y;
    unsigned m, d;
    switch (idx) {
      case 0:
        clockSet = localToday(today);
        setPeriodStart(today);
        break;
      case 1:
        openMonthView();
        break;
      case 2:
        setPeriodStart(periodStart - 7);
        break;
      case 3:
        setPeriodStart(periodStart + 7);
        break;
      case 4:
      case 5: {
        ics::civilFromDays(periodStart, y, m, d);
        int mm = static_cast<int>(m) + (idx == 4 ? -1 : 1);
        if (mm < 1) {
          mm = 12;
          y--;
        } else if (mm > 12) {
          mm = 1;
          y++;
        }
        const int dim = ics::daysInMonth(y, static_cast<unsigned>(mm));
        setPeriodStart(ics::daysFromCivil(y, static_cast<unsigned>(mm), std::min<unsigned>(d, dim)));
        break;
      }
      case 6:
        clockSet = localToday(today);
        reload();
        requestUpdate();
        break;
      case 7:
        startSync();
        break;
      default:
        break;
    }
  });
  requestUpdate();
}

void CalendarActivity::startSync() {
  std::vector<Subscription> subs = data->subscriptions();
  if (subs.empty()) {
    showMessage(tr(STR_CAL_SYNC_NO_URL));
    return;
  }
  auto sync = makeUniqueNoThrow<CalendarSyncActivity>(renderer, mappedInput, config, std::move(subs));
  if (!sync) {
    LOG_ERR("CAL", "OOM: sync activity");
    return;
  }
  // TLS needs ~40 KB of contiguous heap: drop the parsed calendar while syncing.
  {
    RenderLock lock;
    rows.clear();
    texts.clear();
    rowItems.clear();
    data->releaseMemory();
  }
  startActivityForResult(std::move(sync), [this](const ActivityResult&) {
    clockSet = localToday(today);
    reload();
  });
}

void CalendarActivity::openMonthView() {
  monthPick = periodStart;
  auto month = makeUniqueNoThrow<CalendarMonthActivity>(renderer, mappedInput, *data, today, periodStart, &monthPick);
  if (!month) {
    LOG_ERR("CAL", "OOM: month view");
    return;
  }
  startActivityForResult(std::move(month), [this](const ActivityResult& result) {
    if (!result.isCancelled) setPeriodStart(monthPick);
  });
}

void CalendarActivity::showEventDetails(const RowRef& row) {
  const ics::Calendar& cal = data->calendar();
  const ics::Event& ev = cal.events()[row.index];
  std::string summary, location;
  data->eventText(ev, &summary, &location);
  std::string text = summary.empty() ? std::string(tr(STR_CAL_UNTITLED)) : summary;
  text += "\n";
  text += formatDayHeading(row.day, today);
  if (ev.startMin >= 0) {
    text += ", " + formatTime(ev.startMin);
    if (ev.endMin >= 0) text += " - " + formatTime(ev.endMin);
    if (ev.endDay != ev.startDay) text += " (" + formatShortDate(row.day + (ev.endDay - ev.startDay)) + ")";
  } else if (ev.endDay != ev.startDay) {
    text += " - " + formatShortDate(row.day + (ev.endDay - ev.startDay));
  }
  if (!location.empty()) {
    text += "\n";
    text += location;
  }
  const char* options[] = {tr(STR_BACK)};
  popup.show(tr(STR_CAL_EVENT), text.c_str(), options, 1, 0, [](int) {});
  requestUpdate();
}

void CalendarActivity::showMessage(const char* text) {
  deferredMessage = text;
  requestUpdate();
}

void CalendarActivity::toggleTask(const int todoIndex) {
  const auto& todos = data->calendar().todos();
  if (todoIndex < 0 || todoIndex >= static_cast<int>(todos.size())) return;
  const auto result = data->setTodoDone(static_cast<size_t>(todoIndex), !todos[todoIndex].completed);
  if (result == CalendarData::EditResult::ReadOnly) {
    showMessage(tr(STR_CAL_READ_ONLY));
    return;
  }
  if (result == CalendarData::EditResult::Failed) {
    showMessage(tr(STR_CAL_SAVE_FAILED));
    return;
  }
  rebuildRows();
  requestUpdate();
}

void CalendarActivity::deleteTask(const int todoIndex) {
  const auto result = data->deleteTodo(static_cast<size_t>(todoIndex));
  if (result == CalendarData::EditResult::ReadOnly) {
    showMessage(tr(STR_CAL_READ_ONLY));
    return;
  }
  if (result == CalendarData::EditResult::Failed) {
    showMessage(tr(STR_CAL_SAVE_FAILED));
    return;
  }
  rebuildRows();
  requestUpdate();
}

void CalendarActivity::showTaskMenu(const int todoIndex) {
  const auto& cal = data->calendar();
  if (todoIndex < 0 || todoIndex >= static_cast<int>(cal.todos().size())) return;
  const ics::Todo& todo = cal.todos()[todoIndex];
  const char* options[] = {todo.completed ? tr(STR_CAL_MARK_UNDONE) : tr(STR_CAL_MARK_DONE),
                           tr(STR_CAL_DELETE_TASK)};
  popup.show(tr(STR_CAL_TASKS), cal.str(todo.summary), options, 2, 0, [this, todoIndex](int idx) {
    if (idx == 0) toggleTask(todoIndex);
    if (idx == 1) deleteTask(todoIndex);
  });
  requestUpdate();
}

void CalendarActivity::startAddTask() {
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CAL_NEW_TASK), "", 200, InputType::Text);
  if (!keyboard) {
    LOG_ERR("CAL", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* kb = std::get_if<KeyboardResult>(&result.data);
    if (!kb || kb->text.empty()) return;
    pendingSummary = kb->text;
    const char* options[] = {tr(STR_CAL_NO_DUE), tr(STR_CAL_TODAY), tr(STR_CAL_TOMORROW), tr(STR_CAL_IN_3_DAYS),
                             tr(STR_CAL_IN_WEEK)};
    popup.show(tr(STR_CAL_DUE), pendingSummary.c_str(), options, 5, 0, [this](int idx) {
      static constexpr int OFFSETS[] = {0, 0, 1, 3, 7};
      const int32_t due = idx == 0 ? ics::NO_DAY : today + OFFSETS[idx];
      if (!data->addTodo(pendingSummary, due)) {
        showMessage(tr(STR_CAL_SAVE_FAILED));
        return;
      }
      pendingSummary.clear();
      rebuildRows();
      requestUpdate();
    });
  });
}

}  // namespace calendar
