#!/bin/bash
# ggapply.sh: give the running xochitl (XOVI + ggsense, GhostGuard QML loaded) its runtime QML. Called by
# autostart.sh after a healthy arm and after every xochitl restart it sees; by hand: gg.sh apply.
# The QML (ghostguard.qmd) polls control.json every 3 s and runs its "cmd" when the id changes; the command found
# there at its first read is history and never runs, so nothing is sent before the QML has read the file once.
#   1. kbd     the on-screen keyboard watcher (Overlay mode)          payload/kbd.cmd     (tools/kbd_overlay.js)
#   2. clock   the QML clock: defer and the ggsense triggers on       payload/clock.cmd   (tools/clock12.js)
#   3. status  a fresh /run/ghostguard/status.json, checked: defer 1, the four triggers on, the expected ggsense,
#              the keyboard-open clean off in 1.8.3+ (retired there; its status has "sw"), on in 1.8.1 and 1.8.2
#   4. splash  one in-place clean of the whole screen (the restart's splash), only when the pen and touch input
#              could be watched and nobody touched the screen for 3 s; ggsense's own answer is read from status.json
#              ("q":"95", "rc"): busy / notready / pending (nothing was cleaned) get one more try 2 s later
# Each command is confirmed by its result in xochitl's journal ("XOVI-GG71 EVAL [<js>] => <result>"). kbd, clock
# and status are sent once more (new id) when no result comes (they are idempotent); the splash never twice.
# Only the "cmd" line of control.json is rewritten (new file + rename, like tools/ggctl.py), the settings stay; it
# must hold the whole cmd object (json.dumps(indent=1), e.g. tools/ggcmd.py, spreads it over several lines: refused).
# Writes one line to $RUN/apply.result. Exit 0 active, 1 failed, 3 nothing to do (no ggsense in that xochitl).
#   ggapply.sh [PID]        (default: xochitl's MainPID)
set -u
umask 022

B=${GG_BOOT:-/home/root/ghostguard/boot}
CTL=${GG_CTL:-/home/root/ghostguard/control.json}
RUN=${GG_RUN:-/run/ghostguard}
PROC=${GG_PROC:-/proc}
SO_MAPS=${GG_SO_MAPS:-/home/root/ghostguard/boot/ggsense.so}  # the boot's pinned library as xochitl maps it
MAP_WAIT=${GG_MAP_WAIT:-10}          # seconds for a just started xochitl to map its libraries
VER=${GG_VER:-__GG_VER__}            # filled in by persist_install.sh from the library it installs
BANNER_WAIT=${GG_BANNER_WAIT:-90}    # seconds for the GhostGuard QML to load in a new xochitl
PRIME_S=${GG_PRIME_S:-5}             # after the QML banner: more than one 3 s poll of control.json
RESULT_WAIT=${GG_RESULT_WAIT:-15}    # seconds for one command's result
SPLASH=${GG_SPLASH:-1}               # 0: no splash clean
BANNER="XOVI-GG71 v7.1 loaded"
CMDLINE='^ "cmd": [{]"id": [0-9]+, .*[}][[:space:]]*,?[[:space:]]*$'   # the whole cmd object on one line
PL=$B/payload
RES=$RUN/apply.result

mkdir -p "$RUN" 2>/dev/null
say() { echo "$(date -u '+%F %T') ggapply: $*"; }
finish() { # $1 exit code, $2 result line
    say "$2"
    echo "$2" > "$RES.tmp" && mv -f "$RES.tmp" "$RES"
    exit "$1"
}
num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }
cur_pid() { num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)"; }
evdevs() {
    local n
    for n in "$@"; do
        awk -v want="N: Name=\"$n\"" '
            $0 == want { m = 1; next }
            m && /^H: Handlers=/ { sub(/^H: Handlers=/, ""); for (i = 1; i <= NF; i++) if ($i ~ /^event[0-9]+$/) print "/dev/input/" $i; m = 0 }
            /^$/ { m = 0 }' "$PROC/bus/input/devices" 2>/dev/null
    done
}

pid=$(num "${1:-$(cur_pid)}")
[ "$pid" -gt 0 ] || finish 1 "FAILED: xochitl is not running"
# the boot's library only (a deploy by hand has its own library and sends its own QML); a process seen right after
# its fork has not mapped its libraries yet: wait for them
t=0
until grep -q -F "$SO_MAPS" "$PROC/$pid/maps" 2>/dev/null; do
    if [ ! -d "$PROC/$pid" ] || [ "$t" -ge "$MAP_WAIT" ]; then
        finish 3 "no boot ggsense ($SO_MAPS) in xochitl $pid; nothing sent"
    fi
    sleep 1
    t=$((t + 1))
