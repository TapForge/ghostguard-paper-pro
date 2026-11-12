#!/bin/bash
# uninstall.sh [--stock]: remove GhostGuard boot persistence: the three units on the root file system (through a
# bind mount, like install.sh), the rescue drop-in in XOVI's template and in a running XOVI's drop-in tmpfs, and
# this boot's ghostguard-autostart. The running xochitl is left as it is, never restarted (the last line says what
# runs until the reboot), unless --stock (XOVI off now: one xochitl restart, checked; exit 1 when XOVI stays on).
# /home/root/ghostguard/boot stays (logs in state/) with the disable sentinel set, so nothing arms from it; remove
# it by hand if wanted. deploy/persist_uninstall.sh runs this file from the Mac (bash -s).
set -u
GG=${GG_HOME:-/home/root/ghostguard}
B=${GG_BOOT:-$GG/boot}
S=$B/state
X=${GG_XOVI:-/home/root/xovi}
TPL=$X/services/xochitl.service
DROP=${GG_DROP:-/etc/systemd/system/xochitl.service.d}
SO_MAPS=${GG_SO_MAPS:-/home/root/ghostguard/boot/ggsense.so}  # the boot's library as xochitl maps it
ETC=${GG_ETC:-/etc/systemd/system}
R=${GG_ROOTRW:-/tmp/gg-rootrw}
PROC=${GG_PROC:-/proc}
UNITS="ghostguard-autostart.service ghostguard-autostart.timer ghostguard-rescue.service"
WANT=timers.target.wants/ghostguard-autostart.timer
STOCK=0
[ "${1:-}" = "--stock" ] && STOCK=1
rc=0
num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }
mainpid() { num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)"; }
xovi_on() { grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null; }

systemctl stop ghostguard-autostart.timer ghostguard-autostart.service 2>/dev/null
mkdir -p "$S" 2>/dev/null && echo "$(date -u '+%FT%TZ') uninstalled" > "$S/disabled"
sync

# / read-write FIRST, then the bind mount (it copies the per-mount read-only flag of / when it is made; checked on
# the tablet 2026-11-09); undone in reverse
RW=0
BOUND=0
cleanup() {
    sync
    if [ "$BOUND" = 1 ]; then
        BOUND=0
        umount "$R" 2>/dev/null || echo "WARNING: cannot unmount $R"
        rmdir "$R" 2>/dev/null
    fi
    if [ "$RW" = 1 ]; then
        RW=0
        mount -o remount,ro / 2>/dev/null || { echo "WARNING: / could not be put back read-only (rw until reboot)"; rc=1; }
    fi
}
trap cleanup EXIT
if ! mkdir -p "$R" || ! { ! grep -q " $R " "$PROC/mounts" 2>/dev/null || umount "$R"; }; then
    echo "ERROR: cannot prepare $R; units not removed from the root file system"
    rc=1
elif ! mount -o remount,rw /; then
    echo "ERROR: cannot remount / read-write; units not removed from the root file system"
    rc=1
else
    RW=1
    if mount --bind / "$R"; then
        BOUND=1
        D=$R/etc/systemd/system
        rm -f "$D/$WANT"
        for u in $UNITS; do rm -f "$D/$u" "$D/$u.new"; done
        sync
        for u in $UNITS; do [ -e "$D/$u" ] && { echo "ERROR: $D/$u still there"; rc=1; }; done
        [ -e "$D/$WANT" ] || [ -L "$D/$WANT" ] && { echo "ERROR: $D/$WANT still there"; rc=1; }
    else
        echo "ERROR: bind mount of / on $R failed; units not removed from the root file system"
        rc=1
    fi
fi
cleanup
trap - EXIT

# this boot: overlay copies (if any), template, running drop-in tmpfs
rm -f "$ETC/$WANT"
for u in $UNITS; do rm -f "$ETC/$u"; done
rm -f "$TPL/zz-ghostguard.conf" "$TPL/30-ggsense.conf"
xovi_on && rm -f "$DROP/zz-ghostguard.conf"
systemctl daemon-reload 2>/dev/null

src=0
if [ "$STOCK" = 1 ]; then
    # XOVI off now, the way autostart.sh's stock_now() and rescue.sh do it: if xochitl still runs with XOVI after
    # xovi/stock, unmount and restart here; "XOVI off" only once checked
    systemctl reset-failed xochitl.service 2>/dev/null
    "$X/stock"
    if xovi_on; then
        echo "XOVI drop-in tmpfs still mounted after xovi/stock; unmounting it"
        umount "$DROP" 2>/dev/null
        systemctl daemon-reload 2>/dev/null
    fi
    systemctl reset-failed xochitl.service 2>/dev/null
    sleep 2
    p=$(mainpid)
    if [ "$p" -gt 0 ] && grep -q "/xovi.so" "$PROC/$p/maps" 2>/dev/null; then
        echo "xochitl $p still runs with XOVI after xovi/stock; restarting it"
        systemctl restart xochitl.service 2>/dev/null
    elif ! systemctl is-active --quiet xochitl.service 2>/dev/null; then
        systemctl start xochitl.service 2>/dev/null
    fi
    p=$(mainpid)
    px="xochitl $p"
    [ "$p" -gt 0 ] || px="xochitl (not running now)"
    if xovi_on || grep -q "/xovi.so" "$PROC/$p/maps" 2>/dev/null; then
        now="WARNING: XOVI still on (drop-in tmpfs $(xovi_on && echo mounted || echo gone), $px); reboot for stock xochitl"
        src=1
    else
        now="XOVI off (stock $px)"
    fi
else
    # no restart here: what runs until the reboot
    p=$(mainpid)
    px="xochitl $p"
    [ "$p" -gt 0 ] || px="xochitl (not running now)"
    what="no XOVI"
    if grep -q "/xovi.so" "$PROC/$p/maps" 2>/dev/null; then
        what="XOVI, no ggsense"
        if grep -q -F "$SO_MAPS" "$PROC/$p/maps" 2>/dev/null; then what="XOVI + the boot's ggsense"
        elif grep -q "/ggsense.so" "$PROC/$p/maps" 2>/dev/null; then what="XOVI + a ggsense deployed by hand"; fi
    fi
    if [ -e "$S/armed" ] && xovi_on && grep -q -F "$SO_MAPS" "$DROP/30-ggsense.conf" 2>/dev/null; then
        now="an arm was interrupted: XOVI + the boot's ggsense (unverified, no crash guard) stay in xochitl until reboot (now: $px with $what). XOVI off now: uninstall.sh --stock"
    elif [ "$what" != "no XOVI" ]; then
        now="$px with $what until reboot. XOVI off now: uninstall.sh --stock"
    elif xovi_on; then
        now="$px with no XOVI, but XOVI's drop-ins stay mounted until reboot (its next start loads them). XOVI off now: uninstall.sh --stock"
    else
        now="XOVI off (stock $px)"
    fi
fi
if [ "$rc" = 0 ]; then
    echo "UNINSTALLED: no GhostGuard units on the root file system; later boots stay stock."
else
    echo "UNINSTALL INCOMPLETE (see the errors above); the disable sentinel keeps later boots stock."
fi
echo "This boot: $now. Logs: $S"
[ "$src" = 0 ] || rc=1
exit "$rc"
