# Settings

GhostGuard works without any setting. This page is for people who want to try another page-turn behaviour.

Settings are sent from the computer (tablet on USB, awake) with:

```bash
bash settings.sh KEY VALUE
```

A setting works at once and lasts **until the tablet or its interface restarts**; after that the built-in
defaults are back. There is no settings file that survives a restart in this release. To make another behaviour
permanent, change the default in `ggsense/ggsense-1.8.32.c` and rebuild ([BUILDING.md](BUILDING.md)).

Some settings are sent by the start-up extras (README, "Start-up extras") at every start of the interface. A key an
extra sends therefore comes back at the next start. To change it for good, install again with another set, for
example `GG_EXTRAS=none bash install.sh` or `GG_EXTRAS="bwturn ggrefresh" bash install.sh`, and restart the tablet.

The command prints the engine's answer, a line like `GGSENSE ... -> turn_full 1`.

## Page turns on colour pages

"Colour page" means: the page you are leaving holds real colour (a coloured heading, a picture, a highlight).
Scanned black-and-white books and pages with only a few coloured dots are not colour pages.

| Behaviour | Commands | What you see | Verdict of the tester's eye |
|---|---|---|---|
| **One full refresh with the white tail** (default) | `turn_full 1` and `wt_static 1` | The page arrives with one refresh of about 1 s | "Great" – chosen |
| One full refresh without the tail | `turn_full 1` and `wt_static 0` | The same, a very faint trace of the old text | Not good enough |
| Double pass | `wt_static 0`, `cd_std 1`, `turn_full 2` | The page arrives with a full refresh and is refreshed once more in place about 1.1 s later | Very clean, but the second flash is annoying |
| Flash-free | `turn_full 0` and `cd_std 1` | No flash at all; the background of colour documents stays patchy | Rejected |

Two more modes exist in the code and are **not recommended**: `turn_full 3` (the old page is first replaced by a
white screen, then the new page appears; clean but slow and unpleasant) and `cd_std 2` (flash-free turn followed
by a clean; the page visibly darkens first).

Example: switch to the double pass until the next restart:

```bash
bash settings.sh wt_static 0
```

```bash
bash settings.sh cd_std 1
```

```bash
bash settings.sh turn_full 2
```

And back to the default:

```bash
bash settings.sh turn_full 1
```

```bash
bash settings.sh wt_static 1
```

## Black-and-white pages

With the `bwturn` start-up extra (installed by default) the engine gets `tf_chroma 0`: every page with content turns like a colour page, with one full refresh and the white tail. A blank page still turns without a flash. Without the extra, the build's default is `tf_chroma 25` (only colour pages turn that way) and black-and-white page turns are flash-free.

| Key | Values | Meaning |
|---|---|---|
| `tf_chroma` | default 25; 0 = every page with content turns like a colour page | The share of colour that makes a page a colour page |

Go back to the default until the next restart:

```bash
bash settings.sh tf_chroma 25
```

## View changes, Settings and the keyboard

| Key | Values | Meaning |
|---|---|---|
| `defer` | 0 or 1 (the clock sends 1 at every start, the `viewchange` extra then sends 0) | 1: at a view change the engine withdraws xochitl's refresh and cleans in place later. 0: xochitl's own full refresh stays at the change |
| `vs_open` | 0 to 3, default 0 | Bit mask: 1 = opening Settings, 2 = closing it, 3 = both. These view changes get the deferred cycle even with `defer 0`. The `settingskb` extra sends 3 |
| `vs_quiet` | 0.1 to 1.0 s, default 0.2 | How long nothing must change before that clean runs. The extra sends 0.2 |
| `kb_open_full` | 0 or 1, default 0 | 1: one whole-screen in-place clean about 0.5 s after the on-screen keyboard opens. Never while typing or with the pen near. The extra sends 1 |

`vs_open`, `vs_quiet` and `kb_open_full` need ggsense 1.8.31 or later (closing Settings, bit 2 of `vs_open`, needs 1.8.32). With their defaults the engine behaves exactly like 1.8.30. The status JSON shows them as `"vs":[vs_open, vs_quiet, cycles, cycles ended early]` and `"kbFull":0|1`.

Undo until the next restart:

```bash
bash settings.sh defer 1
```

```bash
bash settings.sh vs_open 0
```

```bash
bash settings.sh kb_open_full 0
```

## Other keys

The engine has many more keys (thresholds, timings, the shape of the compensating drive). They are documented in
the long comment at the top of `ggsense/ggsense-1.8.32.c`. We swept 48 combinations of the shape keys and none
beat the defaults ([WHAT-WE-TRIED.md](WHAT-WE-TRIED.md), section 2.5), so they are best left alone.

## Switching GhostGuard off for a while

On the tablet's side, over SSH:

| Command | Effect |
|---|---|
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh stock'` | Stock behaviour now (the interface restarts once); GhostGuard comes back after the next restart |
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh disable'` | GhostGuard does not start at later restarts (nothing changes now) |
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh enable'` | Undo `disable` |

(If the installer told you the tablet is reachable as `root@10.11.99.1`, use that instead of `remarkable`.)
