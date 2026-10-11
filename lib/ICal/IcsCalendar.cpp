#include "IcsCalendar.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ics {

namespace {
constexpr size_t MAX_LINE = 2048;       // logical (unfolded) line cap
constexpr size_t MAX_SUMMARY = 120;     // bytes kept per title
constexpr size_t MAX_UID = 160;         // bytes kept per todo UID
constexpr int MAX_COUNT_SCAN = 20000;   // day steps when resolving COUNT
constexpr int MAX_SPAN_SCAN = 62;       // longest multi-day event looked back on

bool ieq(const std::string& a, const char* b) {
  const size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++) {
    char c = a[i];
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (c != b[i]) return false;
  }
  return true;
}

void upper(std::string& s) {
  for (auto& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
}

// "P1D", "PT1H30M", "P2W", "-PT15M" -> minutes (negative allowed); false if malformed.
bool parseDurationMin(const std::string& v, int32_t& out) {
  size_t i = 0;
  int sign = 1;
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) {
    if (v[i] == '-') sign = -1;
    i++;
  }
  if (i >= v.size() || v[i] != 'P') return false;
  i++;
  int64_t total = 0;
  bool timePart = false;
  int64_t num = 0;
  bool haveNum = false;
  for (; i < v.size(); i++) {
    const char c = v[i];
    if (c >= '0' && c <= '9') {
      num = num * 10 + (c - '0');
      haveNum = true;
      continue;
    }
    if (c == 'T') {
      timePart = true;
      continue;
    }
    if (!haveNum) return false;
    switch (c) {
      case 'W':
        total += num * 7 * 1440;
        break;
      case 'D':
        total += num * 1440;
        break;
      case 'H':
        total += num * 60;
        break;
      case 'M':
        if (timePart) total += num;
        break;
      case 'S':
        total += num / 60;
        break;
      default:
        return false;
    }
    num = 0;
    haveNum = false;
  }
  out = static_cast<int32_t>(sign * total);
  return true;
}

int weekdayCode(const char* s) {
  static constexpr const char* CODES[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  for (int i = 0; i < 7; i++)
    if (s[0] == CODES[i][0] && s[1] == CODES[i][1]) return i;
  return -1;
}
}  // namespace

void unescapeText(std::string& s) {
  size_t w = 0;
  for (size_t r = 0; r < s.size(); r++) {
    char c = s[r];
    if (c == '\\' && r + 1 < s.size()) {
      const char n = s[++r];
      if (n == 'n' || n == 'N') {
        c = ' ';  // titles are single-line on screen
      } else {
        c = n;
      }
    }
    s[w++] = c;
  }
  s.resize(w);
}

void truncateUtf8(std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) cut--;
  s.resize(cut);
}

bool extractPropertyValue(const char* buf, size_t len, std::string& out, size_t maxBytes) {
  out.clear();
  // Unfold the first logical line: CRLF/LF followed by space or tab continues it.
  std::string l;
  l.reserve(len < 512 ? len : 512);
  for (size_t i = 0; i < len; i++) {
    const char c = buf[i];
    if (c == '\r') continue;
    if (c == '\n') {
      if (i + 1 < len && (buf[i + 1] == ' ' || buf[i + 1] == '\t')) {
        i++;
        continue;
      }
      break;
    }
    l.push_back(c);
  }
  bool quoted = false;
  size_t colon = 0;
  for (; colon < l.size(); colon++) {
    if (l[colon] == '"') quoted = !quoted;
    if (l[colon] == ':' && !quoted) break;
  }
  if (colon >= l.size()) return false;
  out = l.substr(colon + 1);
  unescapeText(out);
  truncateUtf8(out, maxBytes);
  return true;
}

uint32_t Calendar::hashUid(const char* s, size_t len) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < len; i++) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 16777619u;
  }
  return h ? h : 1;
}

void Calendar::clear() {
  events_.clear();
  todos_.clear();
  exdates_.clear();
  arena.assign(1, '\0');  // offset 0 is the empty string
  truncated_ = false;
  line.clear();
  comp = Comp::None;
  depth = 0;
}

void Calendar::begin(uint8_t idx) {
  if (arena.empty()) arena.assign(1, '\0');
  fileIndex = idx;
  streamPos = 0;
  linePos = 0;
  line.clear();
  line.reserve(256);
  lineOverflow = false;
  lineStarted = false;
  continuation = false;
  comp = Comp::None;
  depth = 0;
}

