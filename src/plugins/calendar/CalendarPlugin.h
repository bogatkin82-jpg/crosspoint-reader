#pragma once

#include <IcsCalendar.h>
#include <IcsEditor.h>

#include <string>
#include <vector>

#include "plugins/PluginRegistry.h"

// "ical" runtime: an agenda / month view of VEVENTs and an editable to-do
// list of VTODOs, read from the .ics files in the plugin folder.
//
// Manifest config (all optional):
//   "files":     [".ics paths"]  relative to the plugin folder or absolute;
//                                default: every *.ics in the folder
//   "tasksFile": "tasks.ics"     where new tasks are written
//   "daysAhead": 30              length of the agenda period (7..120)
namespace plugins::calendar {

extern const Runtime RUNTIME;

// Local "today" from the RTC. Returns false (and a build-date fallback) when
// the device has no RTC or the clock was never set.
bool localToday(int32_t& day);
// Current UTC epoch seconds, 0 if the clock is unavailable.
int64_t nowUtcEpoch();

class CalendarData {
 public:
  enum class EditResult : uint8_t { Ok, ReadOnly, Failed };

  // Parses the plugin's .ics files. Events are kept for
  // [today - 31 days, today + 400 days].
  bool load(const Manifest& manifest, int32_t today);

  const ics::Calendar& calendar() const { return cal; }
  int daysAhead() const { return daysAhead_; }
  size_t fileCount() const { return paths.size(); }

  EditResult setTodoDone(size_t todoIndex, bool done);
  EditResult deleteTodo(size_t todoIndex);
  // Appends a VTODO to the tasks file, then reloads.
  bool addTodo(const std::string& summary, int32_t dueDay);

 private:
  bool parseFile(const std::string& path, uint8_t index);
  bool rewrite(const std::string& path, const char* uid, ics::TodoEdit edit);

  ics::Calendar cal;
  std::vector<std::string> paths;
  std::string tasksPath;
  Manifest manifestCopy;
  int32_t today_ = 0;
  int daysAhead_ = 30;
};

}  // namespace plugins::calendar
