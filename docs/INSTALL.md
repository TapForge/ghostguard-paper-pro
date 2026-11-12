# Installation guide

This guide assumes you have never opened a terminal. Take it one step at a time; the whole thing takes about
30 minutes, most of it waiting.

**Before you start, check these three things.**

1. Your tablet is a **reMarkable Paper Pro** (the 11.8" colour model; not the Paper Pro Move, not a reMarkable 2).
2. Its software version is **3.29.1**. Look at *Settings → General → Software* (the version is shown there).
   If it is another version, stop here: the installer will refuse. See
   [TROUBLESHOOTING.md](TROUBLESHOOTING.md#other-firmware-versions).
3. You have read the [Risks](../README.md#risks) section of the README and accept them.

## What you need

- The tablet and its USB-C cable.
- A computer:
  - **Mac:** nothing to install. Everything needed is already there. (The first time you type `git`, macOS may
    offer to install the "command line developer tools"; accept that and wait for it to finish.)
  - **Linux:** `git`, `ssh`, `curl` and `python3`. On Ubuntu or Debian: `sudo apt install git openssh-client curl python3 file`.
  - **Windows:** install WSL first (open PowerShell as administrator, type `wsl --install`, restart, then open the
    "Ubuntu" app). Inside Ubuntu run `sudo apt update && sudo apt install git openssh-client curl python3 file`.
    Then follow this guide inside the Ubuntu window. *We have not tested the Windows route ourselves.*
- About 30 minutes during which you do not need the tablet.

## Step 1: Turn on developer mode

Developer mode is reMarkable's official way to give you full access to your own tablet. It is described in
reMarkable's own documentation: <https://developer.remarkable.com/documentation/developer-mode>.

**Turning it on erases everything on the tablet.** Notes and documents that are synced to your reMarkable
account come back afterwards; anything that is not synced is lost.

1. Connect the tablet to Wi-Fi and wait until everything is synced (the cloud symbol shows no pending sync).
   If you do not use a reMarkable account, export what you want to keep first.
2. On the tablet open **Settings → General → Paper Tablet → Software → Advanced → Developer mode**.
3. Read the warnings and confirm. The tablet erases itself and restarts. This takes a few minutes.
4. Set the tablet up again (language, Wi-Fi, your account) and let it sync your documents back.

From now on the tablet shows a short warning screen at every start. That is normal in developer mode.

*If you already have developer mode on, skip this step.*

## Step 2: Find the tablet's password

1. On the tablet open **Settings → General → Help → About**.
2. Tap **Copyrights and licenses**.
3. Under **General information** (you may have to scroll) there is a paragraph about SSH with a **username**
   (`root`), an **IP address** (`10.11.99.1`) and a **password** (a short random text).

Keep that screen open or write the password down. You will type it once, in step 5. Do not share it: with it,
anyone who has your tablet and a cable can read everything on it.

## Step 3: Connect the tablet

Plug the tablet into the computer with the USB-C cable. Keep it **awake and unlocked** (enter your passcode if you
have one). If the tablet goes to sleep during the installation, tap its power button and unlock it again.

## Step 4: Download GhostGuard

Open a terminal:

- **Mac:** press `Cmd` + `Space`, type `Terminal`, press `Enter`.
- **Linux:** open your terminal program.
- **Windows:** open the "Ubuntu" app.

Copy the following line, paste it into the terminal and press `Enter`:

```bash
git clone https://github.com/TapForge/ghostguard-paper-pro.git
```

Then this one:

```bash
cd ghostguard-paper-pro
```

(No `git`? On the project's GitHub page click **Code → Download ZIP**, unpack it, and in the terminal type `cd `
followed by a space, drag the unpacked folder into the terminal window, and press `Enter`.)

## Step 5: Run the installer

```bash
bash install.sh
```

The installer works through six parts. It prints what it is about to do and asks `[y/N]` before each change; type
`y` and press `Enter` to go on, or just `Enter` to stop.

| Part | What happens | What you do |
|---|---|---|
| 1/6 Tools | Checks that the computer has what is needed | Nothing |
| 2/6 Finding the tablet | Sets up a password-free login so the later steps can run | Answer `y`. When it says `root@10.11.99.1's password:` type the password from step 2 and press `Enter`. **Nothing appears while you type; that is normal.** If it first asks `Are you sure you want to continue connecting`, it will already have answered for you. |
| 3/6 Firmware check | Compares the tablet's software with the tested one | Nothing. If it stops here, your version is not supported. |
| 4/6 XOVI | Downloads the extension loader from its official page, checks it, puts it on the tablet and lets it prepare itself | Answer `y`. The tablet's screen stops for a few minutes. **When the terminal asks you to press Enter, do so.** If the tablet asks for your passcode, enter it on the tablet. |
| 5/6 Installing GhostGuard | Copies GhostGuard to the tablet and arms it for the next start | Answer `y` |
| 6/6 Restart | Restarts the tablet | Close any open document on the tablet, then answer `y` |

If any part stops with `STOPPED:`, nothing further is changed. Read the message, look it up in
[TROUBLESHOOTING.md](TROUBLESHOOTING.md), fix it and run `bash install.sh` again; parts that are already done are
skipped.

## Step 6: Wait four minutes

After the restart:

1. Unlock the tablet and then **leave it alone, awake, for about four minutes**. Do not tap, do not write.
   GhostGuard waits until the tablet has been untouched for two minutes before it starts, so every touch
   makes it wait longer.
2. The screen goes blank and the interface restarts once. That is GhostGuard taking over.
3. On the computer run:

   ```bash
   bash status.sh
   ```

   The last line should say `RESULT: GhostGuard is running.`

From now on this happens by itself after every start of the tablet: about three minutes after you last touched it,
the interface restarts once and GhostGuard is on. Until then the tablet behaves like stock.

You can unplug the cable. The computer is only needed again to check, change or remove GhostGuard.

## Step 7: Try it

Open a colour PDF and turn some pages. A turn away from a colour page is now one short full refresh and the old
page should be gone. Black-and-white pages still turn without a flash.

If you do not like it, see [SETTINGS.md](SETTINGS.md) for the alternatives, or remove it.

## Removing GhostGuard

Connect the tablet, keep it awake, and in the project folder run:

```bash
bash uninstall.sh --now
```

The interface restarts once and the tablet is back to stock behaviour, also after every later restart.
The command prints two more lines you can run to delete the leftover files.

Developer mode itself stays on. Turning it off needs reMarkable's recovery procedure (see their documentation) and
erases the tablet again.

## After a software update

When the tablet installs a new software version, GhostGuard notices at the next start that the version is not the
tested one and does not start; the tablet simply behaves like stock. To keep GhostGuard, turn off automatic
updates on the tablet (*Settings → General → Software*). Check this project's page for a build for the newer
version before you update.

## Doing it by hand

If you prefer not to run a script that configures things for you, these are the same steps done manually.

1. SSH key login, so that `ssh remarkable` works without a password:

   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_remarkable
   ```

   ```bash
   cat ~/.ssh/id_ed25519_remarkable.pub | ssh root@10.11.99.1 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys'
   ```

   and add to `~/.ssh/config`:

   ```
   Host remarkable
       HostName 10.11.99.1
       User root
       IdentityFile ~/.ssh/id_ed25519_remarkable
       IdentitiesOnly yes
   ```

2. XOVI: follow the instructions of [asivery/rm-xovi-extensions](https://github.com/asivery/rm-xovi-extensions)
   (release `v19-23052026`, file `xovi-aarch64.tar.gz`): unpack into `/home/root`, run
   `/home/root/xovi/rebuild_hashtable`, then `systemctl start xochitl`. Do **not** start XOVI yourself afterwards;
   GhostGuard's boot service does that.
3. Three files:

   ```bash
   scp ghostguard/ghostguard.qmd remarkable:/home/root/xovi/exthome/qt-resource-rebuilder/
   ```

   ```bash
   scp deploy/20-gglog.conf remarkable:/home/root/xovi/services/xochitl.service/
   ```

   ```bash
   ssh remarkable 'mkdir -p /home/root/ghostguard' && scp ghostguard/control.json remarkable:/home/root/ghostguard/
   ```

4. The boot installer (without `--go` it only shows what it would do):

   ```bash
   bash deploy/persist_install.sh --go --so prebuilt/ggsense.so --clock tools/clock14f.js
   ```

5. Restart the tablet and wait as in step 6.
