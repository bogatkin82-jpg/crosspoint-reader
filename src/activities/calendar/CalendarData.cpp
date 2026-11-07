#include "CalendarData.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <IcsEditor.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>


namespace calendar {

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


bool clockLooksSet(const struct tm& t) { return t.tm_year + 1900 >= 2024; }
}  // namespace

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

int64_t readLastAttempt(const std::string& dir) {
  const std::string path = dir + "/.lastattempt";
  if (!Storage.exists(path.c_str())) return 0;
  const String s = Storage.readFile(path.c_str());
  return static_cast<int64_t>(atoll(s.c_str()));
}

void writeLastAttempt(const std::string& dir, int64_t epoch) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(epoch));
  Storage.writeFile((dir + "/.lastattempt").c_str(), String(buf));
}

int64_t nowUtcEpoch() {
  struct tm t {};
  if (!halClock.isAvailable() || !halClock.localTime(t) || !clockLooksSet(t)) return 0;
  return static_cast<int64_t>(mktime(&t));  // mktime inverts localtime_r under the active TZ
}

void CalendarData::closeReader() {
  if (reader.isOpen()) reader.close();
  readerFile = -1;
}

bool CalendarData::readValue(uint8_t file, uint32_t pos, std::string& out) {
  constexpr size_t TEXT_READ = 640;  // a folded SUMMARY of ~160 bytes fits easily
  out.clear();
  if (pos == ics::NO_POS || file >= paths.size()) return false;
  if (!readBuf) {
    readBuf = makeUniqueNoThrow<char[]>(TEXT_READ);
    if (!readBuf) {
      LOG_ERR("CAL", "OOM: text buffer");
      return false;
    }
  }
  if (readerFile != file) {
    closeReader();
    if (!Storage.openFileForRead("CAL", paths[file], reader)) return false;
    readerFile = file;
  }
  if (!reader.seek(pos)) return false;
  const int n = reader.read(readBuf.get(), TEXT_READ);
  if (n <= 0) return false;
  return ics::extractPropertyValue(readBuf.get(), static_cast<size_t>(n), out);
}

void CalendarData::eventText(const ics::Event& ev, std::string* summary, std::string* location) {
  if (summary) readValue(ev.file, ev.summaryPos, *summary);
  if (location) readValue(ev.file, ev.locationPos, *location);
}

bool CalendarData::fileHasEvents(uint8_t file) const {
  for (const auto& ev : cal.events())
    if (ev.file == file) return true;
  return false;
}

bool CalendarData::load(const Config& config, int32_t today) {
  closeReader();
  if (&config != &configCopy) configCopy = config;
  today_ = today;
  paths.clear();
  tasksPath = config.tasksFile.empty() ? config.dir + "/tasks.ics" : config.tasksFile;
  const int days = config.daysAhead;
  daysAhead_ = days < 7 ? 7 : (days > 120 ? 120 : days);
  autoSyncHours_ = config.autoSyncHours < 0 ? 0 : config.autoSyncHours;
  subs = config.subscriptions;

  if (!config.files.empty()) {
    for (const auto& p : config.files) {
      if (paths.size() < MAX_FILES) paths.push_back(p);
    }
  } else {
    auto dir = Storage.open(config.dir.c_str());
    if (dir && dir.isDirectory()) {
      dir.rewindDirectory();
      char name[96];
      for (auto entry = dir.openNextFile(); entry && paths.size() < MAX_FILES; entry = dir.openNextFile()) {
        entry.getName(name, sizeof(name));
        if (entry.isDirectory() || name[0] == '.' || !hasIcsExtension(name)) continue;
        paths.push_back(config.dir + "/" + name);
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
  limits.maxEvents = 500;  // 36 B each; titles stay on the SD card
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
  closeReader();
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
  if (fileHasEvents(t.file)) {
    load(configCopy, today_);  // event offsets in this file moved
  } else {
    cal.setTodoCompleted(todoIndex, done);
    cal.sortTodos();
  }
  return EditResult::Ok;
}

CalendarData::EditResult CalendarData::deleteTodo(size_t todoIndex) {
  if (todoIndex >= cal.todos().size()) return EditResult::Failed;
  const ics::Todo& t = cal.todos()[todoIndex];
  const char* uid = cal.str(t.uid);
  if (!*uid || t.file >= paths.size()) return EditResult::ReadOnly;
  const std::string uidCopy = uid;
  const uint8_t file = t.file;
  if (!rewrite(paths[file], uidCopy.c_str(), ics::TodoEdit::Delete)) return EditResult::Failed;
  if (fileHasEvents(file)) {
    load(configCopy, today_);
  } else {
    cal.removeTodo(todoIndex);
  }
  return EditResult::Ok;
}

bool CalendarData::addTodo(const std::string& summary, int32_t dueDay) {
  if (summary.empty()) return false;
  closeReader();
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
  return load(configCopy, today_);
}

}  // namespace calendar
