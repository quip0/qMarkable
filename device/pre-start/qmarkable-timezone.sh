#!/bin/sh
# Run by ~/xovi/start before xochitl restarts. The firmware has no timezone
# setting and /etc lives in RAM (reset on every boot), so re-apply the zone
# from ~/.config/qmarkable/timezone (e.g. "Asia/Taipei") each time.
# Empty or missing file: leave the system timezone alone.
CONF=/home/root/.config/qmarkable/timezone
[ -f "$CONF" ] || exit 0
TZNAME=$(tr -d ' \t\r\n' < "$CONF")
[ -n "$TZNAME" ] || exit 0
[ -f "/usr/share/zoneinfo/$TZNAME" ] || { echo "qmarkable: unknown timezone $TZNAME" >&2; exit 0; }
CURRENT=$(readlink /etc/localtime | sed 's|.*/zoneinfo/||')
[ "$CURRENT" = "$TZNAME" ] || timedatectl set-timezone "$TZNAME" 2>/dev/null || ln -sf "/usr/share/zoneinfo/$TZNAME" /etc/localtime
exit 0
