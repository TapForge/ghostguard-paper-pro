#!/bin/bash
# ghostguard-autostart: arm XOVI + ggsense once per boot, then keep its runtime QML applied.
# reMarkable Paper Pro, firmware 3.29.1 (the tested build only). Started by ghostguard-autostart.timer 70 s after
# boot, after rm-sanity (60 s) has reset the bootloader's root partition error counter: nothing done here can make
# the bootloader fall back to the other root partition. Never blocks the boot; every path exits 0.
#
#  1. strikes from the previous boot, then the gates: disabled sentinel, tested firmware / xochitl / ggsense,
#     XOVI's files, /home mounted, the bootloader's fallback state clear (rm-sanity ran successfully in this boot
#     and reset the boot error counter, no firmware update pending; waits up to SANITY_WAIT s for it)
#  2. waits until nobody touched the screen or used the pen for IDLE_S (120 s) while the display is awake (arming
#     restarts xochitl once: never under a reader, never under the sleep screen in the ~37 s before the tablet
#     suspends); a window in which the display slept or woke (power button, folio cover: ggtool sees neither and
#     counts no suspended time) or xochitl restarted starts again. Then the gates again while ggtool watches the
#     input for 3 s, then 3 s more of input watch after them, and right before the restart the display still awake;
#     any input or sleep: back to waiting. When the pen and touch input cannot be watched (devices not found, ggtool
#     failing), this boot does not arm: never a blind restart
#  3. starts XOVI with ggsense (30-ggsense.conf goes into XOVI's template only for this one start); the library is
#     the boot's own pinned copy, boot/ggsense.so: a library deployed by hand never changes what boots
#  4. health watch: xochitl up and not restarting by itself, ggsense in the new process, the GhostGuard QML loaded
#       healthy   -> probation, 15 min crash guard (gg_autorollback.sh) and the runtime QML (ggapply.sh), step 5
#       crashed, failed or down -> XOVI off at once (stock xochitl, one restart) and a strike
#       passive (xochitl runs, nothing crashed, but no ggsense or no GhostGuard QML) -> a strike; XOVI off when idle
#  5. monitor: probation ends after GOOD_ROUNDS x MON_TICK (15 min) of awake time without a crash: the arm is
#     good. A crash of xochitl (systemd's restart, a crash line) marks the boot crashed (no runtime QML again) and
#     turns XOVI off when idle. A restart that is no crash (by hand) gets the runtime QML again; one into a process
#     without the boot's library (a deploy by hand, stock) ends the probation without a verdict
# XOVI off when idle (steps 4 and 5): 30-ggsense.conf leaves the running drop-ins at once, without a restart
# (systemd's next start of xochitl, e.g. after a crash, comes without ggsense; after a crash a crash guard still on
# goes first: its rollback restarts xochitl without ggsense at once), then one restart into stock xochitl at the next
# idle moment (step 2's wait), unless a deploy by hand came meanwhile (left alone); when the input cannot be watched,
# XOVI stays on until the next boot.
# Strikes, counted at the next boot, one per boot at most: an arm that never got healthy (crash, reboot, power
# loss), a boot that ended inside the probation (rm-emergency's reboot, power loss, a freeze; a clean shutdown,
# gg.sh stop or a change by hand removes the probation mark), a crash seen by the monitor, the crash guard's
# rollback of the boot's library. A good arm clears them. MAXSTRIKES (2): disabled until gg.sh enable.
# XOVI already started by hand (its drop-in tmpfs is mounted): with the boot's ggsense in xochitl only step 5, else
# nothing (a deploy by hand, with its own library, is left alone; so is its runtime QML).
#   autostart.sh [--dry-run | --gates]
#     --dry-run  report what would happen, change nothing
#     --gates    only the checks of step 1 (no strikes, no lock): exit 0 when the next boot may arm, else 1
set -u
umask 022

