#pragma once

#include <IcsCalendar.h>

#include "activities/Activity.h"

namespace plugins::calendar {

// Month grid: today is drawn inverted, days with events carry a bar under
// the number, the selected day is framed and its events are listed below.
// Left/Right = day, Up/Down = week, page buttons = month. Confirm returns the
// selected day through *pickedDay; Back cancels.
class CalendarMonthActivity final : public Activity {
 public:
  CalendarMonthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const ics::Calendar& calendar,
                        int32_t today, int32_t initialDay, int32_t* pickedDay)
      : Activity("CalendarMonth", renderer, mappedInput),
        cal(calendar),
        today(today),
        selected(initialDay),
        pickedDay(pickedDay) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Grid {
    int x, y, cellW, cellH, firstDay;  // firstDay: day number of the top-left cell
  };
  Grid layout() const;
  void move(int32_t delta);
  void moveMonths(int delta);
  void pick();

  const ics::Calendar& cal;
  const int32_t today;
  int32_t selected;
  int32_t* pickedDay;
};

}  // namespace plugins::calendar
