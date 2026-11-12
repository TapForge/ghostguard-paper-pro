#!/bin/bash
# rescue.sh: run by ghostguard-rescue.service through OnFailure= on xochitl.service. That OnFailure= comes from
# zz-ghostguard.conf in XOVI's template, so it exists only while XOVI runs (boot arm or started by hand).
# xochitl's own OnFailure=emergency.target (rm-emergency.sh: reboot) starts at the same time, so the FIRST thing
# done is the disable sentinel, on disk: no later boot arms again until "gg.sh enable". Then XOVI off and a stock
# xochitl back for this boot (StartLimitBurst is likely hit: reset-failed before any start). Always exits 0.
set -u
B=${GG_BOOT:-/home/root/ghostguard/boot}
S=$B/state
X=${GG_XOVI:-/home/root/xovi}
DROP=${GG_DROP:-/etc/systemd/system/xochitl.service.d}
PROC=${GG_PROC:-/proc}

mkdir -p "$S" 2>/dev/null
echo "$(date -u '+%FT%TZ') rescued: xochitl.service failed while XOVI ran" > "$S/disabled"
rm -f "$S/armed"
sync

msg="RESCUE: xochitl failed with XOVI on; boot arming disabled, back to stock xochitl"
echo "$msg"
logger -t ghostguard "$msg" 2>/dev/null
echo "$(date -u '+%F %T') [$$] $msg" >> "$S/boot.log"

num() { case "$1" in ''|*[!0-9]*) echo -1 ;; *) echo "$1" ;; esac; }
systemctl reset-failed xochitl.service 2>/dev/null
"$X/stock" >> "$S/xovi-stock.log" 2>&1
if grep -q " $DROP tmpfs " "$PROC/mounts" 2>/dev/null; then
    echo "XOVI drop-in tmpfs still mounted after xovi/stock; unmounting it" | tee -a "$S/boot.log"
    umount "$DROP" 2>/dev/null
    systemctl daemon-reload 2>/dev/null
fi
systemctl reset-failed xochitl.service 2>/dev/null
sleep 2
p=$(num "$(systemctl show xochitl.service -p MainPID --value 2>/dev/null)")
if [ "$p" -gt 0 ] && grep -q "/xovi.so" "$PROC/$p/maps" 2>/dev/null; then
    echo "xochitl $p still runs with XOVI; restarting it" | tee -a "$S/boot.log"
    systemctl restart xochitl.service 2>/dev/null
elif ! systemctl is-active --quiet xochitl.service 2>/dev/null; then
    systemctl start xochitl.service 2>/dev/null
fi
exit 0
