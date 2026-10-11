#!/bin/bash
# Downloads an iCal feed that only the computer can reach (VPN, corporate
# network, login cookies...) and uploads it to a CrossPoint reader over WebDAV
# while the reader shows "File Transfer". Runs from launchd every 10 minutes and
# exits quietly when the feed or the reader is unreachable.
set -u

CONF="$HOME/.config/crosspoint-cal-sync.conf"
[ -f "$CONF" ] && . "$CONF"
: "${ICS_URL:?ICS_URL is not set in $CONF}"
BOOK_HOST="${BOOK_HOST:-crosspoint.local}"
BOOK_PATH="${BOOK_PATH:-/plugins/calendar/calendar-full.ics}"
MIN_DOWNLOAD_INTERVAL="${MIN_DOWNLOAD_INTERVAL:-1800}"   # seconds between feed downloads

STATE="$HOME/Library/Application Support/crosspoint-cal-sync"
mkdir -p "$STATE"
CACHE="$STATE/calendar.ics"
UPLOADED_HASH="$STATE/uploaded.sha"
LOG="$STATE/sync.log"
log() { echo "$(date '+%F %T') $*" >> "$LOG"; tail -n 300 "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"; }
notify() { osascript -e "display notification \"$1\" with title \"CrossPoint calendar\"" >/dev/null 2>&1 || true; }

# 1. Fresh copy of the feed (succeeds only when the network allows it).
age=999999
[ -f "$CACHE" ] && age=$(( $(date +%s) - $(stat -f %m "$CACHE") ))
if [ "$age" -ge "$MIN_DOWNLOAD_INTERVAL" ] || [ "${1:-}" = "--force" ]; then
  tmp="$STATE/download.tmp"
  if curl -fsSL --max-time 60 -o "$tmp" "$ICS_URL" 2>/dev/null; then
    if head -c 200 "$tmp" | LC_ALL=C tr -d '\357\273\277\r\n\t ' | grep -qi '^BEGIN:VCALENDAR'; then
      mv "$tmp" "$CACHE"
      log "feed downloaded ($(wc -c < "$CACHE" | tr -d ' ') bytes)"
    else
      rm -f "$tmp"; log "not an iCal file - check ICS_URL in $CONF"
    fi
  else
    rm -f "$tmp"   # feed unreachable: skip quietly
  fi
fi
[ -f "$CACHE" ] || exit 0

# 2. Is the reader in File Transfer mode?
curl -fs --max-time 4 "http://$BOOK_HOST/api/status" >/dev/null 2>&1 || exit 0

hash=$(shasum -a 256 "$CACHE" | cut -d' ' -f1)
if [ "${1:-}" != "--force" ] && [ -f "$UPLOADED_HASH" ] && [ "$(cat "$UPLOADED_HASH")" = "$hash" ]; then
  exit 0   # reader already has this version
fi

# 3. WebDAV PUT straight into the plugin folder.
if curl -fsS --max-time 120 -T "$CACHE" "http://$BOOK_HOST$BOOK_PATH" >/dev/null 2>>"$LOG"; then
  echo "$hash" > "$UPLOADED_HASH"
  log "uploaded to reader: $BOOK_PATH"
  notify "Calendar uploaded to the reader"
else
  log "upload failed: $BOOK_HOST"
fi
