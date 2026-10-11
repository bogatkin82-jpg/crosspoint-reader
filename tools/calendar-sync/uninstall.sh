#!/bin/bash
PLIST="$HOME/Library/LaunchAgents/ru.crosspoint.calsync.plist"
launchctl unload "$PLIST" 2>/dev/null; rm -f "$PLIST" "$HOME/.local/bin/crosspoint-cal-sync.sh"
echo "Removed. Settings kept in ~/.config/crosspoint-cal-sync.conf"
