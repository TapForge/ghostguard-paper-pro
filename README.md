# GhostGuard for the reMarkable Paper Pro

**Less ghosting and colour residue when you turn pages on a reMarkable Paper Pro.**

On the Paper Pro, a faint copy of the previous page often stays visible after a page turn, and the white background of
colour documents turns patchy (white next to yellowish or lilac). GhostGuard is a small add-on that changes how the
tablet drives its screen during page turns, so that the new page arrives clean:

| Situation | Stock software | With GhostGuard |
|---|---|---|
| Page turn on a colour page | fast partial update, the old page shows through | **one** full refresh (about 1 s), corrected so that the old text does not stay behind |
| Page turn on a black-and-white page | fast partial update, a faint trace | still **no flash**; the trace is reduced by a compensating drive |
| Closing a document, Settings, keyboard closing | a flash that leaves residue | one clean refresh after the screen has settled |
| Menus, pop-ups, typing, handwriting | – | unchanged, no added flash |

It is free, open source, and can be removed with one command.

> **Read this first**
>
> - It works on **exactly one software version**: reMarkable Paper Pro, software **3.29.1** (build `20261028104944`).
>   On any other version the installer refuses and the add-on stays passive. A tablet that updates itself goes back
>   to stock behaviour until this project is updated.
> - It was developed and tested on **one tablet**, judged by **one person's eyes**, installed from **one Mac**.
>   Your panel may behave differently.
> - It needs **developer mode**. Turning developer mode on **erases the tablet** (factory reset; your notes come back
>   from the cloud if you sync first), shows a warning at every start, and reMarkable says damage caused in
>   developer mode is not covered by the warranty.
> - It is not made or endorsed by reMarkable AS. You use it at your own risk. See [Risks](#risks).

## Install in five steps

You need the tablet, its USB-C cable and a computer (macOS or Linux; on Windows use WSL). No programming needed;
you copy and paste three commands. The full guide with every screen and every question is in
**[docs/INSTALL.md](docs/INSTALL.md)**.

1. **Sync your tablet**, then turn on developer mode:
   *Settings → General → Paper Tablet → Software → Advanced → Developer mode*. The tablet erases itself and restarts.
2. **Note the password** shown at *Settings → General → Help → About → Copyrights and licenses → General information*.
3. **Connect the tablet** to the computer with the USB cable. Keep it awake and unlocked.
4. **Download this project and run the installer.** Open a terminal and paste:

   ```bash
   git clone https://github.com/TapForge/ghostguard-paper-pro.git
   ```

   ```bash
   cd ghostguard-paper-pro
   ```

   ```bash
   bash install.sh
   ```

   The installer explains each step and asks before it changes anything. It asks for the password from step 2 once.
5. **Let it restart the tablet**, then leave the tablet awake and untouched for about four minutes. The screen
   restarts once by itself; after that GhostGuard is running. Check with `bash status.sh`.

To remove it: `bash uninstall.sh --now`.

## How do I know it works?

Open a PDF with colour (a textbook, a magazine) and turn a few pages. Each turn away from a colour page is now one
short full refresh, and the page behind it should be gone. In a black-and-white book, page turns stay flash-free.
`bash status.sh` on the computer prints `RESULT: GhostGuard is running.`

## What causes the ghosting?

Short version of what we measured ([docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) has the details and the numbers):

1. **The panel's shared electrode follows the average drive of the whole screen.** Pixels that the software "holds"
   during an update are not really at rest; they get pushed by whatever the rest of the screen is doing, and white
   areas come out tinted.
2. **The panel has two different "whites"** (plain page white, and the white inside shaded or dithered areas).
   On a designed colour PDF about a quarter of all pixels flip between the two at every page turn. They are white
   before and after, yet they get the full, strong white waveform, and that is about 75 % of the disturbance in 1.
3. **A full refresh that changes the content does not erase the old content.** Pixels going from old text to white
   keep a faint dark trace. Only a refresh of a picture that is already on the screen cleans completely.

## What GhostGuard does about it

- **Colour pages:** the turn is sent as one full refresh, and the tablet's waveform table (in memory only) gets a
  short "white tail": seven otherwise idle frames at −12 V on the 43 ink-to-white transitions. That pushes the
  leftover of the old text to white inside the same refresh, so no second pass is needed.
- **Black-and-white pages:** the turn stays flash-free. Held pixels get a small compensating voltage, and the
  white-to-white flips are parked at 0 V for the length of the turn.
- **View changes:** the stock flash is withdrawn and one clean in-place refresh follows when the screen is still.

We tried about twenty other approaches first: cleaning after every page, cleaning every N pages, flash-free
compensation for colour pages, two refreshes per turn, a white screen in between, and more. Each one is listed in
**[docs/WHAT-WE-TRIED.md](docs/WHAT-WE-TRIED.md)** with the reason it was dropped.

## Risks

Please read these before you install.

- **Developer mode.** Factory reset when you turn it on; SSH access to the tablet with a root password; warranty
  and security consequences as described by reMarkable. Leaving developer mode needs reMarkable's recovery tool.
- **One firmware only.** GhostGuard reads private data structures of the tablet's main program (`xochitl`). It
  checks the exact build before doing anything and stays passive on any other. After a software update it switches
  itself off; turn off automatic updates if you want to keep it.
- **It changes waveform data in memory.** The white tail is written into the running program's copy of one
  waveform table (the one for 22–28 °C). Nothing on disk and nothing in the display's own storage is modified, and a
  restart brings the original back. But the tail adds drive to the panel on every full refresh, and **we have not
  measured the long-term effect** on the panel (image retention, DC balance) over weeks or months.
- **Above 28 °C panel temperature** the tablet uses another table; there GhostGuard's colour page turn is a plain
  full refresh and a faint trace can remain.
- **Automatic start.** XOVI, the loader GhostGuard uses, is by design *not* started at boot, and its authors warn
  against starting it automatically, because a broken extension could then keep the tablet from starting its
  interface. GhostGuard does start it automatically, about three minutes after boot, behind these guards: it checks
  the firmware and every file first, waits until the tablet has been untouched for two minutes, watches the
  interface afterwards, falls back to the stock software if anything fails, and disables itself after two bad
  starts. These guards worked on our tablet. They are still our own code, tested on one device.
- **SSH always stays available** over USB in developer mode, so the add-on can be removed even if the interface
  does not come up: `bash uninstall.sh --now`.
- **The installer's first-time path is new.** Setting up the SSH key and installing XOVI on a fresh tablet was
  written for this release and has not been run on a second device yet. The GhostGuard step itself is the one we
  used many times. If something goes wrong, [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) has the way back.

## Everyday commands

Run these on the computer, with the tablet connected by USB and awake.

| Command | What it does |
|---|---|
| `bash status.sh` | Is GhostGuard running? |
| `bash status.sh log` | Its start-up log |
| `bash settings.sh KEY VALUE` | Change a setting until the next restart ([docs/SETTINGS.md](docs/SETTINGS.md)) |
| `bash uninstall.sh` | Remove it (effective after the next restart) |
| `bash uninstall.sh --now` | Remove it and switch back to stock at once |

## Documentation

| Document | For whom |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Everyone: step-by-step installation, including developer mode |
| [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | When something does not work; how to switch it off or remove it |
| [docs/SETTINGS.md](docs/SETTINGS.md) | Choosing another page-turn behaviour |
| [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) | The root cause, the measurements, the waveform table format |
| [docs/WHAT-WE-TRIED.md](docs/WHAT-WE-TRIED.md) | Every approach we tried and why it was not chosen |
| [docs/BUILDING.md](docs/BUILDING.md) | Building from source, porting to another firmware |

## What is in this repository

```
install.sh, uninstall.sh, status.sh, settings.sh   the four commands for users
prebuilt/ggsense.so     the engine, built from ggsense/ggsense-1.8.30.c (aarch64)
ggsense/                engine source, unit tests, fuzz test, build script
ghostguard/             the small QML part that runs inside the tablet's interface
deploy/                 the boot-time installer, guards, rescue and rollback scripts
tools/                  helper programs (input watcher, table tool, command sender)
docs/                   the documents above
```

No reMarkable software, waveform file or other proprietary data is included.

## Tested on

| | |
|---|---|
| Tablet | reMarkable Paper Pro (11.8", Gallery 3 colour panel, 1620 × 2160) |
| Software | 3.29.1, build `20261028104944` |
| Loader | XOVI with qt-resource-rebuilder, release `v19-23052026` of [asivery/rm-xovi-extensions](https://github.com/asivery/rm-xovi-extensions) |
| Computer | macOS (the scripts are plain bash and should run on Linux and WSL; not tested there) |
| Panel temperature | 25–27.5 °C |

The Paper Pro Move and the reMarkable 1 and 2 are **not** supported.

## Helping

- If it works or does not work on your tablet, please open an issue and say which software version you have,
  what kind of document you read, and what you see. Reports from a second device are worth a lot.
- If you port it to another software version, [docs/BUILDING.md](docs/BUILDING.md) lists what is version-specific.

## Credits and licence

- GhostGuard is released under the [MIT licence](LICENSE).
- It relies on [XOVI](https://github.com/asivery/xovi) and
  [rm-xovi-extensions](https://github.com/asivery/rm-xovi-extensions) by asivery, which are separate projects with
  their own licences; the installer downloads them from their official release page and does not redistribute them.
- reMarkable is a trademark of reMarkable AS. This project is not affiliated with reMarkable AS.
- The engine was developed with the help of Claude (Anthropic) in a long measure-and-try loop on the owner's
  tablet; the owner's eyes made every final decision.
