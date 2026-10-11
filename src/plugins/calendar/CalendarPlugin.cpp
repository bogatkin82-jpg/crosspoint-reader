#include "CalendarPlugin.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <IcsEditor.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CalendarActivity.h"

namespace plugins::calendar {

namespace {
constexpr int MAX_FILES = 16;
constexpr size_t READ_CHUNK = 512;

class HalSource final : public ics::Source {
 public:
  explicit HalSource(HalFile& f) : file(f) {}
  int read(char* buf, int n) override { return file.read(buf, static_cast<size_t>(n)); }

 private:
  HalFile& file;
};

class HalSink final : public ics::Sink {
 public:
  explicit HalSink(HalFile& f) : file(f) {}
  bool write(const char* data, size_t n) override { return file.write(data, n) == n; }

 private:
  HalFile& file;
};

void utcToLocal(int64_t epochSec, int32_t& day, int16_t& minute) {
  const time_t t = static_cast<time_t>(epochSec);
  struct tm lt {};
  localtime_r(&t, &lt);
  day = ics::daysFromCivil(lt.tm_year + 1900, static_cast<unsigned>(lt.tm_mon + 1), static_cast<unsigned>(lt.tm_mday));
  minute = static_cast<int16_t>(lt.tm_hour * 60 + lt.tm_min);
}

bool hasIcsExtension(const char* name) {
  const size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".ics") == 0;
}

std::string resolvePath(const std::string& dir, const std::string& p) { return p[0] == '/' ? p : dir + "/" + p; }

bool clockLooksSet(const struct tm& t) { return t.tm_year + 1900 >= 2024; }

bool seedCalendar(const std::string& dir) {
  const std::string path = dir + "/tasks.ics";
  HalFile out;
  if (!Storage.openFileForWrite("CAL", path, out)) return false;
  HalSink sink(out);
  char uid[48];
  snprintf(uid, sizeof(uid), "welcome-%08lx@crosspoint", static_cast<unsigned long>(esp_random()));
  char stamp[20];
  ics::formatUtcStamp(nowUtcEpoch(), stamp, sizeof(stamp));
  ics::NewTodo todo;
  todo.summary = tr(STR_CAL_WELCOME_TASK);
  todo.uid = uid;
  return ics::appendTodo(nullptr, sink, todo, stamp) == ics::EditStatus::Ok;
}

std::unique_ptr<Activity> createCalendar(GfxRenderer& renderer, MappedInputManager& input, const Manifest& m) {
  auto activity = makeUniqueNoThrow<CalendarActivity>(renderer, input, m);
  if (!activity) LOG_ERR("CAL", "OOM: calendar activity");
  return activity;
}
}  // namespace

const Runtime RUNTIME = {
    "ical", StrId::STR_CAL_TITLE, StrId::STR_CAL_DESC, "calendar", "1.0", &seedCalendar, &createCalendar,
};

bool localToday(int32_t& day) {
  struct tm t {};
  if (halClock.isAvailable() && halClock.localTime(t) && clockLooksSet(t)) {
    day = ics::daysFromCivil(t.tm_year + 1900, static_cast<unsigned>(t.tm_mon + 1), static_cast<unsigned>(t.tm_mday));
    return true;
  }
  // Fallback: the firmware build date ("Mmm dd yyyy").
  static constexpr const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;
  int month = 1;
  for (int i = 0; i < 12; i++)
    if (strncmp(date, MONTHS + i * 3, 3) == 0) month = i + 1;
  day = ics::daysFromCivil(atoi(date + 7), static_cast<unsigned>(month), static_cast<unsigned>(atoi(date + 4)));
  return false;
}

int64_t readLastSync(const std::string& dir) {
  const std::string path = dir + "/.lastsync";
  if (!Storage.exists(path.c_str())) return 0;
  const String s = Storage.readFile(path.c_str());
  return static_cast<int64_t>(atoll(s.c_str()));
}

void writeLastSync(const std::string& dir, int64_t epoch) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(epoch));
  Storage.writeFile((dir + "/.lastsync").c_str(), String(buf));
}

int64_t nowUtcEpoch() {
  struct tm t {};
  if (!halClock.isAvailable() || !halClock.localTime(t) || !clockLooksSet(t)) return 0;
  return static_cast<int64_t>(mktime(&t));  // mktime inverts localtime_r under the active TZ
}