void Calendar::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    const char c = data[i];
    if (c == '\r') continue;
    if (c == '\n') {
      continuation = true;  // next char decides: fold or new line
      continue;
    }
    if (continuation) {
      continuation = false;
      if (c == ' ' || c == '\t') continue;  // folded continuation
      if (lineStarted) onLine();
      line.clear();
      lineOverflow = false;
      linePos = streamPos + static_cast<uint32_t>(i);
    }
    if (!lineStarted) linePos = streamPos + static_cast<uint32_t>(i);
    lineStarted = true;
    if (line.size() < MAX_LINE) {
      line.push_back(c);
    } else {
      lineOverflow = true;
    }
  }
  streamPos += static_cast<uint32_t>(len);
}

void Calendar::end() {
  if (lineStarted && !line.empty()) onLine();
  line.clear();
  line.shrink_to_fit();
  lineStarted = false;
  continuation = false;
  comp = Comp::None;
  depth = 0;
}

void Calendar::onLine() {
  // NAME[;PARAM=..]:VALUE — params may hold quoted ':' characters.
  size_t nameEnd = 0;
  while (nameEnd < line.size() && line[nameEnd] != ';' && line[nameEnd] != ':') nameEnd++;
  if (nameEnd >= line.size()) return;
  size_t colon = nameEnd;
  bool quoted = false;
  while (colon < line.size()) {
    const char c = line[colon];
    if (c == '"') quoted = !quoted;
    if (c == ':' && !quoted) break;
    colon++;
  }
  if (colon >= line.size()) return;

  std::string name = line.substr(0, nameEnd);
  upper(name);
  std::string params = nameEnd < colon ? line.substr(nameEnd + 1, colon - nameEnd - 1) : std::string();
  std::string value = line.substr(colon + 1);

  if (name == "BEGIN") {
    if (comp == Comp::None) {
      if (ieq(value, "VEVENT")) {
        comp = Comp::Event;
        cur = Pending{};
        depth = 0;
      } else if (ieq(value, "VTODO")) {
        comp = Comp::Todo;
        cur = Pending{};
        depth = 0;
      } else if (!ieq(value, "VCALENDAR")) {
        comp = Comp::Other;
        depth = 0;
      }
    } else {
      depth++;
    }
    return;
  }
  if (name == "END") {
    if (comp == Comp::None) return;
    if (depth > 0) {
      depth--;
      return;
    }
    if (comp == Comp::Event) commitEvent();
    if (comp == Comp::Todo) commitTodo();
    comp = Comp::None;
    cur = Pending{};
    return;
  }
  if ((comp == Comp::Event || comp == Comp::Todo) && depth == 0) onProperty(name, params, value);
}

void Calendar::onProperty(const std::string& name, const std::string& params, const std::string& value) {
  (void)params;  // TZID is treated as the device's local zone (floating time).
  if (name == "DTSTART") {
    parseDateTime(value.c_str(), value.size(), cur.start);
  } else if (name == "DTEND") {
    parseDateTime(value.c_str(), value.size(), cur.end);
  } else if (name == "DUE") {
    parseDateTime(value.c_str(), value.size(), cur.due);
  } else if (name == "DURATION") {
    int32_t m;
    if (parseDurationMin(value, m) && m >= 0) cur.durationMin = m;
  } else if (name == "SUMMARY") {
    cur.summary = value;
    unescapeText(cur.summary);
    truncateUtf8(cur.summary, MAX_SUMMARY);
    cur.summaryPos = linePos;
  } else if (name == "LOCATION") {
    cur.locationPos = linePos;
  } else if (name == "UID") {
    cur.uid = value;
  } else if (name == "RRULE") {
    parseRRule(value);
  } else if (name == "EXDATE") {
    size_t pos = 0;
    while (pos < value.size()) {
      size_t comma = value.find(',', pos);
      if (comma == std::string::npos) comma = value.size();
      DateTime dt;
      if (parseDateTime(value.c_str() + pos, comma - pos, dt)) cur.exdates.push_back(toLocalDt(dt).day);
      pos = comma + 1;
    }
  } else if (name == "RECURRENCE-ID") {
    parseDateTime(value.c_str(), value.size(), cur.recurrenceId);
  } else if (name == "STATUS") {
    if (ieq(value, "COMPLETED")) cur.completed = true;
  } else if (name == "COMPLETED") {
    cur.completed = true;
  } else if (name == "PERCENT-COMPLETE") {
    if (atoi(value.c_str()) >= 100) cur.completed = true;
  } else if (name == "PRIORITY") {
    const int p = atoi(value.c_str());
    cur.priority = static_cast<uint8_t>(p >= 0 && p <= 9 ? p : 0);
  }
}

