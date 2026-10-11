# Plugins

Home → **Plugins** lists the plugins installed on the SD card and the built-in
plugin types that can be installed.

## Layout on the SD card

```
/plugins/<folder>/plugin.json   manifest
/plugins/<folder>/...           plugin data (e.g. .ics files)
```

```json
{
  "id": "calendar",
  "name": "Calendar",
  "runtime": "ical",
  "version": "1.0",
  "description": "Events and tasks from iCal (.ics) files",
  "enabled": true,
  "config": {}
}
```

* Confirm on an installed plugin opens it; long-press Confirm shows
  Open / Enable-Disable / Remove.
* Confirm on an entry under "Available to install" creates its folder, seeds
  default files and writes `plugin.json`.
* A folder can be copied, renamed and edited by hand (USB, web file manager,
  WebDAV). Several folders may use the same runtime, e.g. `/plugins/work` and
  `/plugins/home`, each with its own `.ics` files.

## Why runtimes, not loadable code

The ESP32-C3 has no PSRAM and runs code from flash through the cache; there is
no safe way to load native code from the SD card. A plugin is therefore data +
configuration bound to a *runtime* compiled into the firmware.

Adding a runtime:

1. Implement an `Activity` (or `UiListActivity`/`UiTabListActivity`) that takes
   a `plugins::Manifest` and reads its files from `manifest.dir`.
2. Define a `plugins::Runtime` (id, name/description string ids, default folder,
   optional `seed()` that writes starter files, `create()` factory).
3. Append it to `RUNTIMES` in `src/plugins/PluginRegistry.cpp`.

## `ical` runtime (Calendar)

Reads every `*.ics` file in the plugin folder (or the list in `config.files`).

| config key  | default       | meaning                                              |
|-------------|---------------|------------------------------------------------------|
| `files`     | all `*.ics`   | paths relative to the folder, or absolute (`/Cal/x.ics`) |
| `tasksFile` | `tasks.ics`   | file new tasks are appended to                        |
| `daysAhead` | `30`          | agenda period length, 7..120                          |
| `url`       | —             | internet calendar (iCal feed) saved as `file` (default `subscription.ics`) |
| `subscriptions` | —         | list of `{"url": ..., "file": ...}` feeds              |
| `autoSyncHours` | `0`       | sync automatically on open when older than N hours (0 = manual) |

### Internet calendars

Period row → **Sync** connects to Wi-Fi (saved networks or the picker), syncs
the clock over NTP, downloads every feed to `<file>.part`, rejects responses
that are not a `VCALENDAR` (SSO/login pages) and swaps the file in, keeping the
old copy on failure. The last success time is stored in `<folder>/.lastsync`.
Leaving the screen restarts the device silently back into the plugin to return
the Wi-Fi/TLS heap, like the other Wi-Fi screens. HTTPS is verified against the
firmware CA bundle; feeds behind a VPN or a corporate login are not reachable.

Busy-only feeds (times without SUMMARY) can be combined with a full export
copied in by hand: in the agenda an untitled occurrence is hidden when a titled
one (or an identical untitled one) occupies the same slot, so each meeting is
listed once, with its title when one is known.

Supported:

* `VEVENT`: `DTSTART`/`DTEND`/`DURATION`, all-day and multi-day events,
  `RRULE` with `FREQ=DAILY|WEEKLY|MONTHLY|YEARLY`, `INTERVAL`, `COUNT`,
  `UNTIL`, `BYDAY` (incl. `2TU`, `-1FR`), `BYMONTHDAY`; `EXDATE`;
  `RECURRENCE-ID` overrides; UTC (`...Z`) times converted to the device zone.
* `VTODO`: `SUMMARY`, `DUE` (or `DTSTART`), `PRIORITY`, `STATUS`/`COMPLETED`/
  `PERCENT-COMPLETE`. Toggling, deleting and adding tasks rewrite the file
  in place (streaming, via `.tmp` + `.bak`), keeping every other line intact.

Limits (RAM): 300 events within [today − 31 days, today + 400 days], 300 tasks,
20 KB of titles/locations; extra entries are skipped and a notice is shown.
`TZID=` parameters are treated as the device's local time zone.

Times need the RTC (X3) set: Settings → clock sync.
