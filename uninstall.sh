#!/bin/bash
# GhostGuard for the reMarkable Paper Pro: remove it again. Run on your computer, tablet connected by USB and awake.
#
#     bash uninstall.sh            remove the auto-start; the tablet is back to normal after its next restart
#     bash uninstall.sh --now      the same, and switch back to the normal software right now (screen restarts once)
#
# What stays on the tablet (harmless, not started by anything): the folders /home/root/ghostguard, /home/root/gg-staging,
# /home/root/gg-backup and XOVI itself (/home/root/xovi). The last lines printed below tell you how to delete those too.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
B="-o BatchMode=yes -o ConnectTimeout=8"
HOST=${GG_HOST:-}
if [ -z "$HOST" ]; then
    if ssh $B remarkable true 2> /dev/null; then HOST=remarkable
    elif ssh $B root@10.11.99.1 true 2> /dev/null; then HOST=root@10.11.99.1
    else echo "STOPPED: cannot reach the tablet (USB connected? awake?)" >&2; exit 1; fi
fi
ARG=""
[ "${1:-}" = "--now" ] && ARG="--stock"
GG_HOST="$HOST" bash "$HERE/deploy/persist_uninstall.sh" --go $ARG
echo
echo "GhostGuard's auto-start is removed."
[ -n "$ARG" ] || echo "It still runs until the next restart of the tablet (or run: bash uninstall.sh --now)."
echo "To also delete its files (optional), after the tablet runs its normal software again:"
echo "  ssh $HOST 'rm -rf /home/root/ghostguard /home/root/gg-staging /home/root/gg-backup'"
echo "  ssh $HOST 'rm -f /home/root/xovi/exthome/qt-resource-rebuilder/ghostguard.qmd /home/root/xovi/services/xochitl.service/20-gglog.conf'"
echo "XOVI itself (/home/root/xovi) is left alone; it does nothing unless something starts it."
