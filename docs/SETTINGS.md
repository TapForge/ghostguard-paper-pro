# Settings

GhostGuard works without any setting. This page is for people who want to try another page-turn behaviour.

Settings are sent from the computer (tablet on USB, awake) with:

```bash
bash settings.sh KEY VALUE
```

A setting works at once and lasts **until the tablet or its interface restarts**; after that the built-in
defaults are back. There is no settings file that survives a restart in this release. To make another behaviour
permanent, change the default in `ggsense/ggsense-1.8.30.c` and rebuild ([BUILDING.md](BUILDING.md)).

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

Black-and-white page turns are always flash-free. There is no recommended setting to change here.

## Other keys

The engine has many more keys (thresholds, timings, the shape of the compensating drive). They are documented in
the long comment at the top of `ggsense/ggsense-1.8.30.c`. We swept 48 combinations of the shape keys and none
beat the defaults ([WHAT-WE-TRIED.md](WHAT-WE-TRIED.md), section 2.5), so they are best left alone.

## Switching GhostGuard off for a while

On the tablet's side, over SSH:

| Command | Effect |
|---|---|
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh stock'` | Stock behaviour now (the interface restarts once); GhostGuard comes back after the next restart |
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh disable'` | GhostGuard does not start at later restarts (nothing changes now) |
| `ssh remarkable 'bash /home/root/ghostguard/boot/gg.sh enable'` | Undo `disable` |

(If the installer told you the tablet is reachable as `root@10.11.99.1`, use that instead of `remarkable`.)
