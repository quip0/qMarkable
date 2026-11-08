#!/bin/sh
# Run by ~/xovi/start (after xochitl restarts). Starts the study tracker as a
# transient systemd unit: nothing is written to /etc, and it goes away on
# reboot together with xovi itself.
BIN=/home/root/qmarkable/qmarkable-trackerd
[ -x "$BIN" ] || exit 0

mkdir -p /run/qmarkable
systemctl stop qmarkable-tracker.service 2>/dev/null
systemctl reset-failed qmarkable-tracker.service 2>/dev/null
systemd-run --unit=qmarkable-tracker \
    --description="qMarkable study tracker" \
    --nice=19 \
    -p IOSchedulingClass=idle \
    -p Restart=on-failure -p RestartSec=10 \
    "$BIN" >/dev/null 2>&1
exit 0
