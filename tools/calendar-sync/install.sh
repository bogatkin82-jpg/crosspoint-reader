#!/bin/bash
# macOS installer: bash install.sh
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$HOME/.local/bin/crosspoint-cal-sync.sh"
CONF="$HOME/.config/crosspoint-cal-sync.conf"
PLIST="$HOME/Library/LaunchAgents/ru.crosspoint.calsync.plist"

mkdir -p "$HOME/.local/bin" "$HOME/.config" "$HOME/Library/LaunchAgents"
install -m 755 "$DIR/crosspoint-cal-sync.sh" "$BIN"

if [ ! -f "$CONF" ]; then
  echo "Paste the iCal (.ics) link of your calendar:"
  read -r URL
  cat > "$CONF" <<CONFEOF
# iCal feed reachable from this computer (e.g. only on VPN)
ICS_URL='$URL'
# Reader address; use the IP shown on the File Transfer screen if crosspoint.local does not resolve
BOOK_HOST='crosspoint.local'
BOOK_PATH='/plugins/calendar/calendar-full.ics'
CONFEOF
  chmod 600 "$CONF"
fi

cat > "$PLIST" <<PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>ru.crosspoint.calsync</string>
  <key>ProgramArguments</key><array><string>/bin/bash</string><string>$BIN</string></array>
  <key>StartInterval</key><integer>600</integer>
  <key>RunAtLoad</key><true/>
</dict></plist>
PLISTEOF
launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"
echo "Done. Checks the feed and the reader every 10 minutes."
echo "Run now: $BIN --force"
echo "Log: ~/Library/Application Support/crosspoint-cal-sync/sync.log"
