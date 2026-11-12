#!/bin/bash
# Mac side: install GhostGuard boot persistence on the tablet. From the next boot on, 70 s after boot and after
# 2 min without touch or pen input, XOVI starts with ggsense (one xochitl restart), a health watch and a crash guard
# check it, and the runtime QML (keyboard watcher, clock, splash clean) is sent; any failure falls back to stock
# xochitl and two bad boots disable it (see deploy/boot/autostart.sh). Nothing changes in the running boot.
# Default is a DRY RUN: builds and checks the staging folder here and prints the commands for the tablet; nothing
# is sent. --go runs them.
#   bash deploy/persist_install.sh [--go] [--so LIB] [--kbd FILE] [--clock FILE] [--ver TEXT] [--copy-so] [--stage DIR]
#     --so LIB      the ggsense build the boots arm (default ggsense/ggsense184.so). It goes with the staging folder
#                   as the boot's own pinned copy, /home/root/ghostguard/boot/ggsense.so (the boot drop-in preloads
#                   that file); its md5 and version string go into the staged scripts. The library a deploy by hand
#                   uses (/home/root/ghostguard/ggsense.so) is never touched: such a deploy cannot change what boots
#     --copy-so     no longer needed (accepted and ignored)
#     --kbd/--clock the runtime QML (default tools/kbd_overlay.js, tools/clock12.js), turned into payloads now
#     --ver TEXT    the library's version string, when it cannot be read from LIB (e.g. "ggsense 1.8.1")
#     --stage DIR   build the staging folder in DIR/boot (kept) instead of a temporary folder
# Needs `ssh remarkable` (key login) for --go. Remove: bash deploy/persist_uninstall.sh
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
GO=0
COPY=0
LIB=$P/ggsense/ggsense184.so
KBD=$P/tools/kbd_overlay.js
CLOCK=$P/tools/clock12.js
VER=""
STAGE_DIR=""
HOST=${GG_HOST:-remarkable}
REM=/home/root/gg-staging
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=10"

die() { echo "persist_install: $*" >&2; exit 1; }
need() { [ $# -ge 2 ] || die "$1 needs a value"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --go) GO=1 ;;
        --dry-run) GO=0 ;;
        --copy-so) COPY=1 ;;   # kept for old command lines; the library always goes with the staging folder
        --so) need "$@"; LIB=$2; shift ;;
        --kbd) need "$@"; KBD=$2; shift ;;
        --clock) need "$@"; CLOCK=$2; shift ;;
        --ver) need "$@"; VER=$2; shift ;;
        --stage) need "$@"; STAGE_DIR=$2; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "persist_install: unknown option $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

md5q() { if command -v md5 > /dev/null 2>&1; then md5 -q "$1"; else md5sum "$1" | cut -d' ' -f1; fi; }

# ---- local checks
echo "== local checks"
[ -f "$LIB" ] || die "no library $LIB (build it, or pass --so)"
SO_MD5=$(md5q "$LIB")
if [ -z "$VER" ]; then
    vers=$(LC_ALL=C grep -a -o -E 'ggsense [0-9]+(\.[0-9]+)+' "$LIB" | LC_ALL=C sort -u)
    [ -n "$vers" ] || die "no 'ggsense N.N' version string in $LIB (pass --ver)"
    [ "$(printf '%s\n' "$vers" | grep -c '')" = 1 ] || die "several version strings in $LIB: $(echo $vers) (pass --ver)"
    VER=$vers
fi
case "$VER" in *[!A-Za-z0-9.\ _-]*|'') die "odd version string '$VER'" ;; esac
for f in "$KBD" "$CLOCK" "$P/tools/ggtool" "$P/deploy/gg_autorollback.sh"; do [ -f "$f" ] || die "missing $f"; done
file -b "$P/tools/ggtool" | grep -q 'ELF 64-bit LSB.*aarch64' || die "tools/ggtool is not an aarch64 Linux binary"
LC_ALL=C grep -a -q 'bad quiet/max' "$P/tools/ggtool" || die "tools/ggtool has no 'quiet' command (rebuild it)"
command -v python3 > /dev/null || die "python3 not found"
for f in "$P"/deploy/boot/*.sh; do bash -n "$f" || die "syntax error in $f"; done
sh -n "$P/deploy/gg_autorollback.sh" || die "syntax error in deploy/gg_autorollback.sh"
OS_TESTED=$(sed -n 's/^OS_TESTED=\${GG_OS_TESTED:-\([0-9]*\)}.*/\1/p' "$P/deploy/boot/autostart.sh")
XO_MD5=$(sed -n 's/^XO_MD5=\${GG_XO_MD5:-\([0-9a-f]*\)}.*/\1/p' "$P/deploy/boot/autostart.sh")
[ -n "$OS_TESTED" ] && [ -n "$XO_MD5" ] || die "cannot read OS_TESTED / XO_MD5 from deploy/boot/autostart.sh"
echo "library $LIB: $VER, md5 $SO_MD5"
echo "tested firmware $OS_TESTED, xochitl md5 $XO_MD5"

# ---- staging folder
if [ -n "$STAGE_DIR" ]; then
    ST=$STAGE_DIR
    mkdir -p "$ST"
    rm -rf "$ST/boot"
elif [ "$GO" = 1 ]; then
    ST=$(mktemp -d "${TMPDIR:-/tmp}/ggpersist.XXXXXX")
    trap 'rm -rf "$ST"' EXIT
else
    ST=$(mktemp -d "${TMPDIR:-/tmp}/ggpersist.XXXXXX")
