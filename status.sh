#!/bin/bash
# GhostGuard for the reMarkable Paper Pro: is it running? Run on your computer, tablet connected by USB and awake.
#     bash status.sh         a summary
#     bash status.sh log     the last lines of GhostGuard's own start-up log
set -eu
B="-o BatchMode=yes -o ConnectTimeout=8"
HOST=${GG_HOST:-}
if [ -z "$HOST" ]; then
    if ssh $B remarkable true 2> /dev/null; then HOST=remarkable
    elif ssh $B root@10.11.99.1 true 2> /dev/null; then HOST=root@10.11.99.1
    else echo "STOPPED: cannot reach the tablet (USB connected? awake?)" >&2; exit 1; fi
fi
if [ "${1:-}" = log ]; then ssh $B "$HOST" 'bash /home/root/ghostguard/boot/gg.sh log 40'; exit 0; fi
out=$(ssh $B "$HOST" 'bash /home/root/ghostguard/boot/gg.sh status 2>&1') || { echo "GhostGuard is not installed on this tablet."; exit 1; }
printf '%s\n' "$out"
echo
if printf '%s\n' "$out" | grep -q '^xochitl:.*ggsense yes'; then
    echo "RESULT: GhostGuard is running."
elif printf '%s\n' "$out" | grep -q '^disabled:   no$'; then
    echo "RESULT: GhostGuard is installed but not running right now. After a restart it waits about 70 s and then"
    echo "for 2 minutes without touch or pen input before it starts. If it stays off: bash status.sh log"
else
    echo "RESULT: GhostGuard is installed but switched off (see 'disabled' above). docs/TROUBLESHOOTING.md explains why."
fi
