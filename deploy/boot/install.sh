#!/bin/bash
# install.sh [SRC]: install GhostGuard boot persistence from a staged folder (deploy/persist_install.sh sends it
# to /home/root/gg-staging/boot and runs this). Nothing changes for the running xochitl; from the next boot on,
# ghostguard-autostart.timer runs boot/autostart.sh 70 s after boot.
#   1. checks the staged files against MANIFEST.md5
#   2. stops this boot's ghostguard-autostart (an older install's monitor) and copies the files to
#      /home/root/ghostguard/boot, each by rename (a running copy keeps its old file), the boot's pinned copy of
#      the library (ggsense.so) included
#   3. puts the rescue drop-in (zz-ghostguard.conf) into XOVI's template
#   4. the gates of autostart.sh: would the next boot arm? If not: step 3 undone, nothing written to the root file
#      system
#   5. writes the three units to the real root file system through a bind mount (/etc is an overlay on tmpfs:
#      anything written there, `systemctl enable` included, is gone after a reboot), enables the timer by hand,
#      checks them on disk, puts / back read-only. Order (checked on the tablet, 2026-11-09): / read-write FIRST,
#      then the bind mount; a bind mount copies the per-mount read-only flag of / at the time it is made, so one
#      made before the remount stays read-only (EROFS). Undone in reverse: unmount the bind, / read-only.
#   6. only now resets state/: strikes 0, no disable sentinel, an old crash-guard rollback mark set aside.
# Before step 2 changes anything in boot/, an "install started" line goes into the disable sentinel (one already
# there, e.g. gg.sh disable, stays), and a failure from then on adds its reason: an install that does not finish
# (a failure, the gates, a lost ssh session, a power loss) leaves later boots stock, also where an older install's
# units are still enabled, until an install succeeds (step 6 removes the sentinel).
# Exit 0 installed, 1 failed (see the message), 3 the gates say the next boot would not arm (not installed).
set -u
umask 022
SRC=${1:-/home/root/gg-staging/boot}
GG=${GG_HOME:-/home/root/ghostguard}
B=${GG_BOOT:-$GG/boot}
S=$B/state
X=${GG_XOVI:-/home/root/xovi}
TPL=$X/services/xochitl.service
BK=${GG_BK:-/home/root/gg-backup}
R=${GG_ROOTRW:-/tmp/gg-rootrw}
PROC=${GG_PROC:-/proc}
UNITS="ghostguard-autostart.service ghostguard-autostart.timer ghostguard-rescue.service"
WANT=timers.target.wants/ghostguard-autostart.timer

STEP=1   # from step 2 on files in $B change (the disable sentinel is set): a failure adds its reason
fail() {
    echo "INSTALL FAILED: $*"
    if [ "$STEP" -lt 2 ]; then
        echo "(nothing in $B changed)"
    else
        echo "$(date -u '+%FT%TZ') install failed in step $STEP: $*; run the install again" >> "$S/disabled" 2>/dev/null
        sync
        echo "step $STEP: later boots do not arm ($S/disabled) until an install succeeds: run it again"
    fi
    exit 1
}
md5of() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# ---- 1. staged files
[ -f "$SRC/MANIFEST.md5" ] || fail "no $SRC/MANIFEST.md5"
( cd "$SRC" && md5sum -c MANIFEST.md5 > /dev/null 2>&1 ) || fail "staged files in $SRC do not match MANIFEST.md5"
for f in autostart.sh ggapply.sh rescue.sh gg.sh install.sh uninstall.sh ggtool gg_autorollback.sh ggsense.so \
         dropins/30-ggsense.conf dropins/zz-ghostguard.conf payload/kbd.cmd payload/clock.cmd payload/status.cmd \
         payload/splash.cmd payload/kbd.ok payload/clock.ok payload/status.ok payload/splash.ok; do
    [ -f "$SRC/$f" ] || fail "missing $SRC/$f"