B=${GG_BOOT:-/home/root/ghostguard/boot}
S=$B/state
X=${GG_XOVI:-/home/root/xovi}
TPL=$X/services/xochitl.service
DROP=${GG_DROP:-/etc/systemd/system/xochitl.service.d}
SO=${GG_SO:-$B/ggsense.so}                                    # the boot's pinned copy of the library
SO_MAPS=${GG_SO_MAPS:-/home/root/ghostguard/boot/ggsense.so}  # its path as xochitl maps it (dropins/30-ggsense.conf)
QMD=${GG_QMD:-$X/exthome/qt-resource-rebuilder/ghostguard.qmd}
CTL=${GG_CTL:-/home/root/ghostguard/control.json}
RUN=${GG_RUN:-/run/ghostguard}
BK=${GG_BK:-/home/root/gg-backup}
PROC=${GG_PROC:-/proc}
LPGPR=${GG_LPGPR:-/sys/devices/platform/lpgpr}   # root_part, root<a|b>_errcnt, swu_applied (rm-sanity.sh)
SANITY_WAIT=${GG_SANITY_WAIT:-60}                # s to wait for rm-sanity's reset (it runs 60 s after boot)
HOME_MNT=${GG_HOME_MNT:-/home}                   # its own file system (checked on the tablet: /dev/mapper/home-...)
RM_EMERG=${GG_RM_EMERG:-/tmp/rm-emergency}       # rm-emergency.service touches it before its reboot
VERSION_FILE=${GG_VERSION_FILE:-/etc/version}
XOCHITL=${GG_XOCHITL:-/usr/bin/xochitl}
TICK=${GG_TICK:-2}                 # seconds between health checks
HEALTH_ROUNDS=${GG_HEALTH_ROUNDS:-75}
IDLE_S=${GG_IDLE_S:-120}           # s without touch or pen input before the restart
MON_TICK=${GG_MON_TICK:-10}
MON_ROUNDS=${GG_MON_ROUNDS:-0}     # 0 = until the service stops
GOOD_ROUNDS=${GG_GOOD_ROUNDS:-90}  # monitor rounds of probation: 90 x 10 s = 15 min of awake time
MAXSTRIKES=2
# the tested build; an update replaces the root file system (and these units with it), the checks stop a stale copy
OS_TESTED=${GG_OS_TESTED:-20261028104944}
XO_MD5=${GG_XO_MD5:-50f86a58704d2b4c4eab20df75e093f3}
# filled in by persist_install.sh from the library it installs (an unfilled copy never passes the gates)
SO_MD5=${GG_SO_MD5:-__SO_MD5__}
VER=${GG_VER:-__GG_VER__}
BANNER="XOVI-GG71 v7.1 loaded"
CMDLINE='^ "cmd": [{]"id": [0-9]+, .*[}][[:space:]]*,?[[:space:]]*$'   # the whole cmd object on one line
CRASH='code=dumped|status=(4/ILL|6/ABRT|7/BUS|8/FPE|11/SEGV)|Failed with result|Start request repeated too quickly|Watchdog timeout'

DRY=0
MODE=arm
case "${1:-}" in
    --dry-run) DRY=1 ;;
    --gates) DRY=1; MODE=gates ;;
esac

mkdir -p "$S" "$RUN" 2>/dev/null
LOGF=$S/boot.log
if [ "$DRY" = 0 ]; then
    for f in "$LOGF" "$S/apply.out" "$S/xovi-start.log" "$S/xovi-stock.log"; do
        if [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 262144 ]; then mv -f "$f" "$f.1"; fi
    done
fi
log() {
    echo "$*"
    [ "$DRY" = 1 ] || echo "$(date -u '+%F %T') [$$] $*" >> "$LOGF"
}
num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }
unit_show() { systemctl show xochitl.service -p "$1" --value 2>/dev/null; }
md5of() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }
has_gg() { grep -q -F "$SO_MAPS" "$PROC/$1/maps" 2>/dev/null; }   # the boot's copy, not one deployed by hand
xovi_on() { grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null; }
disarm() {
    log "DISABLED: $*"
    [ "$DRY" = 1 ] && return 0
    echo "$(date -u '+%FT%TZ') $*" > "$S/disabled"
    sync
}
unstage() { [ "$DRY" = 1 ] || rm -f "$TPL/30-ggsense.conf"; }

# XOVI off: xovi/stock (unmounts the drop-in tmpfs, daemon-reload, restarts xochitl). If xochitl still runs with
# XOVI after that (stock script missing or failed), unmount and restart here. The lock fd is not inherited.
stock_now() { # $1 = log file
    local p
    systemctl reset-failed xochitl.service 2>/dev/null
    "$X/stock" >> "$1" 2>&1 9>&-
    if xovi_on; then
        log "XOVI drop-in tmpfs still mounted after xovi/stock; unmounting it here"
        umount "$DROP" 2>/dev/null
        systemctl daemon-reload 2>/dev/null
    fi
    systemctl reset-failed xochitl.service 2>/dev/null
    sleep 2
    p=$(num "$(unit_show MainPID)")
    if [ "$p" -gt 0 ] && grep -q "/xovi.so" "$PROC/$p/maps" 2>/dev/null; then
        log "xochitl $p still runs with XOVI after xovi/stock; restarting it"
        systemctl restart xochitl.service 2>/dev/null
    elif ! systemctl is-active --quiet xochitl.service 2>/dev/null; then
        systemctl start xochitl.service 2>/dev/null
    fi
}