done

exec 8>"$RUN/apply.lock"
flock -n 8
rc=$?
if [ "$rc" = 1 ]; then say "another ggapply.sh runs; this one exits"; exit 1; fi
[ "$rc" = 0 ] || finish 1 "FAILED: cannot take $RUN/apply.lock (flock exit $rc); nothing sent"

for n in kbd clock status splash; do
    [ -s "$PL/$n.cmd" ] && [ -s "$PL/$n.ok" ] || finish 1 "FAILED: missing payload $PL/$n.cmd or $PL/$n.ok"
done
[ "$(grep -c '^ "cmd": ' "$CTL" 2>/dev/null)" = 1 ] && grep -q -E "$CMDLINE" "$CTL" 2>/dev/null ||
    finish 1 "FAILED: no one-line cmd object in $CTL; nothing sent"

# the library in this process must be the build the payloads were made for
if ! grep -q -F "{\"loaded\":\"$VER\",\"pid\":$pid}" "$RUN/loaded.json" 2>/dev/null; then
    finish 1 "FAILED: $RUN/loaded.json is not $VER in xochitl $pid ($(cut -c1-80 "$RUN/loaded.json" 2>/dev/null | head -n 1)); nothing sent"
fi

# the GhostGuard QML in this process, and one poll of control.json after it
t=0
until journalctl -b _PID="$pid" -o cat --no-pager -q 2>/dev/null | grep -a -q "$BANNER"; do
    [ "$(cur_pid)" = "$pid" ] || finish 1 "FAILED: xochitl $pid ended (now $(cur_pid)) before its QML loaded; nothing sent"
    [ "$t" -ge "$BANNER_WAIT" ] && finish 1 "FAILED: the GhostGuard QML did not load in xochitl $pid within $BANNER_WAIT s; nothing sent"
    sleep 2
    t=$((t + 2))
done
sleep "$PRIME_S"

# rewrite only the cmd line: ' "cmd": {"id": N, ' + the payload line ('"name": "eval", "arg": "..."}')
SENT_ID=""
write_cmd() { # $1 = payload file
    local old nid tmp=$CTL.gg.new
    # the id is a number: the cmd line passed CMDLINE ('"id": [0-9]+,') before anything was sent
    old=$(sed -n 's/^ "cmd": {"id": *\([0-9][0-9]*\)[,}].*/\1/p' "$CTL" 2>/dev/null | head -n 1)
    nid=$((old + 1))
    if ! awk -v pf="$1" -v nid="$nid" -v re="$CMDLINE" '
            BEGIN { ok = ((getline p < pf) > 0) }
            ok && !hit && $0 ~ re {
                suf = ""
                if ($0 ~ /[}],[ \t\r]*$/) suf = ","
                print " \"cmd\": {\"id\": " nid ", " p suf
                hit = 1
                next
            }
            { print }
            END { exit ((ok && hit) ? 0 : 1) }' "$CTL" > "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    if [ "$(grep -c '^ "cmd": [{]' "$tmp")" != 1 ] || [ "$(grep -c '' "$tmp")" != "$(grep -c '' "$CTL")" ]; then
        rm -f "$tmp"
        return 1
    fi
    mv -f "$tmp" "$CTL" || { rm -f "$tmp"; return 1; }
    SENT_ID=$nid
}

