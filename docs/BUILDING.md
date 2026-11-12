# Building from source, and porting

You do not need this to use GhostGuard: a built engine is in `prebuilt/ggsense.so`.

## Build

Needs a C compiler on the computer for the tests (`cc`: Xcode command line tools on macOS, `build-essential` on
Linux) and the zig compiler as a Python package for the tablet binaries (`python3 -m pip install ziglang`).

```bash
bash ggsense/build.sh host
```

compiles and runs the unit tests and the fuzz test on the computer (address and undefined-behaviour sanitizers on).
The last lines should read `ALL OK (0 failures)` and `invariant violations: 0`.

```bash
bash ggsense/build.sh
```

runs the same tests and then builds `prebuilt/ggsense.so` and `tools/ggtool` for the tablet (aarch64, glibc 2.31).
`tools/tabset.c` (a test tool that writes cells of a live waveform table through `/proc/PID/mem`) is built the same
way when you need it; the installer does not use it.

Notes:

- The prebuilt library is the same source built with `-O2 -s`. The copy on the developer's tablet was built
  without `-s`, so its checksum differs from the one here; the code is the same.
- A few unit tests compare against measurement files and a dump of the tablet's waveform table. Those files are
  proprietary or personal and are not in this repository; without them these tests print
  `(no TESTDATA: ...)` and are skipped.

Install your own build with `bash install.sh` (it takes `prebuilt/ggsense.so`), or directly:

```bash
bash deploy/persist_install.sh --go --so prebuilt/ggsense.so --clock tools/clock14f.js
```

The boot scripts pin the checksum of the library they were installed with; a library copied over it by hand is
refused at the next boot. Always go through the installer.

## How the pieces fit

| Piece | Role |
|---|---|
| `ggsense/ggsense-1.8.30.c` | The engine: one C file, loaded into `xochitl` with `LD_PRELOAD` through XOVI's service drop-in. Its header comment (about 400 lines) is the design history and the list of runtime keys. |
| `ghostguard/ghostguard.qmd` | A qt-resource-rebuilder patch: adds a small QML object to the interface that reads `/home/root/ghostguard/control.json` every 3 s and evaluates the command in it. This is the command channel (`tools/ggq.py`). |
| `tools/clock14f.js`, `tools/kbd_overlay.js` | QML snippets sent at start: a clock that tells the engine when the screen has settled, and a watcher for the on-screen keyboard. |
| `deploy/boot/autostart.sh` | Runs 70 s after boot from a systemd timer: checks everything (the "gates"), waits for 120 s without input, starts XOVI, checks health, starts the crash guard. |
| `deploy/boot/rescue.sh`, `deploy/gg_autorollback.sh` | The way back to stock when the interface fails to start or crashes during the first 15 minutes. |
| `deploy/boot/gg.sh` | `status`, `disable`, `enable`, `stock`, `gates`, `log` on the tablet. |
| `tools/ggtool.c` | Small helper on the tablet: watches the input devices for quiet time, reads memory for diagnostics. |

## What is specific to firmware 3.29.1

Porting to another software version means re-finding all of these. None of it is guesswork-safe: a wrong offset
reads or writes the wrong memory in the tablet's main program.

1. **The build pins.** `OS_TESTED` and `XO_MD5` at the top of `deploy/boot/autostart.sh` and in `install.sh`
   (`/etc/version` and the md5 of `/usr/bin/xochitl`), and the build ID the engine checks before it reads anything
   (`xo_buildid` in the engine source).
2. **Addresses and layouts inside `xochitl`.** The engine locates the framebuffer object, its image, the
   display-update call and the waveform table objects by offsets that were found by reading this one build. They
   are constants near the top of the engine source, each with a comment on how it was found and which check
   guards it.
3. **The log lines.** The engine learns what the screen is doing from `xochitl`'s own debug log categories
   (`deploy/20-gglog.conf` turns them on). Their wording can change between versions.
4. **The waveform table.** The white tail assumes the STD table of 22–28 °C with 86 frames and the cell layout
   described in [HOW-IT-WORKS.md](HOW-IT-WORKS.md), section 8. The engine measures the table before writing and
   refuses anything that does not look exactly like the stock one, so on a different table it writes nothing.
5. **The QML patch.** `ghostguard.qmd` hooks into a QML file of the interface; qt-resource-rebuilder's hash table
   must be rebuilt after every software update anyway (`xovi/rebuild_hashtable`).

A port should start in observe-only mode: every engine feature can be switched off with runtime keys, and the unit
and fuzz tests run without a tablet.

## Development method, for the curious

- Every change was first replayed on the computer against saved journals and state dumps, then fuzzed, then
  installed.
- A fixed camera gave a number for "how much of the previous page is in this one". It was useful to reject bad
  ideas quickly. It was **not** able to confirm good ones: several variants that the camera called clean were
  rejected by the eye. Final decisions were made by reading real documents.
- The tablet was never left to arm an untested build unattended: the boot path falls back to stock on any failed
  check, and two bad boots disable it.