done
for u in $UNITS; do [ -f "$SRC/units/$u" ] || fail "missing $SRC/units/$u"; done
# (the pattern is split so that persist_install.sh's filling does not rewrite it)
if grep -q -E '__SO''_MD5__|__GG''_VER__' "$SRC"/*.sh; then fail "placeholders not filled in $SRC"; fi
[ -x "$X/start" ] && [ -x "$X/stock" ] || fail "XOVI not found at $X"
[ -d "$TPL" ] || fail "no XOVI template $TPL"

# ---- 2. files
if systemctl is-active --quiet ghostguard-autostart.service 2>/dev/null; then
    systemctl stop ghostguard-autostart.service 2>/dev/null
    echo "stopped this boot's ghostguard-autostart (an older install's monitor)"
fi
mkdir -p "$B/units" "$B/dropins" "$B/payload" "$S" || fail "cannot create $B"
# the sentinel first, on disk: whatever stops this install from here on (also a lost session or a power loss, where
# fail() never runs), an older install's units never arm a half-installed $B
echo "$(date -u '+%FT%TZ') install started, not finished: run the install again" >> "$S/disabled" && sync ||
    fail "cannot write $S/disabled"
STEP=2
if [ "$(cd "$SRC" && pwd -P)" != "$(cd "$B" && pwd -P)" ]; then
    rm -f "$B/payload/"*
    for f in $(cd "$SRC" && find . -type f | sed 's|^\./||'); do
        mkdir -p "$(dirname "$B/$f")"
        cp -f "$SRC/$f" "$B/$f.new" && mv -f "$B/$f.new" "$B/$f" || fail "cannot copy $f to $B"
    done
fi
chmod 0755 "$B"/*.sh "$B/ggtool" "$B/ggsense.so"
chmod 0644 "$B"/units/* "$B"/dropins/* "$B"/payload/* "$B/MANIFEST.md5"
( cd "$B" && md5sum -c MANIFEST.md5 > /dev/null 2>&1 ) || fail "copied files in $B do not match MANIFEST.md5"
echo "files in $B"

# ---- 3. rescue drop-in in XOVI's template (never the library's drop-in: autostart.sh stages that per start)
STEP=3
cp -f "$B/dropins/zz-ghostguard.conf" "$TPL/zz-ghostguard.conf.new" && mv -f "$TPL/zz-ghostguard.conf.new" "$TPL/zz-ghostguard.conf" ||
    fail "cannot write $TPL/zz-ghostguard.conf"
rm -f "$TPL/30-ggsense.conf"

# ---- 4. gates
g=$(GG_BOOT=$B bash "$B/autostart.sh" --gates 2>&1)
grc=$?
echo "$g"
if [ "$grc" != 0 ]; then
    rm -f "$TPL/zz-ghostguard.conf"
    echo "$(date -u '+%FT%TZ') not installed in step 4: $g; run the install again" >> "$S/disabled" 2>/dev/null
    sync
    echo "NOT INSTALLED: the next boot would not arm ($g). Files stay in $B; nothing written to the root file system;"
    echo "later boots do not arm ($S/disabled) until an install succeeds."
    exit 3
fi

# ---- 5. units on the real root file system
STEP=5
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
        mount -o remount,ro / 2>/dev/null || echo "WARNING: / could not be put back read-only (it stays rw until reboot)"
    fi
}
trap cleanup EXIT
mkdir -p "$R" || fail "cannot create $R"
if grep -q " $R " "$PROC/mounts" 2>/dev/null; then umount "$R" 2>/dev/null; fi
# / read-write first: the bind mount below copies the per-mount flags of / as they are now
mount -o remount,rw / || fail "cannot remount / read-write"
RW=1
mount --bind / "$R" || fail "bind mount of / on $R failed"
BOUND=1
D=$R/etc/systemd/system
[ -d "$D" ] || fail "$D missing (not the root file system?)"
touch "$D/.gg-write-test" 2>/dev/null && rm -f "$D/.gg-write-test" || fail "$D is not writable through the bind mount"
for u in $UNITS; do
    cp -f "$B/units/$u" "$D/$u.new" && chmod 0644 "$D/$u.new" && mv -f "$D/$u.new" "$D/$u" || fail "cannot write $D/$u"
done
# enable by hand, last. Not `systemctl enable`: it would write through the /etc overlay into tmpfs and not survive
# the reboot. (An older install's units stay enabled whatever fails here and would run the new files in $B at the
# next boot: the disable sentinel from step 2 stops them.)
mkdir -p "$D/timers.target.wants" || fail "cannot create $D/timers.target.wants"
ln -sf /etc/systemd/system/ghostguard-autostart.timer "$D/$WANT" || fail "cannot link $D/$WANT"
sync
for u in $UNITS; do
    [ "$(md5of "$D/$u")" = "$(md5of "$B/units/$u")" ] || fail "check failed: $D/$u differs on disk"
done
[ "$(readlink "$D/$WANT")" = /etc/systemd/system/ghostguard-autostart.timer ] || fail "check failed: $D/$WANT"
cleanup
trap - EXIT
ro=$(awk '$2 == "/" { o = $4 } END { print o }' "$PROC/mounts" 2>/dev/null)
case "$ro" in ro|ro,*) ;; *) echo "WARNING: / is mounted '$ro', not read-only" ;; esac

# ---- 6. state: enabled, strikes 0
rm -f "$S/armed" "$S/good" "$S/disabled" "$S/probation" "$S/crashed"
echo 0 > "$S/strikes"
if [ -e "$BK/30-ggsense.conf.autorollback" ]; then
    mv -f "$BK/30-ggsense.conf.autorollback" "$BK/30-ggsense.conf.autorollback.$(date +%s).pre-install"
    echo "NOTE: an older crash-guard rollback mark was set aside (it would have counted as a strike)"
fi
echo "state: strikes 0, enabled"

echo "$(date -u '+%FT%TZ') $(sed -n 's/^VER=\${GG_VER:-\(.*\)}.*/\1/p' "$B/ggapply.sh" | head -n 1), units on the root file system" > "$S/installed"
sync
echo "INSTALLED: from the next boot on, ghostguard-autostart.timer arms XOVI + ggsense 70 s after boot"
echo "(after 2 min without touch or pen input; one xochitl restart). This boot is unchanged."
echo "status: bash $B/gg.sh status    off for later boots: bash $B/gg.sh disable    remove: uninstall.sh"
exit 0
