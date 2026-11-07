# Calendar and the built-in plugin host

> [!NOTE]
> The official SD plugin system (`manifest.json`, `plugin.js`, `device.json`,
> Plugin Hub) is documented in [sd-plugins.md](sd-plugins.md). This page
> describes the small host the fork added for the calendar: a folder with
> `plugin.json` bound to a runtime compiled into the firmware. In this build the
> calendar is opened from **Home → Calendar** and installed on first use; the
> host's own list screen is not reachable from Home and will be removed.

## Calendar

**Home → Calendar** opens the agenda. On first use the firmware creates
`/plugins/calendar`, seeds `tasks.ics` and writes `plugin.json`. The calendar is
built into the firmware, so it is not gated by Settings → Connectivity →
Plugins. If the folder exists but its manifest is unreadable, the firmware
reports an error instead of creating a second copy; if the plugin is disabled,
re-enable it by setting `"enabled": true` in `plugin.json`.

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

Folders can be copied, renamed and edited by hand (USB, web file manager,
WebDAV), and several folders may use the same runtime, each with its own `.ics`
files. The Home row only ever opens `/plugins/calendar`; a renamed folder needs
its own entry point in the firmware. The same applies to new runtimes: adding
one to `RUNTIMES` makes it available to code, but no Home row is created
automatically.

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
old copy on failure. The last success time is stored in `<folder>/.lastsync`,
the last attempt in `<folder>/.lastattempt` so an automatically synced
failing feed is not retried on every open. Leaving the screen restarts the device silently back into the
calendar to return the Wi-Fi/TLS heap, like the other Wi-Fi screens; on touch
boards, where a software reset would cycle the panel rails, the calendar is
reloaded in place instead. HTTPS is
verified against the firmware CA bundle; feeds behind a VPN or a corporate login
are not reachable.

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

Limits (RAM): 500 events within [today − 31 days, today + 400 days], 300 tasks,
20 KB for task titles and UIDs; event titles and locations stay on the SD card
as file offsets and are read for the rows on screen. Extra entries are skipped
and a notice is shown. `TZID=` parameters are treated as the device's local time
zone.

Times need the clock: Settings → System → Clock → Sync Clock Now (shown only on
devices with an RTC).

### Feeds only reachable from a computer

`tools/calendar-sync/` holds a small macOS launchd job that downloads such a
feed (VPN, corporate network) and uploads it to the reader over WebDAV while
the reader is in File Transfer mode. See its README.
