# calendar-sync (macOS)

For calendars the reader cannot download itself — feeds that resolve only on
a VPN or a corporate network. The computer fetches the feed and pushes it to the
reader over WebDAV whenever the reader shows **File Transfer** on the same Wi-Fi.

```
bash install.sh      # asks for the .ics link, installs a launchd job (every 10 min)
~/.local/bin/crosspoint-cal-sync.sh --force   # run immediately
bash uninstall.sh
```

Settings: `~/.config/crosspoint-cal-sync.conf` (`ICS_URL`, `BOOK_HOST`,
`BOOK_PATH`, `MIN_DOWNLOAD_INTERVAL`). The file is uploaded to
`/plugins/calendar/calendar-full.ics` by default; change `BOOK_PATH` for another name.

Combine with a busy-only feed the reader can sync on its own (plugin.json
`"url"`, saved to a different `"file"`): the agenda lists each meeting once and
uses the title from the pushed file when there is one.

Keep feeds with private data on your own devices; do not relay them through
public file hosts.
