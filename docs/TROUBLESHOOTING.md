# Troubleshooting

All commands are run on the computer, in the project folder, with the tablet connected by USB, awake and unlocked.

## The quick way out

If anything about the tablet's behaviour worries you:

```bash
bash uninstall.sh --now
```

The interface restarts once and the tablet runs its stock software, now and after every restart.
Your notebooks and documents are not touched by installing or removing GhostGuard.

If the screen shows nothing useful but the tablet is on, the same command still works: it only needs the USB
connection. As a last resort, restart the tablet (hold the power button for about 10 seconds, then press it again):
for the first three minutes after every start the tablet always runs its stock software, which is enough time to
run the uninstall command.

## The installer says STOPPED

| Message | Meaning and fix |
|---|---|
| `'…' is not installed on this computer` | A tool is missing. See "What you need" in [INSTALL.md](INSTALL.md). |
| `could not log in to root@10.11.99.1` | Wrong password, or no connection. Check: cable plugged in (try another cable or port; some cables only charge), tablet awake and unlocked, developer mode on. The password is case-sensitive; type it exactly as shown on the tablet. |
| `~/.ssh/config already has a 'remarkable' entry that does not work` | You (or another tool) set up an entry earlier. Open `~/.ssh/config` in a text editor and fix or delete the `Host remarkable` block, then run the installer again. |
| `untested firmware` | Your tablet does not run software 3.29.1. See [Other firmware versions](#other-firmware-versions). |
| `the downloaded XOVI file is not the expected one` | The download was incomplete or the release file changed. Try again; if it repeats, open an issue. Nothing was installed. |
| `XOVI did not finish building its table` | Run `bash install.sh` again and follow XOVI's prompts: press Enter in the terminal when asked, and enter your passcode on the tablet if it asks. |
| `the installer step failed` | Look at the end of `install.log` in the project folder. Common reason: the tablet fell asleep during the copy. Wake it and run the installer again. |

Running `bash install.sh` again is always safe: it skips what is already done.

## `status.sh` says GhostGuard is not running

- **Right after a restart** this is normal. GhostGuard starts 70 seconds after boot at the earliest and then waits
  for two minutes without any touch or pen input. Leave the tablet awake and untouched and try again in four minutes.
- **The tablet was asleep.** Sleep pauses the wait. Wake it, leave it alone for two minutes.
- **`disabled:` shows a reason.** GhostGuard switched itself off, usually after two strikes: the interface
  crashed or did not come up twice with GhostGuard, so it stays off. Run `bash status.sh log`, open an issue with
  that log, and if you want to try again: `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh enable'`.
- **The tablet updated its software.** Then the start-up check fails at every boot and GhostGuard stays off; see
  [Other firmware versions](#other-firmware-versions).
- Anything else: `bash status.sh log` shows what the start-up script decided and why.

## It runs, but I still see ghosting

- **Check what kind of page it is.** Only turns *away from* a page with real colour get the full refresh.
  With the `bwturn` extra (installed by default) black-and-white pages turn like colour pages. Without it they
  are turned without a flash and keep a small trace by design; after many such pages a view change (closing the
  document, opening the page overview) cleans the screen.
- **The first turn after opening a document** and a turn from a black-and-white page onto a colour page are not
  treated specially in this release.
- **Warm tablet.** Above 28 °C panel temperature (long use while charging, sunlight) the correction for colour
  pages is not active; a faint trace of the old text can remain.
- **Your panel may differ from ours.** Please open an issue with a photo, the document type and the output of
  `bash status.sh`.

You can also try the double pass ([SETTINGS.md](SETTINGS.md)): it is slower and flashes twice, but it was the
cleanest variant in our tests.

## An extra does something I do not like

The four start-up extras (README, "Start-up extras") are each easy to undo.

- **Now, until the next restart:** `bash settings.sh tf_chroma 25` (`bwturn`: colour pages only), `bash settings.sh defer 1` (`viewchange`), `bash settings.sh vs_open 0` and `bash settings.sh kb_open_full 0` (`settingskb`).
- **For good:** the extras send their settings again at every start, so run the installer again with another set and restart the tablet: `GG_EXTRAS="bwturn ggrefresh" bash install.sh` keeps only the named ones, `GG_EXTRAS=none bash install.sh` installs none.
- **An extra shows `<name> not confirmed`** in the start's result line (`bash status.sh log`). GhostGuard still runs; the extra did not take. Restart the tablet; if it stays, open an issue with the log.

## The refresh button is not there

The button (extra `ggrefresh`) sits in the document toolbar, right below undo/redo. Check:

- The document toolbar is showing; the button moves and hides with it.
- GhostGuard has started. The button is made after every start of the interface, so after a boot it appears only a few minutes later. `bash status.sh` should say `RESULT: GhostGuard is running.`
- The result line names `ggrefresh on` (`bash status.sh log`). If the extra was not installed, install again without `GG_EXTRAS=none`.

## The page turn flashes and I do not like it

That is the trade this project makes on colour pages: one refresh of about a second instead of a ghost.
[SETTINGS.md](SETTINGS.md) shows the flash-free mode (`turn_full 0`); expect a patchy background on colour pages.

## Other firmware versions

GhostGuard finds things inside the tablet's main program by their exact position, which changes with every
software version. That is why it is pinned to one build and refuses all others.

- **Newer or older software on a Paper Pro:** not supported by this release. Watch the project page for an update,
  or see [BUILDING.md](BUILDING.md) if you want to port it yourself.
- **The tablet updated itself and GhostGuard stopped:** expected. It stays off and harmless. Either wait for a new
  release or remove it with `bash uninstall.sh`.

## The tablet asks for the password again / `ssh remarkable` does not work

A factory reset or leaving and re-entering developer mode gives the tablet a new password and forgets your key.
Run `bash install.sh` again; it sets the login up again. If SSH complains that the *host key has changed*, run
`ssh-keygen -R 10.11.99.1` once and try again.

## Removing every trace

After `bash uninstall.sh --now`:

```bash
ssh remarkable 'rm -rf /home/root/ghostguard /home/root/gg-staging /home/root/gg-backup'
```

```bash
ssh remarkable 'rm -f /home/root/xovi/exthome/qt-resource-rebuilder/ghostguard.qmd /home/root/xovi/services/xochitl.service/20-gglog.conf'
```

XOVI itself lives in `/home/root/xovi`. If you do not use it for anything else you can delete that folder too
(`ssh remarkable 'rm -rf /home/root/xovi'`) once the tablet runs its stock software. On the computer, the login
key is `~/.ssh/id_ed25519_remarkable` and the `Host remarkable` block in `~/.ssh/config`.

## Asking for help

Open an issue on the project page and include:

- the output of `bash status.sh` and `bash status.sh log`,
- the tablet's software version,
- what you did and what you saw (a photo helps; please make sure no private document is readable in it).

Never post your tablet's password.