void Calendar::parseRRule(const std::string& value) {
  size_t pos = 0;
  while (pos < value.size()) {
    size_t semi = value.find(';', pos);
    if (semi == std::string::npos) semi = value.size();
    const std::string part = value.substr(pos, semi - pos);
    pos = semi + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    std::string key = part.substr(0, eq);
    std::string val = part.substr(eq + 1);
    upper(key);
    upper(val);
    if (key == "FREQ") {
      if (val == "DAILY") cur.freq = Freq::Daily;
      else if (val == "WEEKLY") cur.freq = Freq::Weekly;
      else if (val == "MONTHLY") cur.freq = Freq::Monthly;
      else if (val == "YEARLY") cur.freq = Freq::Yearly;
    } else if (key == "INTERVAL") {
      const int n = atoi(val.c_str());
      cur.interval = static_cast<uint8_t>(n >= 1 && n <= 255 ? n : 1);
    } else if (key == "COUNT") {
      const int n = atoi(val.c_str());
      cur.count = static_cast<uint16_t>(n > 0 && n < 65535 ? n : 0);
    } else if (key == "UNTIL") {
      parseDateTime(val.c_str(), val.size(), cur.until);
    } else if (key == "BYMONTHDAY") {
      const int n = atoi(val.c_str());
      cur.byMonthDay = static_cast<uint8_t>(n >= 1 && n <= 31 ? n : 0);
    } else if (key == "BYDAY") {
      size_t p = 0;
      while (p < val.size()) {
        size_t comma = val.find(',', p);
        if (comma == std::string::npos) comma = val.size();
        const std::string item = val.substr(p, comma - p);
        p = comma + 1;
        if (item.size() < 2) continue;
        const int wd = weekdayCode(item.c_str() + item.size() - 2);
        if (wd < 0) continue;
        cur.byDayMask |= static_cast<uint8_t>(1u << wd);
        if (item.size() > 2) {
          const int ord = atoi(item.substr(0, item.size() - 2).c_str());
          if (ord >= -5 && ord <= 5) cur.byDayOrd = static_cast<int8_t>(ord);
        }
      }
    }
  }
}

DateTime Calendar::toLocalDt(const DateTime& dt) const {
  if (!dt.valid() || !dt.utc || dt.allDay()) return dt;
  DateTime out = dt;
  const int64_t epoch = static_cast<int64_t>(dt.day) * 86400 + static_cast<int64_t>(dt.minute) * 60;
  toLocal(epoch, out.day, out.minute);
  out.utc = false;
  return out;
}

uint32_t Calendar::addString(const std::string& s, size_t maxBytes) {
  if (s.empty()) return 0;
  std::string t = s;
  truncateUtf8(t, maxBytes);
  if (arena.size() + t.size() + 1 > limits.maxArena) {
    truncated_ = true;
    return 0;
  }
  const auto off = static_cast<uint32_t>(arena.size());
  arena.append(t);
  arena.push_back('\0');
  return off;
}

