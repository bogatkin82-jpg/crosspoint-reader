#include "PluginRegistry.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "activities/Activity.h"
#include "plugins/calendar/CalendarPlugin.h"

namespace plugins {

namespace {
// Runtimes compiled into the firmware. Append new runtimes here.
const Runtime* const RUNTIMES[] = {&calendar::RUNTIME};

std::string manifestPath(const std::string& dir) { return dir + "/" + MANIFEST_NAME; }
}  // namespace

const Runtime* const* runtimes(size_t& count) {
  count = sizeof(RUNTIMES) / sizeof(RUNTIMES[0]);
  return RUNTIMES;
}

const Runtime* findRuntime(const std::string& id) {
  for (const Runtime* rt : RUNTIMES)
    if (id == rt->id) return rt;
  return nullptr;
}

bool loadManifest(const std::string& dir, Manifest& out) {
  const std::string path = manifestPath(dir);
  if (!Storage.exists(path.c_str())) return false;
  const String raw = Storage.readFile(path.c_str());
  if (raw.length() == 0) return false;

  JsonDocument doc;
  const auto err = deserializeJson(doc, raw.c_str(), raw.length());
  if (err) {
    LOG_ERR("PLUG", "Bad manifest %s: %s", path.c_str(), err.c_str());
    return false;
  }
  out = Manifest{};
  out.dir = dir;
  out.id = doc["id"] | "";
  out.name = doc["name"] | "";
  out.runtime = doc["runtime"] | "";
  out.version = doc["version"] | "";
  out.description = doc["description"] | "";
  out.enabled = doc["enabled"] | true;
  if (doc["config"].is<JsonObjectConst>()) {
    serializeJson(doc["config"], out.configJson);
  } else {
    out.configJson = "{}";
  }
  if (out.name.empty()) {
    const size_t slash = dir.find_last_of('/');
    out.name = slash == std::string::npos ? dir : dir.substr(slash + 1);
  }
  if (out.id.empty()) out.id = out.name;
  return !out.runtime.empty();
}

bool saveManifest(const Manifest& m) {
  JsonDocument doc;
  doc["id"] = m.id;
  doc["name"] = m.name;
  doc["runtime"] = m.runtime;
  doc["version"] = m.version;
  doc["description"] = m.description;
  doc["enabled"] = m.enabled;
  JsonDocument config;
  if (deserializeJson(config, m.configJson.empty() ? "{}" : m.configJson.c_str()) || !config.is<JsonObject>()) {
    config.to<JsonObject>();
  }
  doc["config"] = config;
  String out;
  serializeJsonPretty(doc, out);
  return Storage.writeFile(manifestPath(m.dir).c_str(), out);
}

std::vector<Manifest> scanInstalled() {
  std::vector<Manifest> result;
  auto root = Storage.open(PLUGINS_DIR);
  if (!root || !root.isDirectory()) return result;

  std::vector<std::string> dirs;
  dirs.reserve(8);
  root.rewindDirectory();
  char name[96];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (!entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) continue;
    dirs.push_back(std::string(PLUGINS_DIR) + "/" + name);
  }
  root.close();

  result.reserve(dirs.size());
  for (const auto& dir : dirs) {
    Manifest m;
    if (loadManifest(dir, m)) result.push_back(std::move(m));
  }
  std::sort(result.begin(), result.end(), [](const Manifest& a, const Manifest& b) { return a.name < b.name; });
  return result;
}

bool installBuiltin(const Runtime& runtime, Manifest* installed) {
  if (!Storage.ensureDirectoryExists(PLUGINS_DIR)) {
    LOG_ERR("PLUG", "Cannot create %s", PLUGINS_DIR);
    return false;
  }
  std::string dir = std::string(PLUGINS_DIR) + "/" + runtime.defaultFolder;
  for (int n = 2; Storage.exists(dir.c_str()) && n < 100; n++) {
    dir = std::string(PLUGINS_DIR) + "/" + runtime.defaultFolder + "-" + std::to_string(n);
  }
  if (!Storage.mkdir(dir.c_str())) {
    LOG_ERR("PLUG", "Cannot create %s", dir.c_str());
    return false;
  }
  if (runtime.seed && !runtime.seed(dir)) {
    LOG_ERR("PLUG", "Seeding %s failed", dir.c_str());
    Storage.removeDir(dir.c_str());
    return false;
  }
  Manifest m;
  m.dir = dir;
  const size_t slash = dir.find_last_of('/');
  m.id = dir.substr(slash + 1);
  m.name = I18N.get(runtime.nameId);
  m.runtime = runtime.id;
  m.version = runtime.version;
  m.description = I18N.get(runtime.descriptionId);
  m.configJson = "{}";
  m.enabled = true;
  if (!saveManifest(m)) {
    Storage.removeDir(dir.c_str());
    return false;
  }
  LOG_INF("PLUG", "Installed %s into %s", runtime.id, dir.c_str());
  if (installed) *installed = std::move(m);
  return true;
}

bool removePlugin(const Manifest& m) {
  // Only ever delete inside /plugins/.
  const std::string prefix = std::string(PLUGINS_DIR) + "/";
  if (m.dir.size() <= prefix.size() || m.dir.compare(0, prefix.size(), prefix) != 0 ||
      m.dir.find("..") != std::string::npos) {
    LOG_ERR("PLUG", "Refusing to remove %s", m.dir.c_str());
    return false;
  }
  return Storage.removeDir(m.dir.c_str());
}

}  // namespace plugins
