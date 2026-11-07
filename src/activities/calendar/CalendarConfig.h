#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Runtime configuration for the built-in calendar.
//
// Data lives in /calendar. A device that already has /plugins/calendar from the
// plugin-host build keeps using that folder; nothing is moved automatically.
//
// calendar.json (all keys optional):
//   "files":         [".ics paths"]  relative to the folder or absolute;
//                                    default: every *.ics in the folder
//   "tasksFile":     "tasks.ics"     where new tasks are written
//   "daysAhead":     30              agenda period length (7..120)
//   "url":           "https://..."   one feed, saved as "file"
//                                    (default subscription.ics)
//   "subscriptions": [{"url": "...", "file": "work.ics"}, ...]
//   "autoSyncHours": 0               sync on open when older than N hours
namespace calendar {

constexpr const char* CALENDAR_DIR = "/calendar";
constexpr const char* LEGACY_CALENDAR_DIR = "/plugins/calendar";
constexpr const char* CONFIG_NAME = "calendar.json";

struct Subscription {
  std::string url;
  std::string path;  // absolute destination on the SD card
};

struct Config {
  std::string dir;                // absolute data folder
  std::string name;               // empty -> localized "Calendar"
  std::string tasksFile;          // absolute
  std::vector<std::string> files; // absolute; empty -> every *.ics in dir
  std::vector<Subscription> subscriptions;
  int daysAhead = 30;
  int autoSyncHours = 0;
};

// Resolves the data folder (creating /calendar and seeding it on first use)
// and parses calendar.json, falling back to the legacy plugin.json config.
enum class LoadResult : uint8_t { Ok, Storage, BadConfig };
LoadResult loadConfig(Config& out);

}  // namespace calendar