LAST=""
jsince() { journalctl -b _PID="$pid" --since "@$1" -o cat --no-pager -q 2>/dev/null; }
send() { # $1 payload name, $2 attempts; 0 confirmed, 1 no result, 2 error, 3 xochitl ended
    local name=$1 tries=$2 a=0 ok since t out e0 e1
    ok=$(head -n 1 "$PL/$name.ok")
    while [ "$a" -lt "$tries" ]; do
        a=$((a + 1))
        since=$(( $(date +%s) - 1 ))
        # errors already in the window (the journal has 1 s steps here) belong to an earlier command; counted
        # before the write, so this command's own error can never be among them
        e0=$(jsince "$since" | grep -a -c "XOVI-GG71 command error")
        write_cmd "$PL/$name.cmd" || { say "$name: cannot rewrite $CTL"; return 2; }
        say "$name: sent as command $SENT_ID (attempt $a)"
        t=0
        while [ "$t" -lt "$RESULT_WAIT" ]; do
            sleep 1
            t=$((t + 1))
            [ "$(cur_pid)" = "$pid" ] || return 3
            out=$(jsince "$since")
            if printf '%s\n' "$out" | grep -a -q -F "] => $ok"; then
                LAST=$(printf '%s\n' "$out" | grep -a -F "] => $ok" | tail -n 1 | sed 's/.*\] => //; s/ (log qrc:.*//')
                say "$name: $LAST"
                return 0
            fi
            e1=$(printf '%s\n' "$out" | grep -a -c "XOVI-GG71 command error")
            if [ "$e1" -gt "$e0" ]; then
                say "$name: $(printf '%s\n' "$out" | grep -a "XOVI-GG71 command error" | tail -n 1 | sed 's/.*XOVI-GG71 //')"
                return 2
            fi
        done
        say "$name: no result in $RESULT_WAIT s"
    done
    return 1
}
status_ok() {
    local s
    s=$(cat "$RUN/status.json" 2>/dev/null)
    case "$s" in *'"q":"97"'*) ;; *) return 1 ;; esac
    case "$s" in *'"defer":1,'*) ;; *) return 1 ;; esac
    case "$s" in *'"on":[1,1,1,1]'*) ;; *) return 1 ;; esac
    case "$s" in *"\"v\":\"$VER\""*) ;; *) return 1 ;; esac
    # the keyboard-open clean (kbOpen[0]): 1.8.3 and later (their status has the SWTCON state "sw") retired it and
    # the clock sends it off: it must be off. 1.8.1 and 1.8.2: on (their clock turned it on). 1.8: no such field
    case "$s" in
        *'"sw":{'*) case "$s" in *'"kbOpen":[0,'*) ;; *) return 1 ;; esac ;;
        *'"kbOpen":['*) case "$s" in *'"kbOpen":[1,'*) ;; *) return 1 ;; esac ;;
    esac
    return 0
}

kb="not confirmed (keyboard stays stock)"
send kbd 2
rc=$?
[ "$rc" = 3 ] && finish 1 "FAILED: xochitl $pid ended while the keyboard watcher was sent"
[ "$rc" = 0 ] && kb="on"

active=0
for round in 1 2; do
    send clock 2
    rc=$?
    [ "$rc" = 3 ] && finish 1 "FAILED: xochitl $pid ended while the clock was sent"
    rm -f "$RUN/status.json"    # only our status request can write the next one
    send status 2
    rc=$?
    [ "$rc" = 3 ] && finish 1 "FAILED: xochitl $pid ended while the status was asked"
    if status_ok; then active=1; break; fi
    say "status after clock round $round: $(cut -c1-60 "$RUN/status.json" 2>/dev/null | head -n 1)... not defer 1 / triggers on / $VER"
done
[ "$active" = 1 ] || finish 1 "FAILED: clock not confirmed in status.json; ggsense stays passive (stock flashes), keyboard watcher $kb"

# ggsense's answer to the splash clean ("rc" of a status.json with "q":"95"), "" when none came within 5 s
splash_rc() {
    local i=0 s
    while [ "$i" -lt 5 ]; do
        s=$(cat "$RUN/status.json" 2>/dev/null)
        case "$s" in *'"q":"95"'*) echo "$s" | grep -o '"rc":"[a-z]*"' | head -n 1 | cut -d'"' -f4; return ;; esac
        sleep 1
        i=$((i + 1))
    done
}
sp="off"
if [ "$SPLASH" = 1 ]; then
    q=2
    devs=$(evdevs "Elan marker input" "Elan touch input")
    n=0
    for d in $devs; do n=$((n + 1)); done
    if [ "$n" = 2 ] && [ -x "$B/ggtool" ]; then
        "$B/ggtool" quiet 3 20 $devs
        q=$?
    fi
    case "$q" in
        0)
            a=1
            while :; do
                rm -f "$RUN/status.json"
                send splash 1
                rc=$?
                [ "$rc" = 3 ] && finish 1 "FAILED: xochitl $pid ended while the splash clean was asked"
                if [ "$rc" != 0 ]; then sp="not confirmed"; break; fi
                r=$(splash_rc)
                case "$r" in
                    ok) sp="cleaned"; [ "$a" = 2 ] && sp="cleaned (2nd try)"; break ;;
                    busy|notready|pending)
                        if [ "$a" = 1 ]; then say "splash: $r (nothing cleaned); one more try in 2 s"; sleep 2; a=2; continue; fi
                        sp="not cleaned ($r twice)"; break ;;
                    '') sp="asked, no answer from ggsense"; break ;;
                    *) sp="not cleaned ($r)"; break ;;
                esac
            done
            ;;
        1) sp="skipped (screen in use)" ;;
        *) sp="skipped (the pen and touch input cannot be watched)" ;;
    esac
fi
finish 0 "ACTIVE: $VER in xochitl $pid, defer 1, triggers on; keyboard watcher $kb; splash $sp"
