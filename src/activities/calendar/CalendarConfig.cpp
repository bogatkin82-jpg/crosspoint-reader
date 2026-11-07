#include "CalendarConfig.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <IcsEditor.h>
#include <Logging.h>
#include <esp_random.h>

#include <cstdio>

#include "CalendarData.h"

namespace calendar {

namespace {

constexpr int MAX_FILES = 16;
constexpr int MAX_SUBSCRIPTIONS = 8;

class HalSink final : public ics::Sink {
 public:
  explicit HalSink(HalFile& f) : file(f) {}
  bool write(const char* data, size_t n) override { return file.write(data, n) == n; }

 private:
  HalFile& file;
};

std::string resolvePath(const std::string& dir, const std::string& p) { return p[0] == '/' ? p : dir + "/" + p; }

bool writeDefaultConfig(const std::string& dir) {
  JsonDocument doc;
  doc["daysAhead"] = 30;
  doc["tasksFile"] = "tasks.ics";
  String out;
  serializeJsonPretty(doc, out);
  return Storage.writeFile((dir + "/" + CONFIG_NAME).c_str(), out);
}

bool seedTasks(const std::string& dir) {
  const std::string path = dir + "/tasks.ics";
  if (Storage.exists(path.c_str())) return true;
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

// A folder created by the old plugin host always carries plugin.json, so it
// wins over /calendar; new installs get /calendar.
bool ensureDataDir(std::string& dir) {
  const std::string legacyMarker = std::string(LEGACY_CALENDAR_DIR) + "/plugin.json";
  const bool legacyExists = Storage.exists(LEGACY_CALENDAR_DIR);
  const bool ownExists = Storage.exists(CALENDAR_DIR);
  // Prefer the plugin-build folder unless /calendar was set up deliberately.
  if (legacyExists && (!ownExists || Storage.exists(legacyMarker.c_str()))) {
    dir = LEGACY_CALENDAR_DIR;
    LOG_INF("CAL", "Using legacy calendar folder %s", dir.c_str());
    return true;
  }
  if (ownExists) {
    dir = CALENDAR_DIR;
    LOG_INF("CAL", "Using calendar folder %s", dir.c_str());
    return true;
  }
  if (!Storage.ensureDirectoryExists(CALENDAR_DIR)) {
    LOG_ERR("CAL", "Cannot create %s", CALENDAR_DIR);
    return false;
  }
  if (!writeDefaultConfig(CALENDAR_DIR) || !seedTasks(CALENDAR_DIR)) {
    LOG_ERR("CAL", "Cannot seed %s", CALENDAR_DIR);
    Storage.removeDir(CALENDAR_DIR);
    return false;
  }
  dir = CALENDAR_DIR;
  LOG_INF("CAL", "Created calendar folder %s", dir.c_str());
  return true;
}

bool parseConfig(const std::string& dir, Config& out) {
  const std::string ownPath = dir + "/" + CONFIG_NAME;
  const std::string legacyPath = dir + "/plugin.json";
  const bool legacy = !Storage.exists(ownPath.c_str());
  const std::string& path = legacy ? legacyPath : ownPath;

  out.tasksFile = dir + "/tasks.ics";
  out.daysAhead = 30;
  out.autoSyncHours = 0;
  if (!Storage.exists(path.c_str())) return true;  // defaults only

  const String raw = Storage.readFile(path.c_str());
  if (raw.length() == 0) {
    LOG_ERR("CAL", "Empty config %s", path.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, raw.c_str(), raw.length())) {
    LOG_ERR("CAL", "Bad config %s", path.c_str());
    return false;
  }
  if (doc["name"].is<const char*>()) out.name = doc["name"].as<const char*>();
  const JsonObjectConst root = legacy ? doc["config"].as<JsonObjectConst>() : doc.as<JsonObjectConst>();
  if (root.isNull()) return true;

  const char* tasks = root["tasksFile"] | "tasks.ics";
  out.tasksFile = resolvePath(dir, *tasks ? tasks : "tasks.ics");

  const int days = root["daysAhead"] | 30;
  out.daysAhead = days < 7 ? 7 : (days > 120 ? 120 : days);
  const int autoHours = root["autoSyncHours"] | 0;
  out.autoSyncHours = autoHours < 0 ? 0 : autoHours;

  if (root["files"].is<JsonArrayConst>()) {
    for (JsonVariantConst f : root["files"].as<JsonArrayConst>()) {
      const char* p = f | "";
      if (*p && static_cast<int>(out.files.size()) < MAX_FILES) out.files.push_back(resolvePath(dir, p));
    }
  }

  if (root["url"].is<const char*>()) {
    const char* file = root["file"] | "subscription.ics";
    out.subscriptions.push_back(
        {root["url"].as<const char*>(), resolvePath(dir, *file ? file : "subscription.ics")});
  }
  if (root["subscriptions"].is<JsonArrayConst>()) {
    int n = 0;
    for (JsonObjectConst s : root["subscriptions"].as<JsonArrayConst>()) {
      const char* url = s["url"] | "";
      if (!*url || static_cast<int>(out.subscriptions.size()) >= MAX_SUBSCRIPTIONS) continue;
      char def[32];
      snprintf(def, sizeof(def), "subscription-%d.ics", ++n);
      const char* file = s["file"] | def;
      out.subscriptions.push_back({url, resolvePath(dir, *file ? file : def)});
    }
  }
  return true;
}

}  // namespace

LoadResult loadConfig(Config& out) {
  out = Config{};
  if (!ensureDataDir(out.dir)) return LoadResult::NoStorage;
  if (!parseConfig(out.dir, out)) return LoadResult::BadConfig;
  if (out.tasksFile.empty()) out.tasksFile = out.dir + "/tasks.ics";
  return LoadResult::Ok;
}

}  // namespace calendar
