#!/bin/bash
# Build and install qMarkable onto the tablet over SSH.
#   ./install.sh [ssh-host]        (default host: remarkable)
#   NO_RESTART=1 ./install.sh      copy files only; don't run ~/xovi/start
set -euo pipefail
HOST="${1:-remarkable}"
SSH_OPTS=(-o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)
ssh() { command ssh "${SSH_OPTS[@]}" "$@"; }
scp() { command scp "${SSH_OPTS[@]}" "$@"; }
cd "$(dirname "$0")"

make -s tracker

QRR=/home/root/xovi/exthome/qt-resource-rebuilder
echo "==> Checking device"
ssh "$HOST" 'grep -q "IMG_VERSION=\"3.28.0.172\"" /etc/os-release || { echo "Firmware is not 3.28.0.172 - the .qmd files target that version only." >&2; exit 1; }
             test -f /home/root/xovi/extensions.d/qt-resource-rebuilder.so || { echo "qt-resource-rebuilder not installed" >&2; exit 1; }'

echo "==> Copying files"
ssh "$HOST" "mkdir -p /home/root/qmarkable /home/root/.config/qmarkable /home/root/xovi/scripts/post-start /home/root/xovi/scripts/post-stock"
scp -q mods/*.qmd "$HOST:$QRR/"
scp -q tracker/qmarkable-trackerd "$HOST:/home/root/qmarkable/qmarkable-trackerd.new"
scp -q device/post-start/qmarkable.sh "$HOST:/home/root/xovi/scripts/post-start/qmarkable.sh"
scp -q device/post-stock/qmarkable.sh "$HOST:/home/root/xovi/scripts/post-stock/qmarkable.sh"
scp -q device/tracker.conf "$HOST:/home/root/qmarkable/tracker.conf.default"
ssh "$HOST" 'set -e
  cd /home/root/qmarkable
  chmod 755 qmarkable-trackerd.new /home/root/xovi/scripts/post-start/qmarkable.sh /home/root/xovi/scripts/post-stock/qmarkable.sh
  mv -f qmarkable-trackerd.new qmarkable-trackerd
  [ -f /home/root/.config/qmarkable/tracker.conf ] || cp tracker.conf.default /home/root/.config/qmarkable/tracker.conf'

if [ "${NO_RESTART:-}" = "1" ]; then
  echo "==> Installed. Run ~/xovi/start on the tablet to apply."
  exit 0
fi
echo "==> Restarting xovi (xochitl will restart)"
ssh "$HOST" '/home/root/xovi/start >/dev/null 2>&1; sleep 2; systemctl is-active qmarkable-tracker.service'
echo "==> Done"