# ---- strikes from the previous boot (once per run, before anything else)
strikes=0
account() {
    local bad="" rb=$BK/30-ggsense.conf.autorollback
    strikes=$(num "$(cat "$S/strikes" 2>/dev/null)")
    [ "$strikes" -lt 0 ] && strikes=0
    [ -e "$S/armed" ] && bad="armed ($(cat "$S/armed" 2>/dev/null)) and never got healthy"
    [ -e "$S/probation" ] &&
        bad="${bad:+$bad; }healthy ($(cat "$S/probation" 2>/dev/null)) but the boot ended inside the probation"
    [ -e "$S/crashed" ] && bad="${bad:+$bad; }xochitl crashed with ggsense ($(head -n 1 "$S/crashed" 2>/dev/null))"
    # the crash guard's mark is a copy of the drop-in it took away: only the boot's own library counts here
    # (a guard started by a deploy by hand rolls back that deploy's library)
    if [ -e "$rb" ]; then
        if grep -q -F "$SO_MAPS" "$rb" 2>/dev/null; then
            bad="${bad:+$bad; }the crash guard rolled the boot's ggsense back"
        else
            log "a crash-guard rollback mark of a deploy by hand (not $SO_MAPS): set aside, no strike"
        fi
    fi
    if [ -n "$bad" ]; then
        strikes=$((strikes + 1))
        log "previous arm: $bad -> strike $strikes/$MAXSTRIKES"
    elif [ -e "$S/good" ]; then
        [ "$strikes" -gt 0 ] && log "previous arm was good: strikes $strikes -> 0"
        strikes=0
    fi
    if [ "$DRY" = 0 ]; then
        rm -f "$S/armed" "$S/good" "$S/probation" "$S/crashed"
        [ -e "$rb" ] && mv -f "$rb" "$rb.$(date +%s)"
        echo "$strikes" > "$S/strikes"
        sync
    fi
    if [ "$strikes" -ge "$MAXSTRIKES" ] && [ ! -e "$S/disabled" ]; then
        disarm "auto-disabled after $strikes strikes"
    fi
}

# ---- the bootloader's fallback state (prints the reason when not clear). rm-sanity.service (60 s after boot) resets
# the error counter of the running root partition and swu_applied; rm-emergency.sh (xochitl's OnFailure) falls back
# to the other partition only while swu_applied is 1. Arming only while both are clear, after rm-sanity has run
# successfully in this boot (before it ran its Result is "success" too, but its exit time is 0), means a crash of
# xochitl with ggsense can only ever reboot into this same partition.
counters() {
    local part r x
    r=$(systemctl show rm-sanity.service -p Result --value 2>/dev/null)
    x=$(num "$(systemctl show rm-sanity.service -p ExecMainExitTimestampMonotonic --value 2>/dev/null)")
    [ "$r" = success ] && [ "$x" -gt 0 ] ||
        { echo "rm-sanity has not run successfully in this boot (Result '$r', exit time $x)"; return 1; }
    part=$(cat "$LPGPR/root_part" 2>/dev/null)
    case "$part" in a|b) ;; *) echo "the boot partition is unknown ($LPGPR/root_part: '$part')"; return 1 ;; esac
    [ "$(cat "$LPGPR/swu_applied" 2>/dev/null)" = 0 ] || { echo "a firmware update is pending (swu_applied is not 0)"; return 1; }
    [ "$(cat "$LPGPR/root${part}_errcnt" 2>/dev/null)" = 0 ] ||
        { echo "the boot error counter of root $part is not 0 (rm-sanity has not reset it)"; return 1; }
    return 0
}

