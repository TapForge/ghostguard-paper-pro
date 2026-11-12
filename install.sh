#!/bin/bash
# GhostGuard for the reMarkable Paper Pro: one-step installer. Run it on your computer (macOS, Linux, or WSL on
# Windows) with the tablet connected by USB, awake and unlocked:
#
#     bash install.sh
#
# It asks before every step that changes something. What it does, in order:
#   1. checks the tools it needs on this computer
#   2. finds the tablet (and, with your consent, sets up password-free SSH login for it)
#   3. checks that the tablet runs exactly the firmware this build was made for (it refuses otherwise)
#   4. installs XOVI (the extension loader GhostGuard needs) when it is not there yet
#   5. copies GhostGuard to the tablet and arms it for the next boot
#   6. offers to restart the tablet
# Nothing here writes to your notebooks or documents. Remove everything again with: bash uninstall.sh
#
# Options:  GG_HOST=name-or-address bash install.sh    use another SSH host than "remarkable" / root@10.11.99.1
#           bash install.sh --yes                      answer yes to every question (for people who read the script)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OS_TESTED=20261028104944
OS_NAME="3.29.1"
XO_MD5=50f86a58704d2b4c4eab20df75e093f3
XOVI_URL=https://github.com/asivery/rm-xovi-extensions/releases/download/v19-23052026/xovi-aarch64.tar.gz
XOVI_SHA256=32d64d1262ddc984e3235c7d0340a398fe6d5b3efa6a979865f5977b32630d27
KEY=$HOME/.ssh/id_ed25519_remarkable
YES=0
[ "${1:-}" = "--yes" ] && YES=1

