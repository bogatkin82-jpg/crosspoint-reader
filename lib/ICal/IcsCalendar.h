#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "IcsDate.h"

// Compact in-memory model of the VEVENT / VTODO components of one or more
// .ics files. Strings live in one arena (offsets, not pointers) so a full
// calendar costs a handful of allocations; events outside the configured day
// window are dropped while parsing to bound RAM on the ESP32-C3.
namespace ics {

enum class Freq : uint8_t { None, Daily, Weekly, Monthly, Yearly };

struct Event {
  int32_t startDay = NO_DAY;
  int32_t endDay = NO_DAY;  // last day the event covers (inclusive)
  int32_t untilDay = NO_DAY;
  uint32_t summary = 0;
  uint32_t location = 0;
  uint32_t uidHash = 0;
  int16_t startMin = -1;  // -1 = all-day
  int16_t endMin = -1;
  uint16_t count = 0;
  uint8_t interval = 1;
  uint8_t byDayMask = 0;  // bit0 = Monday ... bit6 = Sunday
  int8_t byDayOrd = 0;    // monthly "2TU" -> 2, "-1FR" -> -1
  uint8_t byMonthDay = 0;
  Freq freq = Freq::None;
  uint8_t file = 0;
};

struct Todo {
  uint32_t uid = 0;
  uint32_t summary = 0;
  int32_t dueDay = NO_DAY;
  int16_t dueMin = -1;
  uint8_t priority = 0;  // 1 (highest) .. 9, 0 = undefined
  bool completed = false;
  uint8_t file = 0;
};

struct Occurrence {
  uint16_t event;
  int16_t startMin;  // -1 all-day; for continued multi-day events the day's start
  bool continued;    // started on an earlier day
};

class Calendar {
 public:
  struct Limits {
    uint16_t maxEvents = 400;
    uint16_t maxTodos = 300;
    uint32_t maxArena = 32 * 1024;
    int32_t windowStart = NO_DAY;  // keep non-recurring events ending on/after
    int32_t windowEnd = NO_DAY;    // ...and starting on/before
  };

  void clear();
  void setLimits(const Limits& l) { limits = l; }
  void setUtcConverter(UtcToLocalFn fn) { toLocal = fn ? fn : utcIdentity; }

  // Streaming parse: call begin(fileIndex), feed() with raw bytes as they are
  // read, then end(). Several files may be parsed in sequence; call
  // finalize() once after the last one.
  void begin(uint8_t fileIndex);
  void feed(const char* data, size_t len);
  void end();
  void finalize();

  const std::vector<Event>& events() const { return events_; }
  const std::vector<Todo>& todos() const { return todos_; }
  const char* str(uint32_t off) const { return arena.c_str() + off; }
  bool truncated() const { return truncated_; }

  // Occurrences on a local day, sorted all-day first, then by start time.
  size_t occurrencesOn(int32_t day, Occurrence* out, size_t maxOut) const;
  bool hasEventsOn(int32_t day) const;
  // True if an occurrence of `ev` starts on `day` (recurrence + exdates).
  bool startsOn(const Event& ev, int32_t day) const;

  // Incomplete first, then by due date, priority, summary.
  void sortTodos();
  // Find a todo by UID; -1 if missing.
  int findTodo(const char* uid) const;
  // Local edits mirrored after a successful file rewrite.
  void setTodoCompleted(size_t index, bool done) { todos_[index].completed = done; }
  void removeTodo(size_t index) { todos_.erase(todos_.begin() + static_cast<long>(index)); }

  static uint32_t hashUid(const char* s, size_t len);

 private:
  enum class Comp : uint8_t { None, Event, Todo, Other };
  struct ExDate {
    uint32_t uidHash;
    int32_t day;
    bool operator<(const ExDate& o) const { return uidHash != o.uidHash ? uidHash < o.uidHash : day < o.day; }
  };

  // Parser state for the component being read.
  struct Pending {
    DateTime start, end, due, recurrenceId, until;
    int32_t durationMin = -1;
    std::string summary, location, uid;
    std::vector<int32_t> exdates;
    Freq freq = Freq::None;
    uint16_t count = 0;
    uint8_t interval = 1;
    uint8_t byDayMask = 0;
    int8_t byDayOrd = 0;
    uint8_t byMonthDay = 0;
    uint8_t priority = 0;
    bool completed = false;
  };

  void onLine();
  void onProperty(const std::string& name, const std::string& params, const std::string& value);
  void commitEvent();
  void commitTodo();
  uint32_t addString(const std::string& s, size_t maxBytes);
  DateTime toLocalDt(const DateTime& dt) const;
  void parseRRule(const std::string& value);
  bool isExcluded(const Event& ev, int32_t day) const;
  bool ruleMatches(const Event& ev, int32_t day) const;
  void resolveCount(Event& ev) const;

  Limits limits;
  UtcToLocalFn toLocal = utcIdentity;
  std::vector<Event> events_;
  std::vector<Todo> todos_;
  std::vector<ExDate> exdates_;
  std::string arena;
  bool truncated_ = false;

  // line assembly
  std::string line;
  bool lineOverflow = false;
  bool pendingCR = false;
  bool lineStarted = false;
  bool continuation = false;
  uint8_t fileIndex = 0;
  Comp comp = Comp::None;
  int depth = 0;  // nesting below the current VEVENT/VTODO (VALARM etc.)
  Pending cur;
};

// Unescapes TEXT values (\n \, \; \\) in place.
void unescapeText(std::string& s);
// Truncates to at most maxBytes without splitting a UTF-8 sequence.
void truncateUtf8(std::string& s, size_t maxBytes);

}  // namespace ics
