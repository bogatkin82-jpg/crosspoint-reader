# Calendar

Built-in agenda, month view and an editable task list for iCal (`.ics`) files.
Open it from **Home → Calendar**. The calendar is part of the firmware, so it is
not gated by Settings → Connectivity → Plugins; the official SD plugin system
is documented in [sd-plugins.md](sd-plugins.md).

## Data on the SD card

```
<folder>/            /calendar, or /plugins/calendar on upgraded devices
    calendar.json    configuration (optional)
    *.ics            events and tasks
    tasks.ics        default file for new tasks
    .lastsync        time of the last successful feed sync
    .lastattempt     time of the last sync attempt
```

On first use the firmware creates `/calendar`, seeds `tasks.ics` and writes a
minimal `calendar.json`.

An existing `/plugins/calendar` is used while `/calendar` does not exist, so an
upgraded device keeps its files and settings. When both exist, the plugin-build
folder wins only while it still has `plugin.json`; otherwise `/calendar` is used.
To migrate, move the `.ics` files into `/calendar`, copy the options there and
delete `/plugins/calendar`. The legacy folder also stays inside the official
plugin root, where a plugin bundle named `calendar` could reuse or delete it, so
moving out is worthwhile.

Invalid JSON (or an empty file) is reported on screen when the calendar opens; a
missing `calendar.json` means defaults. If `/calendar` cannot be created, the
screen says so instead of opening an empty calendar.

## calendar.json

| key             | default       | meaning                                              |
|-----------------|---------------|------------------------------------------------------|
| `name`          | —             | screen title; localized "Calendar" when absent       |
| `files`         | all `*.ics`   | paths relative to the folder, or absolute            |
| `tasksFile`     | `tasks.ics`   | file new tasks are appended to                       |
| `daysAhead`     | `30`          | agenda period length, 7..120                         |
| `url`           | —             | internet calendar (iCal feed) saved as `file` (default `subscription.ics`) |
| `subscriptions` | —             | list of `{"url": ..., "file": ...}` feeds            |
| `autoSyncHours` | `0`           | sync automatically on open when older than N hours (0 = manual) |

```json
{
  "daysAhead": 30,
  "tasksFile": "tasks.ics",
  "url": "https://example.com/calendar.ics",
  "file": "work.ics"
}
```

A device still using `/plugins/calendar` reads these same keys from the
`config` object of its `plugin.json`; a `calendar.json` placed in that folder
takes precedence over it.

## Internet calendars

Period row → **Sync** connects to Wi-Fi (saved networks or the picker), syncs
the clock over NTP, downloads every feed to `<file>.part`, rejects responses
that are not a `VCALENDAR` (SSO/login pages) and swaps the file in, keeping the
old copy on failure. The last success time is stored in `<folder>/.lastsync`,
the last attempt in `<folder>/.lastattempt` so an automatically synced failing
feed is not retried on every open. Leaving the screen restarts the device
silently back into the calendar to return the Wi-Fi/TLS heap, like the other
Wi-Fi screens; on touch boards, where a software reset would cycle the panel
rails, the calendar is reloaded in place instead. HTTPS is verified against the
firmware CA bundle; feeds behind a VPN or a corporate login are not reachable.

Busy-only feeds (times without SUMMARY) can be combined with a full export
copied in by hand: in the agenda an untitled occurrence is hidden when a titled
one (or an identical untitled one) occupies the same slot, so each meeting is
listed once, with its title when one is known.

## Supported

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

## Feeds only reachable from a computer

`tools/calendar-sync/` holds a small macOS launchd job that downloads such a
feed (VPN, corporate network) and uploads it to the reader over WebDAV while
the reader is in File Transfer mode. It writes `/calendar/calendar-full.ics` by
default; point `BOOK_PATH` at the legacy folder on an upgraded device. See its
README.
