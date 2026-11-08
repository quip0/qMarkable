#!/bin/bash
# Remove qMarkable from the tablet. Recorded study data is kept unless
# PURGE=1 is set (it lives in /home/root/.local/share/qmarkable).
set -euo pipefail
HOST="${1:-remarkable}"
cd "$(dirname "$0")"
QMDS=$(for f in mods/*.qmd; do printf ' /home/root/xovi/exthome/qt-resource-rebuilder/%s' "$(basename "$f")"; done)
ssh "$HOST" "rm -f $QMDS"
ssh "$HOST" 'set -e
  systemctl stop qmarkable-tracker.service 2>/dev/null || true
  systemctl reset-failed qmarkable-tracker.service 2>/dev/null || true
  rm -f /home/root/xovi/scripts/post-start/qmarkable.sh \
        /home/root/xovi/scripts/post-stock/qmarkable.sh
  rm -rf /home/root/qmarkable /run/qmarkable'
if [ "${PURGE:-}" = "1" ]; then
  ssh "$HOST" 'rm -rf /home/root/.local/share/qmarkable /home/root/.config/qmarkable'
fi
ssh "$HOST" '/home/root/xovi/start >/dev/null 2>&1' || true
echo "Removed."