# ---- gates (prints the reason on failure)
gates() {
    local f v
    counters || return 1
    awk -v m="$HOME_MNT" '$2 == m { f = 1 } END { exit !f }' "$PROC/mounts" 2>/dev/null ||
        { echo "$HOME_MNT is not mounted"; return 1; }
    v=$(cat "$VERSION_FILE" 2>/dev/null)
    [ "$v" = "$OS_TESTED" ] || { echo "firmware '$v' is not the tested $OS_TESTED"; return 1; }
    [ "$(md5of "$XOCHITL")" = "$XO_MD5" ] || { echo "$XOCHITL is not the tested build"; return 1; }
    [ "$(md5of "$SO")" = "$SO_MD5" ] || { echo "$SO is not the tested $VER build ($SO_MD5)"; return 1; }
    for f in "$X/start" "$X/stock" "$B/ggapply.sh" "$B/ggtool"; do
        [ -x "$f" ] || { echo "missing or not executable: $f"; return 1; }
    done
    command -v flock > /dev/null 2>&1 || { echo "flock not found"; return 1; }
    [ -x /bin/bash ] || { echo "/bin/bash not found (the units run it)"; return 1; }
    [ "$(evdevs "Elan marker input" "Elan touch input" | grep -c '^/dev/input/event')" = 2 ] ||
        { echo "the pen and touch devices are not both found in $PROC/bus/input/devices"; return 1; }
    for f in "$X/xovi.so" "$X/extensions.d/qt-resource-rebuilder.so" "$QMD" \
             "$X/exthome/qt-resource-rebuilder/hashtab" "$TPL/20-gglog.conf" "$TPL/zz-ghostguard.conf" \
             "$B/dropins/30-ggsense.conf" "$B/gg_autorollback.sh" \
             "$B/payload/kbd.cmd" "$B/payload/clock.cmd" "$B/payload/status.cmd" "$B/payload/splash.cmd"; do
        [ -f "$f" ] || { echo "missing: $f"; return 1; }
    done
    grep -q -F "$SO_MAPS" "$B/dropins/30-ggsense.conf" 2>/dev/null || { echo "30-ggsense.conf does not preload $SO_MAPS"; return 1; }
    # the GhostGuard QML whose banner and result lines the health watch and ggapply.sh read ("XOVI-GG71 " + text)
    for v in 'XOVI-GG71 ' 'v7.1 loaded' 'EVAL [' 'command error'; do
        grep -q -F "$v" "$QMD" 2>/dev/null || { echo "$QMD has no '$v' (not the GhostGuard v7.1 QML)"; return 1; }
    done
    # ggapply.sh rewrites the cmd line in place: it must hold the whole cmd object (tools/ggctl.py's format;
    # json.dumps(indent=1), e.g. tools/ggcmd.py, spreads it over several lines)
    [ "$(grep -c '^ "cmd": ' "$CTL" 2>/dev/null)" = 1 ] && grep -q -E "$CMDLINE" "$CTL" 2>/dev/null ||
        { echo "no one-line cmd object in $CTL"; return 1; }
    [ "$(systemctl is-active xochitl.service 2>/dev/null)" = active ] || { echo "xochitl is not active"; return 1; }
    return 0
}

