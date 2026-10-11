#pragma once

#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

class Activity;
class GfxRenderer;
class MappedInputManager;

// Plugin host.
//
// A plugin is a folder on the SD card, /plugins/<folder>/, holding a
// plugin.json manifest plus whatever data the plugin works on:
//
//   {
//     "id": "calendar",            // stable identifier
//     "name": "Calendar",          // shown in the Plugins menu
//     "runtime": "ical",           // which built-in runtime drives it
//     "version": "1.0",
//     "description": "...",
//     "enabled": true,
//     "config": { ... }            // runtime-specific settings
//   }
//
// The ESP32-C3 has no PSRAM and executes from flash through the cache, so
// native code cannot be loaded from the SD card safely. Plugins are therefore
// data + configuration bound to a runtime compiled into the firmware: several
// plugin folders can share one runtime (e.g. a "Work" and a "Home" calendar,
// each with its own .ics files), and new runtimes are added by registering a
// PluginRuntime in PluginRegistry.cpp.
namespace plugins {

constexpr const char* PLUGINS_DIR = "/plugins";
constexpr const char* MANIFEST_NAME = "plugin.json";

struct Manifest {
  std::string dir;  // absolute folder path, no trailing slash
  std::string id;
  std::string name;
  std::string runtime;
  std::string version;
  std::string description;
  std::string configJson;  // serialized "config" object ("{}" when absent)
  bool enabled = true;
};

struct Runtime {
  const char* id;             // matches Manifest::runtime
  StrId nameId;               // default display name
  StrId descriptionId;        // default description
  const char* defaultFolder;  // folder name used by installBuiltin()
  const char* version;
  // Seeds data files into a freshly created plugin folder; may be null.
  bool (*seed)(const std::string& dir);
  // Creates the plugin's main screen.
  std::unique_ptr<Activity> (*create)(GfxRenderer& renderer, MappedInputManager& input, const Manifest& manifest);
};

// Runtimes compiled into this firmware.
const Runtime* const* runtimes(size_t& count);
const Runtime* findRuntime(const std::string& id);

// Reads every /plugins/*/plugin.json (sorted by name).
std::vector<Manifest> scanInstalled();
bool loadManifest(const std::string& dir, Manifest& out);
bool saveManifest(const Manifest& manifest);

// Creates /plugins/<defaultFolder>[-N], seeds it and writes its manifest.
bool installBuiltin(const Runtime& runtime, Manifest* installed = nullptr);
// Deletes the plugin folder recursively.
bool removePlugin(const Manifest& manifest);

}  // namespace plugins