void Calendar::commitEvent() {
  if (!cur.start.valid()) return;
  const DateTime s = toLocalDt(cur.start);
  Event ev;
  ev.startDay = s.day;
  ev.startMin = s.minute;
  ev.file = fileIndex;

  // End as an exclusive instant; convert to an inclusive last day.
  int32_t endDay = s.day;
  int16_t endMin = s.minute;
  if (cur.end.valid()) {
    const DateTime e = toLocalDt(cur.end);
    endDay = e.day;
    endMin = e.minute;
  } else if (cur.durationMin >= 0) {
    if (s.allDay()) {
      endDay = s.day + std::max<int32_t>(1, cur.durationMin / 1440);
    } else {
      const int64_t total = static_cast<int64_t>(s.day) * 1440 + s.minute + cur.durationMin;
      endDay = static_cast<int32_t>(total / 1440);
      endMin = static_cast<int16_t>(total % 1440);
    }
  } else if (s.allDay()) {
    endDay = s.day + 1;
  }
  if (s.allDay()) {
    ev.endDay = std::max(s.day, endDay - 1);
    ev.endMin = -1;
  } else {
    ev.endDay = (endMin == 0 && endDay > s.day) ? endDay - 1 : std::max(s.day, endDay);
    ev.endMin = endMin;
  }

  ev.freq = cur.freq;
  ev.interval = cur.interval;
  ev.count = cur.count;
  ev.byDayMask = cur.byDayMask;
  ev.byDayOrd = cur.byDayOrd;
  ev.byMonthDay = cur.byMonthDay;
  if (cur.until.valid()) ev.untilDay = toLocalDt(cur.until).day;
  ev.uidHash = cur.uid.empty() ? (0x80000000u | static_cast<uint32_t>(events_.size() + 1))
                               : hashUid(cur.uid.c_str(), cur.uid.size());

  // An overridden instance hides the matching occurrence of its master series.
  if (cur.recurrenceId.valid()) exdates_.push_back({ev.uidHash, toLocalDt(cur.recurrenceId).day});

  // Resolve COUNT now so finished series are dropped before they take a slot.
  if (ev.freq != Freq::None && ev.count > 0) {
    resolveCount(ev);
    ev.count = 0;
  }
  if (ev.freq == Freq::None) {
    if (limits.windowStart != NO_DAY && ev.endDay < limits.windowStart) return;
    if (limits.windowEnd != NO_DAY && ev.startDay > limits.windowEnd) return;
  } else {
    if (limits.windowEnd != NO_DAY && ev.startDay > limits.windowEnd) return;
    if (limits.windowStart != NO_DAY && ev.untilDay != NO_DAY &&
        ev.untilDay + (ev.endDay - ev.startDay) < limits.windowStart)
      return;
  }
  if (events_.size() >= limits.maxEvents) {
    truncated_ = true;
    return;
  }
  ev.summaryPos = cur.summaryPos;
  ev.locationPos = cur.locationPos;
  for (const int32_t d : cur.exdates) exdates_.push_back({ev.uidHash, d});
  events_.push_back(ev);
}

void Calendar::commitTodo() {
  if (todos_.size() >= limits.maxTodos) {
    truncated_ = true;
    return;
  }
  Todo t;
  t.file = fileIndex;
  t.completed = cur.completed;
  t.priority = cur.priority;
  const DateTime due = toLocalDt(cur.due.valid() ? cur.due : cur.start);
  t.dueDay = due.day;
  t.dueMin = due.minute;
  t.summary = addString(cur.summary, MAX_SUMMARY);
  t.uid = addString(cur.uid, MAX_UID);
  todos_.push_back(t);
}

bool Calendar::ruleMatches(const Event& ev, int32_t day) const {
  if (day < ev.startDay) return false;
  if (ev.untilDay != NO_DAY && day > ev.untilDay) return false;
  const int interval = ev.interval ? ev.interval : 1;
  switch (ev.freq) {
    case Freq::None:
      return day == ev.startDay;
    case Freq::Daily:
      return (day - ev.startDay) % interval == 0;
    case Freq::Weekly: {
      const uint8_t mask = ev.byDayMask ? ev.byDayMask : static_cast<uint8_t>(1u << weekdayMon0(ev.startDay));
      if (!(mask & (1u << weekdayMon0(day)))) return false;
      const int32_t weeks = (mondayOf(day) - mondayOf(ev.startDay)) / 7;
      return weeks % interval == 0;
    }
    case Freq::Monthly: {
      int y, ys;
      unsigned m, d, ms, ds;
      civilFromDays(day, y, m, d);
      civilFromDays(ev.startDay, ys, ms, ds);
      const int months = (y - ys) * 12 + static_cast<int>(m) - static_cast<int>(ms);
      if (months % interval != 0) return false;
      if (ev.byDayMask) {
        if (!(ev.byDayMask & (1u << weekdayMon0(day)))) return false;
        if (ev.byDayOrd > 0) return static_cast<int>((d - 1) / 7 + 1) == ev.byDayOrd;
        if (ev.byDayOrd < 0) return (daysInMonth(y, m) - static_cast<int>(d)) / 7 + 1 == -ev.byDayOrd;
        return true;
      }
      const unsigned dom = ev.byMonthDay ? ev.byMonthDay : ds;
      return d == dom;
    }
    case Freq::Yearly: {
      int y, ys;
      unsigned m, d, ms, ds;
      civilFromDays(day, y, m, d);
      civilFromDays(ev.startDay, ys, ms, ds);
      return m == ms && d == ds && (y - ys) % interval == 0;
    }
  }
  return false;
}