# ---- input devices by name (event numbers are not fixed)
evdevs() {
    local n
    for n in "$@"; do
        awk -v want="N: Name=\"$n\"" '
            $0 == want { m = 1; next }
            m && /^H: Handlers=/ { sub(/^H: Handlers=/, ""); for (i = 1; i <= NF; i++) if ($i ~ /^event[0-9]+$/) print "/dev/input/" $i; m = 0 }
            /^$/ { m = 0 }' "$PROC/bus/input/devices" 2>/dev/null
    done
}
# the last display state xochitl logged in this boot ("" when none); rm.batterymanager logs it at info level:
#   "Changing display state from Normal to DeepSleep (setDisplayState ...)"
display_state() { # $1 = xochitl pid
    journalctl -b _PID="$1" -o cat --no-pager 2>/dev/null | grep -a "Changing display state from " | tail -n 1 |
        sed -n 's/.*Changing display state from [A-Za-z]* to \([A-Za-z]*\).*/\1/p'
}
# how many display state changes xochitl $1 logged in this boot (a sleep and its wake are two)
display_changes() { # $1 = xochitl pid
    journalctl -b _PID="$1" -o cat --no-pager 2>/dev/null | grep -a -c "Changing display state from "
}
# returns 1 when the input cannot be watched: then this boot does not arm (a restart could hit a pen stroke)
DEVS=""
idle_wait() {
    local d n=0 rc st said="" fails=0 pid np dc
    DEVS=$(evdevs "Elan marker input" "Elan touch input")
    for d in $DEVS; do n=$((n + 1)); done
    if [ "$n" != 2 ]; then
        log "pen and touch devices not both found ('$(echo $DEVS)'); cannot watch the input"
        return 1
    fi
    log "waiting for $IDLE_S s without touch or pen input ($(echo $DEVS))"
    while :; do
        # ggtool counts awake time only (CLOCK_MONOTONIC stops while the tablet is suspended) and sees neither the
        # power button nor the folio cover: a sleep and a wake inside the window could end it seconds after the
        # wake. A window counts only when xochitl and its display state stayed the same all through it
        pid=$(num "$(unit_show MainPID)")
        dc=$(display_changes "$pid")
        "$B/ggtool" quiet "$IDLE_S" 0 $DEVS 9>&-
        rc=$?
        if [ "$rc" != 0 ]; then
            fails=$((fails + 1))
            log "input watch failed (exit $rc, $fails/2)"
            [ "$fails" -ge 2 ] && return 1
            sleep 5
            continue
        fi
        np=$(num "$(unit_show MainPID)")
        if [ "$np" != "$pid" ]; then
            log "xochitl restarted during the wait ($pid -> $np); waiting $IDLE_S s again"
            continue
        fi
        if [ "$(display_changes "$pid")" != "$dc" ]; then
            log "the display slept or woke during the wait (power button or cover); waiting $IDLE_S s again"
            said=""
            continue
        fi
        # the power button or the folio cover puts xochitl to sleep without any touch: a restart then would wake
        # the screen into the library and keep the tablet up; wait for the display to come back first
        st=$(display_state "$pid")
        case "$st" in ''|Normal) return 0 ;; esac
        [ "$said" = "$st" ] || log "display state $st (asleep); arming waits until it is awake and idle again"
        said=$st
    done
}
# right before the restart (after idle_wait, the gates and the input watches beside and after them, which see neither
# the power button nor the folio cover): the display still awake? Prints the reason when not.
pre_restart() {
    local st
    st=$(display_state "$(num "$(unit_show MainPID)")")
    case "$st" in ''|Normal) return 0 ;; esac
    echo "display state $st right before the restart"
    return 1
}
# XOVI off when idle (a passive arm, a crash seen by the monitor), no restart now: 30-ggsense.conf leaves the
# running drop-in tmpfs at once (renamed: systemd ignores a name not ending in .conf), so systemd's next start of
# xochitl, e.g. its automatic restart after a crash, comes without ggsense; XOVI off (one restart) at the next idle
# moment, unless a deploy by hand came meanwhile
stock_when_idle() { # $1 = suffix for the renamed drop-in
    local w p
    if xovi_on && [ -f "$DROP/30-ggsense.conf" ]; then
        if mv -f "$DROP/30-ggsense.conf" "$DROP/30-ggsense.conf.$1" 2>/dev/null; then
            systemctl daemon-reload 2>/dev/null
            log "30-ggsense.conf out of xochitl's running drop-ins (no restart now: its next start comes without ggsense)"
        else
            log "WARNING: cannot rename $DROP/30-ggsense.conf: xochitl's next start still loads ggsense"
        fi
    fi
    if idle_wait; then
        if xovi_on; then
            # a deploy by hand since (a 30-ggsense.conf or a ggsense in xochitl that is not the boot's copy): left alone
            p=$(num "$(unit_show MainPID)")
            if { [ -f "$DROP/30-ggsense.conf" ] && ! grep -q -F "$SO_MAPS" "$DROP/30-ggsense.conf" 2>/dev/null; } ||
                { grep -q "/ggsense.so" "$PROC/$p/maps" 2>/dev/null && ! has_gg "$p"; }; then
                log "a deploy by hand since (its own ggsense; xochitl $p): left alone, XOVI stays on"
            else
                stock_now "$S/xovi-stock.log"
                log "XOVI off (stock xochitl)"
            fi
        fi
    elif xovi_on; then
        w="no runtime QML; xochitl's next start comes without ggsense"
        [ -f "$DROP/30-ggsense.conf" ] && w="no runtime QML: ggsense passive"
        log "the input cannot be watched: XOVI stays on until the next boot ($w)"
    fi
}

APPLIED_PID=0
apply_qml() { # $1 = xochitl pid
    "$B/ggapply.sh" "$1" >> "$S/apply.out" 2>&1 9>&-
    log "runtime QML for xochitl $1: $(cat "$RUN/apply.result" 2>/dev/null || echo 'no result')"
    APPLIED_PID=$1
}

