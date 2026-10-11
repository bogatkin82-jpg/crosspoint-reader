#pragma once

#include <memory>
#include <string>
#include <vector>

#include "CalendarPlugin.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

namespace plugins::calendar {

// Calendar plugin main screen: "Events" tab (agenda grouped by day for the
// configured period, with a period row that jumps by week/month or opens the
// month grid) and "Tasks" tab (VTODO checklist; Confirm toggles completion,
// long-press offers delete; new tasks are appended to the tasks .ics file).
class CalendarActivity final : public UiTabListActivity {
 public:
  CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Manifest& manifest);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  enum class Tab : uint8_t { Agenda, Tasks, Count };
  enum class RowKind : uint8_t { Period, Event, Empty, Info, AddTask, ShowDone, Task };
  struct RowRef {
    RowKind kind;
    int16_t index;  // event or todo index
    int32_t day;
  };
  struct RowText {
    std::string label, value, subtitle, heading;
  };

  // UiTabListActivity contract
  int listCount() const override { return static_cast<int>(rowItems.size()); }
  int tabCount() const override { return static_cast<int>(Tab::Count); }
  int activeTab() const override { return static_cast<int>(tab); }
  const char* tabLabel(int index) const override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleButtons() override;
  bool handleCustomInput() override;
  const char* headerTitle() const override { return manifest.name.c_str(); }
  void drawFooter() override;

  void reload();
  // Row vectors are read by the render task: mutate them only under RenderLock.
  void rebuildRows();  // takes RenderLock
  void rebuildRowsLocked();
  void buildAgendaRows();
  void buildTaskRows();
  void finishRows();
  void showPeriodMenu();
  void showEventDetails(const RowRef& row);
  void showTaskMenu(int todoIndex);
  void toggleTask(int todoIndex);
  void deleteTask(int todoIndex);
  void startAddTask();
  void openMonthView();
  void startSync();
  void showMessage(const char* text);
  void setPeriodStart(int32_t day);

  Manifest manifest;
  std::unique_ptr<CalendarData> data;
  OptionPopup popup;
  Tab tab = Tab::Agenda;
  int32_t today = 0;
  int32_t periodStart = 0;
  int32_t monthPick = 0;  // written by the month view
  bool clockSet = false;
  bool showDone = false;
  bool autoSyncPending = false;
  int64_t lastSync = 0;  // completed tasks hidden by default (exports carry many)

  std::vector<RowRef> rows;
  std::vector<RowText> texts;
  std::vector<freeink::ui::ListItem> rowItems;
  std::string pendingSummary;
  // Shown from handleCustomInput(): popup callbacks must not re-show the popup
  // (that would replace the std::function that is currently executing).
  const char* deferredMessage = nullptr;
};

// Formatting helpers shared with the month view.
std::string formatDayHeading(int32_t day, int32_t today);
std::string formatShortDate(int32_t day);
std::string formatMonthTitle(int year, unsigned month);
std::string formatTime(int16_t minute);

}  // namespace plugins::calendar