bool CalendarData::load(const Manifest& manifest, int32_t today) {
  if (&manifest != &manifestCopy) manifestCopy = manifest;
  today_ = today;
  paths.clear();
  tasksPath.clear();
  daysAhead_ = 30;

  JsonDocument config;
  if (deserializeJson(config, manifest.configJson.c_str())) config.to<JsonObject>();
  const int days = config["daysAhead"] | 30;
  daysAhead_ = days < 7 ? 7 : (days > 120 ? 120 : days);
  const char* tasks = config["tasksFile"] | "tasks.ics";
  tasksPath = resolvePath(manifest.dir, tasks);
  const int autoHours = config["autoSyncHours"] | 0;
  autoSyncHours_ = autoHours < 0 ? 0 : autoHours;

  subs.clear();
  if (config["url"].is<const char*>()) {
    const char* file = config["file"] | "subscription.ics";
    subs.push_back({config["url"].as<const char*>(), resolvePath(manifest.dir, *file ? file : "subscription.ics")});
  }
  if (config["subscriptions"].is<JsonArrayConst>()) {
    int n = 0;
    for (JsonObjectConst s : config["subscriptions"].as<JsonArrayConst>()) {
      const char* url = s["url"] | "";
      if (!*url || subs.size() >= 8) continue;
      char def[32];
      snprintf(def, sizeof(def), "subscription-%d.ics", ++n);
      const char* file = s["file"] | def;
      subs.push_back({url, resolvePath(manifest.dir, *file ? file : def)});
    }
  }

  if (config["files"].is<JsonArrayConst>()) {
    for (JsonVariantConst f : config["files"].as<JsonArrayConst>()) {
      const char* p = f | "";
      if (*p && paths.size() < MAX_FILES) paths.push_back(resolvePath(manifest.dir, p));
    }
  } else {
    auto dir = Storage.open(manifest.dir.c_str());
    if (dir && dir.isDirectory()) {
      dir.rewindDirectory();
      char name[96];
      for (auto entry = dir.openNextFile(); entry && paths.size() < MAX_FILES; entry = dir.openNextFile()) {
        entry.getName(name, sizeof(name));
        if (entry.isDirectory() || name[0] == '.' || !hasIcsExtension(name)) continue;
        paths.push_back(manifest.dir + "/" + name);
      }
    }
  }
  // Downloaded feeds are always shown, even with an explicit "files" list.
  for (const auto& s : subs) {
    bool listed = false;
    for (const auto& p : paths) listed |= p == s.path;
    if (!listed && Storage.exists(s.path.c_str()) && paths.size() < MAX_FILES) paths.push_back(s.path);
  }
  // Make sure the tasks file is editable even when listed implicitly.
  bool tasksListed = false;
  for (const auto& p : paths) tasksListed |= p == tasksPath;
  if (!tasksListed && Storage.exists(tasksPath.c_str()) && paths.size() < MAX_FILES) paths.push_back(tasksPath);

  cal.clear();
  cal.setUtcConverter(&utcToLocal);
  ics::Calendar::Limits limits;
  limits.maxEvents = 300;
  limits.maxTodos = 300;
  limits.maxArena = 20 * 1024;
  limits.windowStart = today - 31;
  limits.windowEnd = today + 400;
  cal.setLimits(limits);

  bool ok = true;
  for (size_t i = 0; i < paths.size(); i++) ok &= parseFile(paths[i], static_cast<uint8_t>(i));
  cal.finalize();
  LOG_INF("CAL", "Loaded %u files: %u events, %u todos%s", static_cast<unsigned>(paths.size()),
          static_cast<unsigned>(cal.events().size()), static_cast<unsigned>(cal.todos().size()),
          cal.truncated() ? " (truncated)" : "");
  return ok;
}

bool CalendarData::parseFile(const std::string& path, uint8_t index) {
  HalFile file;
  if (!Storage.openFileForRead("CAL", path, file)) return false;
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("CAL", "OOM: read buffer");
    return false;
  }
  cal.begin(index);
  while (true) {
    const int n = file.read(buf.get(), READ_CHUNK);
    if (n <= 0) break;
    cal.feed(buf.get(), static_cast<size_t>(n));
  }
  cal.end();
  return true;
}