# ---- arm: XOVI + ggsense, one xochitl restart, health watch
HEALTHY_NR=-1
HEALTHY_T=""
GUARD=""   # this arm's crash guard (its systemd-run unit)
arm() {
    local t0 e0 oldpid base rc i pid st nr first=0 why ok=0 hit kind
    t0=$(date '+%Y-%m-%d %H:%M:%S')
    e0=$(date +%s)
    oldpid=$(num "$(unit_show MainPID)")
    echo "$(date -u '+%FT%TZ') boot $(cat "$PROC/sys/kernel/random/boot_id" 2>/dev/null)" > "$S/armed"
    sync
    if ! cp -f "$B/dropins/30-ggsense.conf" "$TPL/30-ggsense.conf"; then
        log "cannot stage 30-ggsense.conf; not arming"
        rm -f "$S/armed"
        return 1
    fi
    rm -f "$RUN/apply.result"
    log "arming XOVI + $VER (strikes $strikes, xochitl pid $oldpid)"
    "$X/start" >> "$S/xovi-start.log" 2>&1 9>&-
    rc=$?
    unstage
    base=$(num "$(unit_show NRestarts)")
    log "xovi/start returned $rc (NRestarts now $base)"
    if [ "$rc" != 0 ] && ! xovi_on && [ "$(num "$(unit_show MainPID)")" = "$oldpid" ]; then
        # it stopped before it restarted xochitl: nothing to undo, and no xovi/stock restart for nothing
        strikes=$((strikes + 1))
        echo "$strikes" > "$S/strikes"
        rm -f "$S/armed"
        sync
        log "xovi/start failed before restarting xochitl (exit $rc); xochitl untouched; strike $strikes/$MAXSTRIKES"
        [ "$strikes" -ge "$MAXSTRIKES" ] && disarm "auto-disabled after $strikes strikes (last: xovi/start exit $rc)"
        return 1
    fi
    # kind of failure: crash (failed, restarted, a crash line) and down (not active) -> XOVI off at once;
    # passive (xochitl runs and nothing crashed: not restarted, no ggsense, no QML) -> XOVI off when idle
    why="no healthy xochitl within $((HEALTH_ROUNDS * TICK)) s"
    kind=down
    i=0
    while [ "$i" -lt "$HEALTH_ROUNDS" ]; do
        i=$((i + 1))
        sleep "$TICK"
        st=$(systemctl is-active xochitl.service 2>/dev/null)
        nr=$(num "$(unit_show NRestarts)")
        pid=$(num "$(unit_show MainPID)")
        hit=$(journalctl -b -u xochitl.service -p notice --since "$t0" --no-pager -q -o cat 2>/dev/null | grep -a -E "$CRASH" | head -n 1)
        # (an unreadable NRestarts, -1, never counts as a restart)
        if [ "$st" = failed ] || { [ "$base" -ge 0 ] && [ "$nr" -gt "$base" ]; } || [ -n "$hit" ]; then
            why="xochitl failed or restarted by itself (state $st, NRestarts $nr${hit:+, $hit})"
            kind=crash
            break
        fi
        if [ "$st" != active ] || [ "$pid" -le 0 ]; then
            why="no healthy xochitl within $((HEALTH_ROUNDS * TICK)) s (state $st)"
            kind=down
            continue
        fi
        if [ "$pid" = "$oldpid" ]; then
            why="xovi/start did not restart xochitl (still pid $oldpid)"
            kind=passive
            continue
        fi
        if [ "$first" = 0 ]; then
            first=$pid
        elif [ "$pid" != "$first" ]; then
            why="xochitl restarted ($first -> $pid)"
            kind=crash
            break
        fi
        if ! has_gg "$pid"; then
            why="ggsense is not in xochitl $pid"
            kind=passive
            continue
        fi
        if journalctl -b _PID="$pid" -o cat --no-pager 2>/dev/null | grep -a -q "$BANNER"; then
            ok=1
            break
        fi
        why="the GhostGuard QML did not load in xochitl $pid"
        kind=passive
    done
    if [ "$ok" != 1 ]; then
        strikes=$((strikes + 1))
        echo "$strikes" > "$S/strikes"
        rm -f "$S/armed"
        sync
        if [ "$kind" = passive ]; then
            # nothing crashed and xochitl runs: no restart under the user now
            log "UNHEALTHY: $why; strike $strikes/$MAXSTRIKES; xochitl runs and nothing crashed: XOVI goes off at the next idle moment"
            [ "$strikes" -ge "$MAXSTRIKES" ] && disarm "auto-disabled after $strikes strikes (last: $why)"
            stock_when_idle passive
            return 1
        fi
        log "UNHEALTHY: $why; XOVI off (stock xochitl)"
        stock_now "$S/xovi-stock.log"
        log "strike $strikes/$MAXSTRIKES"
        [ "$strikes" -ge "$MAXSTRIKES" ] && disarm "auto-disabled after $strikes strikes (last: $why)"
        return 1
    fi
    # probation: good only after GOOD_ROUNDS monitor rounds without a crash (see monitor)
    rm -f "$S/armed"
    echo "$(date -u '+%FT%TZ') xochitl $pid" > "$S/probation"
    sync
    # the monitor goes on from what the health watch checked (NRestarts since xovi/start, crash lines since t0):
    # a crash in the moments since that check is still a crash, not a restart by hand
    HEALTHY_NR=$base
    HEALTHY_T=$t0
    log "HEALTHY: xochitl $pid runs $VER and the GhostGuard QML (probation: good after $((GOOD_ROUNDS * MON_TICK / 60)) min of awake time without a crash)"
    if systemd-run --unit="gg-autorollback-$e0" --collect --quiet --setenv=GG_SO="$SO" \
            /bin/sh "$B/gg_autorollback.sh" "$t0" 9>&-; then
        GUARD=gg-autorollback-$e0
        log "crash guard on (15 min of awake time)"
    else
        log "WARNING: crash guard not started"
    fi
    apply_qml "$pid"
    return 0
}