bool Calendar::isExcluded(const Event& ev, int32_t day) const {
  if (ev.freq == Freq::None || exdates_.empty()) return false;
  return std::binary_search(exdates_.begin(), exdates_.end(), ExDate{ev.uidHash, day});
}

bool Calendar::startsOn(const Event& ev, int32_t day) const { return ruleMatches(ev, day) && !isExcluded(ev, day); }

void Calendar::resolveCount(Event& ev) const {
  const int32_t explicitUntil = ev.untilDay;
  ev.untilDay = NO_DAY;
  int n = 0;
  int32_t last = NO_DAY;
  for (int step = 0; step < MAX_COUNT_SCAN; step++) {
    const int32_t d = ev.startDay + step;
    if (explicitUntil != NO_DAY && d > explicitUntil) break;
    if (ruleMatches(ev, d)) {
      last = d;
      if (++n >= ev.count) break;
    }
  }
  ev.untilDay = last != NO_DAY ? last : ev.startDay;
  if (explicitUntil != NO_DAY && explicitUntil < ev.untilDay) ev.untilDay = explicitUntil;
}

void Calendar::finalize() {
  std::sort(exdates_.begin(), exdates_.end());
  for (auto& ev : events_) {
    if (ev.freq != Freq::None && ev.count > 0) resolveCount(ev);
  }
  if (limits.windowStart != NO_DAY) {
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [this](const Event& ev) {
                                   return ev.freq != Freq::None && ev.untilDay != NO_DAY &&
                                          ev.untilDay + (ev.endDay - ev.startDay) < limits.windowStart;
                                 }),
                  events_.end());
  }
  sortTodos();
}

size_t Calendar::occurrencesOn(int32_t day, Occurrence* out, size_t maxOut) const {
  size_t n = 0;
  for (size_t i = 0; i < events_.size() && n < maxOut; i++) {
    const Event& ev = events_[i];
    const int span = std::min<int32_t>(ev.endDay - ev.startDay, MAX_SPAN_SCAN);
    for (int k = 0; k <= span; k++) {
      if (startsOn(ev, day - k)) {
        out[n++] = Occurrence{static_cast<uint16_t>(i), k == 0 ? ev.startMin : static_cast<int16_t>(-1), k > 0};
        break;
      }
    }
  }
  // Insertion sort: all-day (-1) first, then by start minute; stable.
  for (size_t i = 1; i < n; i++) {
    const Occurrence o = out[i];
    size_t j = i;
    while (j > 0 && out[j - 1].startMin > o.startMin) {
      out[j] = out[j - 1];
      j--;
    }
    out[j] = o;
  }
  return n;
}

bool Calendar::hasEventsOn(int32_t day) const {
  for (const Event& ev : events_) {
    const int span = std::min<int32_t>(ev.endDay - ev.startDay, MAX_SPAN_SCAN);
    for (int k = 0; k <= span; k++)
      if (startsOn(ev, day - k)) return true;
  }
  return false;
}

void Calendar::sortTodos() {
  std::stable_sort(todos_.begin(), todos_.end(), [this](const Todo& a, const Todo& b) {
    if (a.completed != b.completed) return !a.completed;
    const bool ad = a.dueDay != NO_DAY, bd = b.dueDay != NO_DAY;
    if (ad != bd) return ad;
    if (ad && a.dueDay != b.dueDay) return a.dueDay < b.dueDay;
    if (ad && a.dueMin != b.dueMin) return a.dueMin < b.dueMin;
    const int pa = a.priority ? a.priority : 10, pb = b.priority ? b.priority : 10;
    if (pa != pb) return pa < pb;
    return strcmp(str(a.summary), str(b.summary)) < 0;
  });
}

int Calendar::findTodo(const char* uid) const {
  if (!uid || !*uid) return -1;
  for (size_t i = 0; i < todos_.size(); i++)
    if (strcmp(str(todos_[i].uid), uid) == 0) return static_cast<int>(i);
  return -1;
}

}  // namespace ics
