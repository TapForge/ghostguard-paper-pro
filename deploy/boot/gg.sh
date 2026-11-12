#!/bin/bash
# gg.sh: GhostGuard boot persistence on the tablet (installed by deploy/persist_install.sh).
#   gg.sh status          installed?, what the last boots did, what runs now
#   gg.sh disable [why]   no arming from the next boot on (nothing changes now; "gg.sh stock" = stock now)
#   gg.sh enable          arm again from the next boot (clears the disable sentinel, the strikes and their marks)
#   gg.sh stop            stop this boot's ghostguard-autostart (its monitor); XOVI and ggsense keep running
#   gg.sh stock           stop it and turn XOVI off now (one xochitl restart); later boots still arm
#   gg.sh apply           send the runtime QML to the running xochitl now (ggapply.sh)
#   gg.sh gates           would the next boot arm? (the checks of autostart.sh)
#   gg.sh log [N]         the last N lines of the boot log (default 40)
set -u
B=${GG_BOOT:-/home/root/ghostguard/boot}
S=$B/state
X=${GG_XOVI:-/home/root/xovi}
DROP=${GG_DROP:-/etc/systemd/system/xochitl.service.d}
RUN=${GG_RUN:-/run/ghostguard}
PROC=${GG_PROC:-/proc}
BK=${GG_BK:-/home/root/gg-backup}                             # the crash guard's rollback mark is kept here
SO=${GG_SO:-$B/ggsense.so}                                    # the boot's pinned copy of the library
SO_MAPS=${GG_SO_MAPS:-/home/root/ghostguard/boot/ggsense.so}  # its path as xochitl maps it (dropins/30-ggsense.conf)

num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }
field() { grep -o "\"$1\":[^,}]*" "$RUN/status.json" 2>/dev/null | head -n 1; }
afield() { grep -o "\"$1\":\[[^]]*\]" "$RUN/status.json" 2>/dev/null | head -n 1; }

case "${1:-status}" in
status)
    echo "installed:  $(cat "$S/installed" 2>/dev/null || echo 'no record')"
    echo "files:      $(cd "$B" 2>/dev/null && md5sum -c MANIFEST.md5 2>/dev/null | grep -c ': OK$') OK of $(grep -c '' "$B/MANIFEST.md5" 2>/dev/null) in MANIFEST.md5"
    echo "timer:      $(systemctl is-enabled ghostguard-autostart.timer 2>/dev/null || echo '?') / service $(systemctl is-active ghostguard-autostart.service 2>/dev/null)"
    echo "disabled:   $(cat "$S/disabled" 2>/dev/null || echo no)"
    echo "strikes:    $(cat "$S/strikes" 2>/dev/null || echo 0)/2   last good arm: $(cat "$S/last-good" 2>/dev/null || echo never)"
    [ -e "$S/probation" ] && echo "probation:  healthy since $(cat "$S/probation") (good after 15 min of awake time without a crash)"
    [ -e "$S/crashed" ] && echo "crashed:    $(cat "$S/crashed") (a strike at the next boot)"
    pid=$(num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)")
    xo=off; grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null && xo=on
    gg=no
    if grep -q -F "$SO_MAPS" "$PROC/$pid/maps" 2>/dev/null; then gg="yes (the boot's copy)"
    elif grep -q "/ggsense.so" "$PROC/$pid/maps" 2>/dev/null; then gg="yes (deployed by hand, not the boot's copy)"; fi
    echo "xochitl:    pid $pid, XOVI $xo, ggsense $gg, $(cat "$RUN/loaded.json" 2>/dev/null)"
    echo "library:    $SO md5 $(md5sum "$SO" 2>/dev/null | cut -d' ' -f1)"
    echo "last apply: $(cat "$RUN/apply.result" 2>/dev/null || echo none in this boot)"
    echo "status:     $(field v) $(field defer) $(afield on) $(afield kbOpen)"
    echo "--- boot log"
    tail -n 8 "$S/boot.log" 2>/dev/null
    ;;
disable)
    shift
    mkdir -p "$S"
    echo "$(date -u '+%FT%TZ') by hand${*:+: $*}" > "$S/disabled"
    sync
    echo "disabled: no arming from the next boot on (this boot is unchanged; stock now: $0 stock)"
    ;;
enable)
    rm -f "$S/disabled" "$S/armed" "$S/probation" "$S/crashed"
    # the crash guard's rollback mark would be a strike at the next boot: set aside (kept, like install.sh does)
    rb=$BK/30-ggsense.conf.autorollback
    [ -e "$rb" ] && mv -f "$rb" "$rb.$(date +%s).enable"
    echo 0 > "$S/strikes"
    sync
    echo "enabled: the next boot arms XOVI + ggsense (strikes 0); checks now:"
    bash "$B/autostart.sh" --gates
    ;;
stop)
    systemctl stop ghostguard-autostart.service 2>/dev/null
    rm -f "$S/armed" "$S/probation"    # a deliberate stop during an arm or the probation is no strike
    echo "ghostguard-autostart stopped for this boot (state $(systemctl is-active ghostguard-autostart.service 2>/dev/null))"
    ;;
stock)
    systemctl stop ghostguard-autostart.service 2>/dev/null
    rm -f "$S/armed" "$S/probation"
    # the way autostart.sh's stock_now() and rescue.sh do it: if xochitl still runs with XOVI after xovi/stock,
    # unmount and restart here; "XOVI off" only once checked
    systemctl reset-failed xochitl.service 2>/dev/null
    "$X/stock"
    if grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null; then
        echo "XOVI drop-in tmpfs still mounted after xovi/stock; unmounting it"
        umount "$DROP" 2>/dev/null
        systemctl daemon-reload 2>/dev/null
    fi
    systemctl reset-failed xochitl.service 2>/dev/null
    sleep 2
    pid=$(num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)")
    if [ "$pid" -gt 0 ] && grep -q "/xovi.so" "$PROC/$pid/maps" 2>/dev/null; then
        echo "xochitl $pid still runs with XOVI after xovi/stock; restarting it"
        systemctl restart xochitl.service 2>/dev/null
    elif ! systemctl is-active --quiet xochitl.service 2>/dev/null; then
        systemctl start xochitl.service 2>/dev/null
    fi
    pid=$(num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)")
    xo=gone; grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null && xo=mounted
    if [ "$xo" = mounted ] || grep -q "/xovi.so" "$PROC/$pid/maps" 2>/dev/null; then
        echo "WARNING: XOVI still on (drop-in tmpfs $xo, xochitl pid $pid); reboot for stock xochitl"
        exit 1
    fi
    echo "XOVI off (stock xochitl); later boots still arm unless: $0 disable"
    ;;
apply)
    bash "$B/ggapply.sh"
    echo "rc $?: $(cat "$RUN/apply.result" 2>/dev/null)"
    ;;
gates)
    bash "$B/autostart.sh" --gates
    ;;
log)
    tail -n "${2:-40}" "$S/boot.log" 2>/dev/null
    ;;
*)
    sed -n '2,10p' "$0"
    exit 2
    ;;
esac