# ---- monitor: ends the probation; a crash marks the boot crashed (one strike at the next boot, no runtime QML
# again) and turns XOVI off when idle; a new xochitl process that is no crash (a restart by hand) gets the runtime
# QML again, or, when it does not run the boot's library (a deploy by hand, stock), ends the probation without a
# verdict and stops the monitor
monitor() {
    local last=$1 pid nr hit i=0 w base=$HEALTHY_NR since=$HEALTHY_T
    [ -n "$since" ] || since=$(date '+%Y-%m-%d %H:%M:%S')
    log "monitor on: re-applies the runtime QML when xochitl restarts with ggsense (now pid $last)"
    while :; do
        i=$((i + 1))
        if [ "$MON_ROUNDS" -gt 0 ] && [ "$i" -gt "$MON_ROUNDS" ]; then return 0; fi
        sleep "$MON_TICK"
        if [ -e "$S/disabled" ]; then log "monitor: disabled; stops"; return 0; fi
        nr=$(num "$(unit_show NRestarts)")
        pid=$(num "$(unit_show MainPID)")
        hit=""
        if [ "$pid" -gt 0 ] && [ "$pid" != "$last" ] || { [ "$base" -ge 0 ] && [ "$nr" -gt "$base" ]; }; then
            hit=$(journalctl -b -u xochitl.service -p notice --since "$since" --no-pager -q -o cat 2>/dev/null | grep -a -E "$CRASH" | head -n 1)
        fi
        # systemd's automatic restarts raise NRestarts (a start by hand resets it to 0); an unreadable one is no hit
        if [ -n "$hit" ] || { [ "$base" -ge 0 ] && [ "$nr" -gt "$base" ]; }; then
            echo "$(date -u '+%FT%TZ') xochitl $last -> $pid, NRestarts $base -> $nr${hit:+, $hit}" > "$S/crashed"
            sync
            log "monitor: xochitl crashed with ggsense (pid $last -> $pid, NRestarts $base -> $nr${hit:+, $hit}): one strike at the next boot; the runtime QML is not sent again in this boot; monitor stops, XOVI goes off at the next idle moment"
            # the crash guard (15 min after HEALTHY, a poll every 3 s) restarts xochitl without ggsense as soon as it
            # sees the crash; a drop-in renamed before its poll would stop it without that restart: its poll first
            if [ -n "$GUARD" ] && [ -f "$DROP/30-ggsense.conf" ] && systemctl is-active --quiet "$GUARD" 2>/dev/null; then
                log "monitor: the crash guard still runs: its rollback first (up to 6 s)"
                w=0
                while [ -f "$DROP/30-ggsense.conf" ] && [ "$w" -lt 6 ] && systemctl is-active --quiet "$GUARD" 2>/dev/null; do
                    sleep 1
                    w=$((w + 1))
                done
                [ -f "$DROP/30-ggsense.conf" ] ||
                    log "monitor: the crash guard took 30-ggsense.conf out (its restart comes without ggsense)"
            fi
            # (after a rollback by the crash guard there is no drop-in left to rename: only XOVI off when idle)
            stock_when_idle crashed
            return 0
        fi
        [ "$nr" -ge 0 ] && base=$nr
        if [ "$i" -ge "$GOOD_ROUNDS" ] && [ -e "$S/probation" ]; then
            date -u '+%FT%TZ' > "$S/good"
            date -u '+%FT%TZ' > "$S/last-good"
            rm -f "$S/probation"
            sync
            log "probation passed: $((i * MON_TICK / 60)) min of awake time without a crash; this arm is good"
        fi
        [ "$pid" -gt 0 ] && [ "$pid" != "$last" ] || continue
        last=$pid
        since=$(date '+%Y-%m-%d %H:%M:%S')
        # changed by hand into a process without the boot's library: this arm can no longer be judged, and the
        # crashes of a deploy by hand (or of stock) are not the boot's. A process seen right after its fork has
        # not mapped its libraries yet: up to 10 s for them
        w=0
        while ! has_gg "$pid" && [ -d "$PROC/$pid" ] && [ "$w" -lt 10 ]; do
            sleep 1
            w=$((w + 1))
        done
        if ! has_gg "$pid"; then
            rm -f "$S/probation"
            sync
            log "monitor: xochitl $pid runs without the boot's ggsense (changed by hand): no verdict on this arm; monitor stops"
            return 0
        fi
        log "monitor: xochitl restarted by hand (pid $pid, no crash)"
        apply_qml "$pid"
    done
}