bool CalendarData::rewrite(const std::string& path, const char* uid, ics::TodoEdit edit) {
  const std::string tmp = path + ".tmp";
  const std::string bak = path + ".bak";
  char stamp[20];
  ics::formatUtcStamp(nowUtcEpoch(), stamp, sizeof(stamp));

  ics::EditStatus status;
  {
    HalFile in, out;
    if (!Storage.openFileForRead("CAL", path, in)) return false;
    if (!Storage.openFileForWrite("CAL", tmp, out)) return false;
    HalSource src(in);
    HalSink sink(out);
    status = ics::editTodo(src, sink, uid, edit, stamp);
    out.flush();
    out.close();
    in.close();
  }
  if (status != ics::EditStatus::Ok) {
    LOG_ERR("CAL", "Edit of %s failed (%d)", path.c_str(), static_cast<int>(status));
    Storage.remove(tmp.c_str());
    return false;
  }
  // Swap via a backup so a power loss never leaves the calendar missing.
  Storage.remove(bak.c_str());
  if (!Storage.rename(path.c_str(), bak.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.rename(bak.c_str(), path.c_str());
    return false;
  }
  Storage.remove(bak.c_str());
  return true;
}

CalendarData::EditResult CalendarData::setTodoDone(size_t todoIndex, bool done) {
  if (todoIndex >= cal.todos().size()) return EditResult::Failed;
  const ics::Todo& t = cal.todos()[todoIndex];
  const char* uid = cal.str(t.uid);
  if (!*uid || t.file >= paths.size()) return EditResult::ReadOnly;
  const std::string uidCopy = uid;
  if (!rewrite(paths[t.file], uidCopy.c_str(), done ? ics::TodoEdit::SetCompleted : ics::TodoEdit::SetIncomplete)) {
    return EditResult::Failed;
  }
  cal.setTodoCompleted(todoIndex, done);
  cal.sortTodos();
  return EditResult::Ok;
}

CalendarData::EditResult CalendarData::deleteTodo(size_t todoIndex) {
  if (todoIndex >= cal.todos().size()) return EditResult::Failed;
  const ics::Todo& t = cal.todos()[todoIndex];
  const char* uid = cal.str(t.uid);
  if (!*uid || t.file >= paths.size()) return EditResult::ReadOnly;
  const std::string uidCopy = uid;
  if (!rewrite(paths[t.file], uidCopy.c_str(), ics::TodoEdit::Delete)) return EditResult::Failed;
  cal.removeTodo(todoIndex);
  return EditResult::Ok;
}

bool CalendarData::addTodo(const std::string& summary, int32_t dueDay) {
  if (summary.empty()) return false;
  const int64_t now = nowUtcEpoch();
  char uid[64];
  snprintf(uid, sizeof(uid), "%lld-%08lx@crosspoint", static_cast<long long>(now),
           static_cast<unsigned long>(esp_random()));
  char stamp[20];
  ics::formatUtcStamp(now, stamp, sizeof(stamp));
  ics::NewTodo todo;
  todo.summary = summary.c_str();
  todo.uid = uid;
  todo.dueDay = dueDay;

  const bool exists = Storage.exists(tasksPath.c_str());
  const std::string tmp = tasksPath + ".tmp";
  ics::EditStatus status;
  {
    HalFile in, out;
    if (exists && !Storage.openFileForRead("CAL", tasksPath, in)) return false;
    if (!Storage.openFileForWrite("CAL", tmp, out)) return false;
    HalSource src(in);
    HalSink sink(out);
    status = ics::appendTodo(exists ? &src : nullptr, sink, todo, stamp);
    out.flush();
    out.close();
    if (exists) in.close();
  }
  if (status != ics::EditStatus::Ok) {
    Storage.remove(tmp.c_str());
    return false;
  }
  if (exists) {
    const std::string bak = tasksPath + ".bak";
    Storage.remove(bak.c_str());
    if (!Storage.rename(tasksPath.c_str(), bak.c_str())) {
      Storage.remove(tmp.c_str());
      return false;
    }
    if (!Storage.rename(tmp.c_str(), tasksPath.c_str())) {
      Storage.rename(bak.c_str(), tasksPath.c_str());
      return false;
    }
    Storage.remove(bak.c_str());
  } else if (!Storage.rename(tmp.c_str(), tasksPath.c_str())) {
    return false;
  }
  return load(manifestCopy, today_);
}

}  // namespace plugins::calendar