fi
SB=$ST/boot
echo "== staging in $SB"
mkdir -p "$SB/units" "$SB/dropins" "$SB/payload"
for f in autostart.sh ggapply.sh rescue.sh gg.sh install.sh uninstall.sh; do
    sed -e "s/__SO_MD5__/$SO_MD5/g" -e "s/__GG_VER__/$VER/g" "$P/deploy/boot/$f" > "$SB/$f"
done
cp "$P"/deploy/boot/units/ghostguard-autostart.service "$P"/deploy/boot/units/ghostguard-autostart.timer \
   "$P"/deploy/boot/units/ghostguard-rescue.service "$SB/units/"
cp "$P"/deploy/boot/dropins/30-ggsense.conf "$P"/deploy/boot/dropins/zz-ghostguard.conf "$SB/dropins/"
cp "$P/deploy/gg_autorollback.sh" "$SB/gg_autorollback.sh"
cp "$P/tools/ggtool" "$SB/ggtool"
cp "$LIB" "$SB/ggsense.so"
python3 -I "$P/deploy/boot/mkpayload.py" "$SB/payload" --kbd "$KBD" --clock "$CLOCK"
chmod 0755 "$SB"/*.sh "$SB/ggtool" "$SB/ggsense.so"
chmod 0644 "$SB"/units/* "$SB"/dropins/* "$SB"/payload/*
if grep -r -l -E '__SO_MD5__|__GG_VER__' "$SB" > /dev/null; then die "placeholders left in $SB"; fi
grep -q "^SO_MD5=\${GG_SO_MD5:-$SO_MD5}" "$SB/autostart.sh" || die "SO_MD5 not filled in"
grep -q "^VER=\${GG_VER:-$VER}" "$SB/ggapply.sh" || die "VER not filled in"
for f in "$SB"/*.sh; do bash -n "$f" || die "syntax error in staged $f"; done
if grep -q ggsense "$SB/dropins/zz-ghostguard.conf"; then die "zz-ghostguard.conf names the library (deploy scripts would refuse the template)"; fi
( cd "$SB" && find . -type f ! -name MANIFEST.md5 | sed 's|^\./||' | LC_ALL=C sort | while read -r f; do
    printf '%s  %s\n' "$(md5q "$f")" "$f"; done ) > "$SB/MANIFEST.md5"
echo "$(grep -c '' "$SB/MANIFEST.md5") files in MANIFEST.md5"

# ---- the tablet
q() { printf '%q ' "$@"; echo; }
run() { # run (or print) one local command
    if [ "$GO" = 1 ]; then "$@"; else echo "  $(q "$@")"; fi
}
rsh() { # run (or print) one command on the tablet
    if [ "$GO" = 1 ]; then ssh $SSH_OPTS "$HOST" "$1"; else echo "  ssh $SSH_OPTS $HOST $(q "$1")"; fi
}
[ "$GO" = 1 ] || echo "== DRY RUN: nothing is sent; with --go these run:"

[ "$COPY" = 1 ] && echo "note: --copy-so is no longer needed: the library goes with the staging folder (boot/ggsense.so)"
echo "== 1. the tablet: firmware, xochitl and XOVI"
if [ "$GO" = 1 ]; then
    info=$(ssh $SSH_OPTS "$HOST" "cat /etc/version; md5sum /usr/bin/xochitl /home/root/ghostguard/ggsense.so 2>/dev/null; [ -x /home/root/xovi/start ] && echo xovi-ok; true") ||
        die "ssh $HOST failed"
    printf '%s\n' "$info"
    dv=$(printf '%s\n' "$info" | head -n 1)
    dxo=$(printf '%s\n' "$info" | grep ' /usr/bin/xochitl$' | cut -d' ' -f1)
    [ "$dv" = "$OS_TESTED" ] || die "the tablet runs firmware $dv, not the tested $OS_TESTED; not installed"
    [ "$dxo" = "$XO_MD5" ] || die "the tablet's xochitl is not the tested build; not installed"
    printf '%s\n' "$info" | grep -q '^xovi-ok$' || die "no XOVI on the tablet; not installed"
else
    rsh "cat /etc/version; md5sum /usr/bin/xochitl /home/root/ghostguard/ggsense.so; [ -x /home/root/xovi/start ] && echo xovi-ok"
    echo "  (then: firmware must be $OS_TESTED, xochitl md5 $XO_MD5; the boot library goes with the staging folder)"
fi

echo "== 2. send the staging folder"
rsh "rm -rf $REM/boot && mkdir -p $REM" || die "cannot prepare $REM on the tablet"
run scp $SSH_OPTS -q -r "$SB" "$HOST:$REM/" || die "copying the staging folder to the tablet failed"
rsh "cd $REM/boot && md5sum -c MANIFEST.md5 | grep -v ': OK\$'; md5sum -c MANIFEST.md5 > /dev/null && echo 'staged files ok'" ||
    die "the staged files on the tablet do not match MANIFEST.md5 (transfer problem?); not installed"

echo "== 3. install (root file system units, XOVI template rescue drop-in, gates)"
rsh "bash $REM/boot/install.sh $REM/boot" ||
    die "install on the tablet stopped (exit $?: 3 = not installed, the gates say the next boot would not arm; 1 = failed, see INSTALL FAILED above; else cut off); once it got to its step 2, later boots do not arm until an install succeeds"

if [ "$GO" = 1 ]; then
    echo "== done. Check after the next reboot (about 2 min): ssh $HOST bash /home/root/ghostguard/boot/gg.sh status"
else
    echo "== dry run done; staging folder kept: $SB"
fi
