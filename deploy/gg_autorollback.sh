#!/bin/sh
# gg_autorollback.sh [SINCE]  -  one-shot crash guard for the ggsense 1.5 activation (BusyBox ash / dash).
#
# deploy_v15.sh starts it right BEFORE "systemctl restart xochitl", in its own cgroup (systemd-run or a
# runtime unit), so it does not die with the SSH session. SINCE = "YYYY-MM-DD HH:MM:SS" (local time)
# taken just before that restart. For ROUNDS polls of AWAKE time (sleep does not advance while the tablet
# is suspended; capped at WALLMAX s of wall time) it watches for the first sign that xochitl failed:
#   - systemd's own lines about xochitl.service (journal, priority notice or higher): core dump,
#     SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE, watchdog timeout, "Failed with result", start limit hit
#   - the unit's automatic-restart counter (NRestarts) going up
#   - the unit being in the "failed" state (e.g. start-limit-hit after a fast crash loop)
# On the first hit it renames 30-ggsense.conf away (one atomic rename: if another watcher or
# disable_ggsense.sh got there first, this one does not restart), runs daemon-reload, clears the failed /
# start-limit state and restarts xochitl ONCE without ggsense, then exits. No loop, no second restart.
# If the ggsense drop-in disappears (disable_ggsense.sh, xovi/start, reboot) it just exits.

DROPDIR=${GG_DROPDIR:-/etc/systemd/system/xochitl.service.d}
DROP=$DROPDIR/30-ggsense.conf
GONE=$DROP.autorollback          # not *.conf, so systemd ignores it
SO=${GG_SO:-/home/root/ghostguard/ggsense.so}
BK=${GG_BK:-/home/root/gg-backup}
LOG=$BK/autorollback.log
LOCK=${GG_LOCK:-/run/gg-autorollback.lock}
PROC=${GG_PROC:-/proc}
POLL=${GG_POLL:-3}           # seconds between checks
ROUNDS=${GG_ROUNDS:-300}     # 300 x 3 s = 15 min of awake time
WALLMAX=${GG_WALLMAX:-21600} # never watch longer than 6 h of wall time
PAT='code=dumped|status=(4/ILL|6/ABRT|7/BUS|8/FPE|11/SEGV)|Failed with result|Start request repeated too quickly|Watchdog timeout'
ONSIG='log "stopped by signal"; exit 0'

mkdir -p "$BK" 2>/dev/null
log() { echo "$(date '+%F %T') [$$] $*" >> "$LOG"; }
num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }

since=$1
[ -n "$since" ] || since=$(date '+%Y-%m-%d %H:%M:%S')

# ---- single instance (a stale lock whose pid is dead or now belongs to another program is taken over)
if ! mkdir "$LOCK" 2>/dev/null; then
    other=$(cat "$LOCK/pid" 2>/dev/null)
    if [ -n "$other" ] && kill -0 "$other" 2>/dev/null; then
        case "$(tr '\0' ' ' 2>/dev/null < "$PROC/$other/cmdline")" in
            *gg_autorollback*) log "watcher $other already running; this one exits"; exit 0 ;;
        esac
    fi
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || { log "cannot take $LOCK; exit"; exit 0; }
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT
trap "$ONSIG" HUP INT TERM

if [ ! -f "$DROP" ] || ! grep -q ggsense "$DROP" 2>/dev/null; then
    log "no ggsense drop-in at $DROP; nothing to guard"
    exit 0
fi

last_nr=$(num "$(systemctl show xochitl.service -p NRestarts --value 2>/dev/null)")
cg=$(grep -o 'dropbear[^/]*' "$PROC/self/cgroup" 2>/dev/null | head -n 1)
log "watch start: since '$since', $ROUNDS x ${POLL}s awake, NRestarts=$last_nr${cg:+, WARNING: inside $cg (dies with the SSH session if it kills its cgroup)}"

rollback() {
    # called at most once; always ends the script. Signals are ignored until the restart is issued.
    trap '' HUP INT TERM
    log "FAILURE: $1"
    if ! mv "$DROP" "$GONE" 2>/dev/null; then
        if [ ! -f "$DROP" ]; then
            log "drop-in already gone (someone rolled back meanwhile); no restart from here"
            exit 0
        fi
        log "cannot rename $DROP; the library will be moved aside instead"
    fi
    cp -f "$GONE" "$BK/30-ggsense.conf.autorollback" 2>/dev/null || cp -f "$DROP" "$BK/30-ggsense.conf.autorollback" 2>/dev/null
    systemctl daemon-reload || log "daemon-reload failed"
    # if the unit still preloads ggsense (another drop-in, failed reload), move the library away;
    # ld.so ignores a missing LD_PRELOAD object ("cannot be preloaded ... ignored")
    if systemctl show xochitl.service -p Environment 2>/dev/null | grep -q ggsense; then
        mv -f "$SO" "$SO.autorollback" 2>/dev/null && log "unit still preloads ggsense: moved $SO aside"
    fi
    systemctl reset-failed xochitl.service 2>/dev/null
    if systemctl restart xochitl.service; then
        log "ROLLED BACK: xochitl restarted once without ggsense"
    else
        log "ROLLED BACK, but 'systemctl restart xochitl' returned an error; no further attempt (reboot = stock)"
    fi
    trap "$ONSIG" HUP INT TERM
    sleep "${GG_POST:-20}"
    p=$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)
    log "after rollback: pid=$p state=$(systemctl is-active xochitl.service 2>/dev/null) ggsense mapped=$(grep -c ggsense "$PROC/$p/maps" 2>/dev/null)"
    exit 0
}

t_start=$(date +%s)
i=0
while [ "$i" -lt "$ROUNDS" ]; do
    i=$((i + 1))
    sleep "$POLL"   # first check one poll after start, when the deploy's own restart is under way
    if [ ! -f "$DROP" ]; then
        log "ggsense drop-in removed by someone else; watcher stops (no restart)"
        exit 0
    fi
    # 1. systemd's own messages about the unit (xochitl's own output is info level and filtered out)
    hit=$(journalctl -b -u xochitl.service -p notice --since "$since" --no-pager -q -o cat 2>/dev/null \
          | grep -E "$PAT" | head -n 3 | tr '\n' ' ')
    [ -n "$hit" ] && rollback "journal: $hit"
    # 2. automatic restarts (a manual restart flushes the counter to 0, so compare with the last poll)
    nr=$(num "$(systemctl show xochitl.service -p NRestarts --value 2>/dev/null)")
    if [ "$nr" -ge 0 ] && [ "$last_nr" -ge 0 ] && [ "$nr" -gt "$last_nr" ]; then
        rollback "NRestarts went from $last_nr to $nr"
    fi
    [ "$nr" -ge 0 ] && last_nr=$nr
    # 3. unit failed (start-limit-hit after a fast crash loop, or any other failure)
    if systemctl is-failed --quiet xochitl.service 2>/dev/null; then
        rollback "unit state failed ($(systemctl show xochitl.service -p Result --value 2>/dev/null))"
    fi
    if [ $(( $(date +%s) - t_start )) -ge "$WALLMAX" ]; then
        log "wall-time cap reached after $i checks; watcher done"
        exit 0
    fi
done
log "no failure in $ROUNDS checks (~15 min awake); watcher done, ggsense stays"
exit 0
