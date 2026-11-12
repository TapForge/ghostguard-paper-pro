#!/bin/bash
# GhostGuard for the reMarkable Paper Pro: change a setting of the running engine. Run on your computer, tablet
# connected by USB and awake.
#     bash settings.sh KEY VALUE        e.g.  bash settings.sh turn_full 0
# The change works at once and lasts until the tablet (or its main program) restarts; then the built-in defaults
# are back. docs/SETTINGS.md lists the keys that are meant to be changed.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# = 2 ] || { sed -n '2,6p' "$0"; exit 2; }
case "$1$2" in *[!A-Za-z0-9_.-]*) echo "STOPPED: odd characters in the key or value" >&2; exit 2 ;; esac
B="-o BatchMode=yes -o ConnectTimeout=8"
HOST=${GG_HOST:-}
if [ -z "$HOST" ]; then
    if ssh $B remarkable true 2> /dev/null; then HOST=remarkable
    elif ssh $B root@10.11.99.1 true 2> /dev/null; then HOST=root@10.11.99.1
    else echo "STOPPED: cannot reach the tablet (USB connected? awake?)" >&2; exit 1; fi
fi
GG_HOST="$HOST" python3 -I "$HERE/tools/ggq.py" --code "(function(){console.log('GGQ 9 set $1 $2');return 'sent set $1 $2';})()"
ssh $B "$HOST" 'journalctl -u xochitl --since "-25s" --no-pager -o cat | grep -a "GGSENSE.* -> " | tail -n 1'