say()  { printf '\n== %s\n' "$*"; }
die()  { printf '\nSTOPPED: %s\n' "$*" >&2; exit 1; }
ask()  { # ask "question": returns 0 on yes
    [ "$YES" = 1 ] && return 0
    printf '%s [y/N] ' "$1"; read -r a || a=n
    case "$a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}
sha256q() { if command -v shasum > /dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1; else sha256sum "$1" | cut -d' ' -f1; fi; }

say "1/6 Tools on this computer"
for t in ssh scp python3 file sed grep curl tar; do
    command -v "$t" > /dev/null 2>&1 || die "'$t' is not installed on this computer. See docs/INSTALL.md, 'What you need'."
done
command -v md5 > /dev/null 2>&1 || command -v md5sum > /dev/null 2>&1 || die "neither 'md5' nor 'md5sum' found"
for f in prebuilt/ggsense.so tools/ggtool tools/clock14f.js ghostguard/ghostguard.qmd ghostguard/control.json \
         deploy/20-gglog.conf deploy/persist_install.sh; do
    [ -f "$HERE/$f" ] || die "the file $f is missing: download the whole project again"
done
echo "ok"

say "2/6 Finding the tablet"
B="-o BatchMode=yes -o ConnectTimeout=8"
HOST=${GG_HOST:-}
if [ -z "$HOST" ]; then
    if ssh $B remarkable true 2> /dev/null; then HOST=remarkable
    elif ssh $B root@10.11.99.1 true 2> /dev/null; then HOST=root@10.11.99.1
    fi
fi
if [ -z "$HOST" ]; then
    echo "The tablet does not accept a password-free login from this computer yet."
    echo "Check first: USB cable connected, tablet awake and unlocked, developer mode on (docs/INSTALL.md, step 1)."
    echo "I can set the login up now: a new key file ($KEY) is created on this computer,"
    echo "its public half is added to the tablet, and a 'remarkable' entry is added to ~/.ssh/config."
    echo "You will be asked ONCE for the tablet's root password (Settings > General > Help > About >"
    echo "Copyrights and licenses > General information). It is typed into ssh, not into this script."
    ask "Set up the login now?" || die "no login to the tablet. Set it up yourself (docs/INSTALL.md) and run this again."
    mkdir -p "$HOME/.ssh"; chmod 700 "$HOME/.ssh"
    [ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N "" -C "ghostguard-installer" -f "$KEY"
    ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new root@10.11.99.1 \
        'mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys' \
        < "$KEY.pub" || die "could not log in to root@10.11.99.1 (wrong password, or the tablet is not reachable over USB)"
    if grep -q -E '^[[:space:]]*Host[[:space:]]+remarkable[[:space:]]*$' "$HOME/.ssh/config" 2> /dev/null; then
        die "~/.ssh/config already has a 'remarkable' entry that does not work. Fix or remove it, then run this again."
    fi
    printf '\nHost remarkable\n    HostName 10.11.99.1\n    User root\n    IdentityFile %s\n    IdentitiesOnly yes\n' "$KEY" \
        >> "$HOME/.ssh/config"
    chmod 600 "$HOME/.ssh/config"
    ssh $B remarkable true || die "the new key login does not work; see docs/TROUBLESHOOTING.md"
    HOST=remarkable
fi
ssh $B "$HOST" true || die "cannot reach the tablet as '$HOST'"
echo "tablet reachable as: $HOST"
r() { ssh $B "$HOST" "$1"; }

say "3/6 Firmware check"
ver=$(r 'cat /etc/version' | head -n 1)
xo=$(r 'md5sum /usr/bin/xochitl' | cut -d' ' -f1)
echo "tablet firmware build: $ver"
if [ "$ver" != "$OS_TESTED" ] || [ "$xo" != "$XO_MD5" ]; then
    echo "This build of GhostGuard was made and tested ONLY for the reMarkable Paper Pro with software"
    echo "$OS_NAME (build $OS_TESTED). Your tablet runs something else, so nothing was installed."
    echo "GhostGuard reads private structures of the tablet's main program; on another version it would"
    echo "at best do nothing. See docs/TROUBLESHOOTING.md, 'Other firmware versions'."
    die "untested firmware"
fi
echo "ok: this is the tested firmware ($OS_NAME)"

say "4/6 XOVI (extension loader)"
if r '[ -x /home/root/xovi/start ] && [ -f /home/root/xovi/xovi.so ] && [ -f /home/root/xovi/extensions.d/qt-resource-rebuilder.so ] && [ -s /home/root/xovi/exthome/qt-resource-rebuilder/hashtab ]'; then
    echo "ok: XOVI with qt-resource-rebuilder is already installed"
else
    echo "XOVI is not (fully) installed. It is a separate open-source project by asivery:"
    echo "  $XOVI_URL"
    echo "I can download that exact release, check its checksum and unpack it into /home/root/xovi on the tablet."
    echo "Then XOVI builds its table of the tablet's screens once: the tablet's screen will stop for a few minutes,"
    echo "you press Enter here when asked, and you may have to type your passcode on the tablet."
    ask "Install XOVI now?" || die "XOVI is needed. Install it yourself (docs/INSTALL.md) and run this again."
    T=$(mktemp -d "${TMPDIR:-/tmp}/gg-xovi.XXXXXX")
    curl -fL --max-time 300 -o "$T/xovi.tar.gz" "$XOVI_URL" || die "download failed"
    [ "$(sha256q "$T/xovi.tar.gz")" = "$XOVI_SHA256" ] || die "the downloaded XOVI file is not the expected one (checksum differs); nothing was installed"
    scp $B -q "$T/xovi.tar.gz" "$HOST:/tmp/xovi.tar.gz" || die "copying XOVI to the tablet failed"
    rm -rf "$T"
    if r '[ -e /home/root/xovi ]'; then
        r 'mv /home/root/xovi /home/root/xovi.before-ghostguard.$(date +%s)'
        echo "(an incomplete /home/root/xovi was kept as /home/root/xovi.before-ghostguard.*)"
    fi
    r 'tar -xzf /tmp/xovi.tar.gz -C /home/root && rm -f /tmp/xovi.tar.gz' || die "unpacking XOVI on the tablet failed"
    echo "Building XOVI's table now. Follow what it prints:"
    ssh -t -o ConnectTimeout=8 "$HOST" /home/root/xovi/rebuild_hashtable || true
    r 'systemctl start xochitl' || true
    r '[ -s /home/root/xovi/exthome/qt-resource-rebuilder/hashtab ]' ||
        die "XOVI did not finish building its table. The tablet runs its normal software. Run this installer again."
    echo "ok: XOVI installed"
fi

say "5/6 Installing GhostGuard"
echo "This copies GhostGuard to /home/root/ghostguard and /home/root/xovi on the tablet and adds three small"
echo "start-up units so that it starts by itself about three minutes after every boot (see README, 'Risks')."
ask "Install GhostGuard now?" || die "not installed (nothing was changed in this step)"
r 'mkdir -p /home/root/ghostguard /home/root/xovi/exthome/qt-resource-rebuilder /home/root/xovi/services/xochitl.service'
scp $B -q "$HERE/ghostguard/ghostguard.qmd" "$HOST:/home/root/xovi/exthome/qt-resource-rebuilder/ghostguard.qmd"
scp $B -q "$HERE/deploy/20-gglog.conf" "$HOST:/home/root/xovi/services/xochitl.service/20-gglog.conf"
if r '[ -s /home/root/ghostguard/control.json ]'; then
    echo "(keeping the existing /home/root/ghostguard/control.json)"
else
    scp $B -q "$HERE/ghostguard/control.json" "$HOST:/home/root/ghostguard/control.json"
fi
GG_HOST="$HOST" bash "$HERE/deploy/persist_install.sh" --go --so "$HERE/prebuilt/ggsense.so" \
    --clock "$HERE/tools/clock14f.js" > "$HERE/install.log" 2>&1 || {
        tail -n 15 "$HERE/install.log"; die "the installer step failed; the full output is in install.log"; }
grep -q '^== done' "$HERE/install.log" || { tail -n 15 "$HERE/install.log"; die "the installer step did not finish; see install.log"; }
echo "ok: GhostGuard is installed and armed for the next boot (details in install.log)"

say "6/6 Restart"
echo "GhostGuard starts after a restart of the tablet. Close the document you have open first."
echo "After the restart: leave the tablet awake and do not touch it for about 3-4 minutes."
echo "The screen will go blank once for a few seconds when GhostGuard takes over; that is expected."
if ask "Restart the tablet now?"; then
    r 'systemctl reboot' || true
    echo "Restarting. In about four minutes, check with:  bash status.sh"
else
    echo "Restart it yourself when you are ready (hold the power button > Restart), then check with:  bash status.sh"
fi
