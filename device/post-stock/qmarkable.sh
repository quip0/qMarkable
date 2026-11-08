#!/bin/sh
# Run by ~/xovi/stock: stop the tracker so the tablet is fully stock.
systemctl stop qmarkable-tracker.service 2>/dev/null
systemctl reset-failed qmarkable-tracker.service 2>/dev/null
exit 0