# ---- main
if [ "$MODE" = gates ]; then
    if why=$(gates); then echo "gates pass: the next boot arms XOVI + $VER (when not disabled)"; exit 0; fi
    echo "gates fail: $why"
    exit 1
fi
# stopped (gg.sh stop/stock, install.sh, shutdown): 30-ggsense.conf never stays in XOVI's persistent template.
# "armed" stays: a reboot caused by our own failure also arrives as SIGTERM (gg.sh stop/stock remove it). The
# probation mark goes, unless rm-emergency is rebooting (xochitl failed): a clean shutdown within the 15 min is no
# strike (and no good arm either). A crash the monitor saw (then waiting to turn XOVI off) stays a strike.
on_signal() {
    unstage
    if [ -e "$S/crashed" ]; then
        log "stopped by signal after xochitl crashed with ggsense in this boot (a strike at the next boot)"
    elif [ -e "$S/probation" ] && [ ! -e "$RM_EMERG" ]; then
        rm -f "$S/probation"
        sync
        log "stopped by signal during the probation (shutdown, gg.sh stop): no strike"
    elif [ -e "$S/probation" ]; then
        log "stopped by signal during the probation while rm-emergency reboots: probation kept (a strike at the next boot)"
    else
        log "stopped by signal"
    fi
    exit 0
}
if [ "$DRY" = 0 ]; then
    trap on_signal TERM INT HUP
    exec 9>"$S/.lock"
    flock -n 9
    rc=$?
    if [ "$rc" = 1 ]; then log "another ghostguard-autostart holds the lock; exit"; exit 0; fi
    if [ "$rc" != 0 ]; then log "cannot take the lock (flock exit $rc); not arming"; exit 0; fi
fi
unstage
account
if [ -e "$S/disabled" ]; then
    log "not arming: disabled ($(head -n 2 "$S/disabled" | tr '\n' ' '))"
    exit 0
fi
if xovi_on; then
    pid=$(num "$(unit_show MainPID)")
    if [ -f "$DROP/30-ggsense.conf" ] && [ "$pid" -gt 0 ] && has_gg "$pid"; then
        log "XOVI with the boot's ggsense already runs (xochitl $pid, started by hand): monitor only"
        [ "$DRY" = 1 ] && exit 0
        monitor "$pid"
        exit 0
    fi
    log "XOVI already runs without the boot's ggsense (started by hand); not touching it"
    exit 0
fi
if [ "$DRY" = 0 ]; then
    w=0
    while ! counters > /dev/null && [ "$w" -lt "$SANITY_WAIT" ]; do
        sleep 5
        w=$((w + 5))
    done
fi
if ! why=$(gates); then
    log "not arming: $why"
    exit 0
fi
if [ "$DRY" = 1 ]; then
    log "dry run: all gates pass (strikes $strikes); would wait for $IDLE_S s without input and arm"
    exit 0
fi
while :; do
    if ! idle_wait; then log "not arming in this boot: the pen and touch input cannot be watched"; exit 0; fi
    if [ -e "$S/disabled" ]; then log "disabled while waiting; not arming"; exit 0; fi
    if xovi_on; then log "XOVI was started by hand while waiting; not arming"; exit 0; fi
    # again, right before the restart (a firmware update may have been installed meanwhile), while ggtool watches
    # the input for 3 s (the gates take a few), then 3 s more after them, however long they took: one touch or pen
    # event in either sends it back to waiting. The job is waited for here, never inside $(...) (not that
    # subshell's child: wait would return 127)
    "$B/ggtool" quiet 3 3 $DEVS 9>&- &
    qp=$!
    if ! why=$(gates); then wait "$qp"; log "not arming: $why"; exit 0; fi
    wait "$qp"
    rc=$?
    if [ "$rc" = 0 ]; then
        "$B/ggtool" quiet 3 3 $DEVS 9>&-
        rc=$?
    fi
    if [ "$rc" != 0 ]; then log "input right before the restart (ggtool exit $rc); waiting again"; continue; fi
    if why=$(pre_restart); then break; fi
    log "$why; waiting again"
done
if arm; then
    monitor "$APPLIED_PID"
fi
exit 0
