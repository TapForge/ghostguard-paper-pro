#!/bin/bash
# Mac side: remove GhostGuard boot persistence from the tablet (runs deploy/boot/uninstall.sh there through
# `ssh ... bash -s`, so the current copy of it is used). Units off the root file system, rescue drop-in out of
# XOVI's template, this boot's ghostguard-autostart stopped; the running xochitl is left as it is unless --stock.
# Default is a DRY RUN (prints the command); --go runs it.
#   bash deploy/persist_uninstall.sh [--go] [--stock]
#     --stock   also XOVI off now (one xochitl restart) instead of at the next reboot
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
GO=0
ARGS=""
HOST=${GG_HOST:-remarkable}
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=10"
while [ $# -gt 0 ]; do
    case "$1" in
        --go) GO=1 ;;
        --dry-run) GO=0 ;;
        --stock) ARGS="--stock" ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) echo "persist_uninstall: unknown option $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
U=$P/deploy/boot/uninstall.sh
bash -n "$U" || { echo "persist_uninstall: syntax error in $U" >&2; exit 1; }
if [ "$GO" = 1 ]; then
    ssh $SSH_OPTS "$HOST" "bash -s -- $ARGS" < "$U"
else
    echo "DRY RUN: nothing is sent; with --go this runs:"
    printf '  ssh %s %s %q < %q\n' "$SSH_OPTS" "$HOST" "bash -s -- $ARGS" "$U"
fi
