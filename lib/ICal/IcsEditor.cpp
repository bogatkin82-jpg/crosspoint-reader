#include "IcsEditor.h"

#include <cstring>
#include <string>
#include <vector>

namespace ics {

namespace {
constexpr size_t MAX_COMPONENT_BYTES = 16 * 1024;  // buffered VTODO cap

// Physical-line reader over a Source; strips CR/LF terminators.
class PhysReader {
 public:
  explicit PhysReader(Source* s) : src(s) {}
  // Returns false at EOF when nothing was read; sets error() on I/O failure.
  bool next(std::string& out) {
    out.clear();
    if (!src) return false;
    bool any = false;
    while (true) {
      if (pos >= len) {
        if (eof) return any;
        len = src->read(buf, sizeof(buf));
        pos = 0;
        if (len < 0) {
          err = true;
          eof = true;
          len = 0;
          return any;
        }
        if (len == 0) {
          eof = true;
          return any;
        }
      }
      const char c = buf[pos++];
      any = true;
      if (c == '\n') return true;
      if (c != '\r') out.push_back(c);
    }
  }
  bool error() const { return err; }

 private:
  Source* src;
  char buf[256];
  int len = 0;
  int pos = 0;
  bool eof = false;
  bool err = false;
};

bool isCont(const std::string& p) { return !p.empty() && (p[0] == ' ' || p[0] == '\t'); }

// Upper-cased property name and raw value of an (unfolded) line.
void splitLine(const std::string& l, std::string& name, std::string& value) {
  size_t i = 0;
  while (i < l.size() && l[i] != ';' && l[i] != ':') i++;
  name = l.substr(0, i);
  for (auto& c : name)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  bool quoted = false;
  size_t colon = i;
  while (colon < l.size()) {
    if (l[colon] == '"') quoted = !quoted;
    if (l[colon] == ':' && !quoted) break;
    colon++;
  }
  value = colon < l.size() ? l.substr(colon + 1) : std::string();
}

bool valueIs(const std::string& v, const char* want) {
  if (v.size() != strlen(want)) return false;
  for (size_t i = 0; i < v.size(); i++) {
    char c = v[i];
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    if (c != want[i]) return false;
  }
  return true;
}

bool writeLine(Sink& out, const std::string& l) { return out.write(l.data(), l.size()) && out.write("\r\n", 2); }
bool writeLine(Sink& out, const char* l) { return out.write(l, strlen(l)) && out.write("\r\n", 2); }

bool writeProp(Sink& out, const char* name, const char* value) {
  std::string l = name;
  l.push_back(':');
  l.append(value);
  return writeLine(out, l);
}

// Logical line inside a buffered component: physical lines [first, last].
struct Group {
  size_t first, last;
  std::string name, value;
  int depth;
};

std::vector<Group> groupLines(const std::vector<std::string>& phys) {
  std::vector<Group> groups;
  groups.reserve(phys.size());
  int depth = 0;
  for (size_t i = 0; i < phys.size(); i++) {
    if (isCont(phys[i]) && !groups.empty()) {
      groups.back().last = i;
      continue;
    }
    groups.push_back(Group{i, i, {}, {}, 0});
  }
  for (auto& g : groups) {
    std::string unfolded = phys[g.first];
    for (size_t i = g.first + 1; i <= g.last; i++) unfolded.append(phys[i], 1, std::string::npos);
    splitLine(unfolded, g.name, g.value);
    if (g.name == "END") depth--;
    g.depth = depth;  // BEGIN/END of the component itself sit at depth 0
    if (g.name == "BEGIN") depth++;
  }
  // depth is 1 for the component's own properties; normalize so 0 = own level.
  for (auto& g : groups) g.depth = g.depth > 0 ? g.depth - 1 : 0;
  return groups;
}
}  // namespace

bool writeTextProperty(Sink& out, const char* name, const char* text) {
  std::string l = name;
  l.push_back(':');
  for (const char* p = text; p && *p; p++) {
    switch (*p) {
      case '\\':
        l.append("\\\\");
        break;
      case ';':
        l.append("\\;");
        break;
      case ',':
        l.append("\\,");
        break;
      case '\n':
        l.append("\\n");
        break;
      case '\r':
        break;
      default:
        l.push_back(*p);
    }
  }
  size_t pos = 0;
  bool first = true;
  while (pos < l.size()) {
    size_t take = first ? 75 : 74;
    if (pos + take < l.size()) {
      while (take > 1 && (static_cast<uint8_t>(l[pos + take]) & 0xC0) == 0x80) take--;
    } else {
      take = l.size() - pos;
    }
    if (!first && !out.write(" ", 1)) return false;
    if (!out.write(l.data() + pos, take) || !out.write("\r\n", 2)) return false;
    pos += take;
    first = false;
  }
  return true;
}

EditStatus editTodo(Source& src, Sink& out, const char* uid, TodoEdit edit, const char* nowUtcStamp) {
  PhysReader reader(&src);
  std::string p, name, value;
  std::vector<std::string> comp;
  size_t compBytes = 0;
  bool buffering = false;
  int innerDepth = 0;
  bool found = false;

  while (reader.next(p)) {
    if (!buffering) {
      if (!isCont(p)) {
        splitLine(p, name, value);
        if (name == "BEGIN" && valueIs(value, "VTODO") && !found) {
          buffering = true;
          innerDepth = 0;
          comp.clear();
          compBytes = 0;
          comp.push_back(p);
          compBytes += p.size();
          continue;
        }
      }
      if (!writeLine(out, p)) return EditStatus::IoError;
      continue;
    }

    comp.push_back(p);
    compBytes += p.size();
    if (compBytes > MAX_COMPONENT_BYTES) return EditStatus::TooLarge;
    if (isCont(p)) continue;
    splitLine(p, name, value);
    if (name == "BEGIN") {
      innerDepth++;
      continue;
    }
    if (name != "END") continue;
    if (innerDepth > 0) {
      innerDepth--;
      continue;
    }

    // Complete VTODO buffered: decide whether it is the target.
    buffering = false;
    const auto groups = groupLines(comp);
    bool match = false;
    for (const auto& g : groups) {
      if (g.depth == 0 && g.name == "UID" && g.value == uid) {
        match = true;
        break;
      }
    }
    if (!match) {
      for (const auto& l : comp)
        if (!writeLine(out, l)) return EditStatus::IoError;
      continue;
    }
    found = true;
    if (edit == TodoEdit::Delete) continue;

    const bool done = edit == TodoEdit::SetCompleted;
    for (size_t gi = 0; gi + 1 < groups.size(); gi++) {  // last group is END:VTODO
      const auto& g = groups[gi];
      if (g.depth == 0 && (g.name == "STATUS" || g.name == "COMPLETED" || g.name == "PERCENT-COMPLETE" ||
                           g.name == "LAST-MODIFIED")) {
        continue;
      }
      for (size_t i = g.first; i <= g.last; i++)
        if (!writeLine(out, comp[i])) return EditStatus::IoError;
    }
    bool ok = writeProp(out, "LAST-MODIFIED", nowUtcStamp);
    if (done) {
      ok = ok && writeProp(out, "STATUS", "COMPLETED") && writeProp(out, "COMPLETED", nowUtcStamp) &&
           writeProp(out, "PERCENT-COMPLETE", "100");
    } else {
      ok = ok && writeProp(out, "STATUS", "NEEDS-ACTION");
    }
    ok = ok && writeLine(out, "END:VTODO");
    if (!ok) return EditStatus::IoError;
  }
  if (reader.error()) return EditStatus::IoError;
  if (buffering) {  // truncated file: keep what was there
    for (const auto& l : comp)
      if (!writeLine(out, l)) return EditStatus::IoError;
  }
  return found ? EditStatus::Ok : EditStatus::NotFound;
}

EditStatus appendTodo(Source* src, Sink& out, const NewTodo& todo, const char* nowUtcStamp) {
  PhysReader reader(src);
  std::string p, name, value, heldEnd;
  bool sawCalendar = false;
  bool holding = false;

  while (reader.next(p)) {
    if (!isCont(p)) {
      splitLine(p, name, value);
      if (name == "BEGIN" && valueIs(value, "VCALENDAR")) sawCalendar = true;
      if (name == "END" && valueIs(value, "VCALENDAR")) {
        if (holding && !writeLine(out, heldEnd)) return EditStatus::IoError;
        heldEnd = p;
        holding = true;
        continue;
      }
    }
    if (holding) {  // content after END:VCALENDAR: flush the held line in order
      if (!writeLine(out, heldEnd)) return EditStatus::IoError;
      holding = false;
    }
    if (!writeLine(out, p)) return EditStatus::IoError;
  }
  if (reader.error()) return EditStatus::IoError;

  bool ok = true;
  if (!sawCalendar) {
    ok = writeLine(out, "BEGIN:VCALENDAR") && writeLine(out, "VERSION:2.0") &&
         writeLine(out, "PRODID:-//CrossPoint Reader//Calendar plugin//EN");
  }
  ok = ok && writeLine(out, "BEGIN:VTODO") && writeProp(out, "UID", todo.uid) &&
       writeProp(out, "DTSTAMP", nowUtcStamp) && writeProp(out, "CREATED", nowUtcStamp) &&
       writeTextProperty(out, "SUMMARY", todo.summary);
  if (ok && todo.dueDay != NO_DAY) {
    char date[12];
    formatDate(todo.dueDay, date, sizeof(date));
    ok = writeProp(out, "DUE;VALUE=DATE", date);
  }
  ok = ok && writeProp(out, "STATUS", "NEEDS-ACTION") && writeLine(out, "END:VTODO");
  ok = ok && (holding ? writeLine(out, heldEnd) : writeLine(out, "END:VCALENDAR"));
  return ok ? EditStatus::Ok : EditStatus::IoError;
}

}  // namespace ics
