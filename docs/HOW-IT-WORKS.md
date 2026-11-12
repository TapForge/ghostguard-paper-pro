# How it works

This document explains what causes ghosting on the reMarkable Paper Pro, what we measured, and what the fix (ggsense 1.8.32) does. It also lists what we did not verify.

Short version: the panel's shared electrode drifts with the average voltage of each refresh. Pixels that are not driven pick up that drift, and pixels that are driven from old text to white keep a faint trace. ggsense changes how page turns are driven so that both effects get smaller.

## 1. What you see

On the tested tablet, with the stock software:

- **Ghost text.** After you turn a page, a faint copy of the previous page's text shows through the new page.
- **Patchy background on colour documents.** The white of the page looks uneven. Some areas are white and others slightly yellowish or lilac.
- **Colour residue.** Tints from the old page, from menus, or from the on-screen keyboard stay on the screen for a while.

The effect is strongest on colour PDFs (for example a designed textbook with coloured boxes and shaded table rows). Plain black-and-white pages show much less of it.

## 2. The tested device

| Item | Value |
|---|---|
| Tablet | reMarkable Paper Pro |
| Firmware | 3.29.1 (build 20261028104944) |
| Panel | Gallery 3 colour e-ink, 1620 x 2160 pixels |
| Number of devices tested | **One** |
| Panel temperature during most tests | about 25 to 27.5 C |

Everything below was measured on this one device. We do not know how other units, other firmware versions, or other temperatures behave. The library checks the firmware build and does nothing on any other build (see section 8).

## 3. Terms used in this document

- **Waveform.** A table of voltages that the display controller applies to each pixel over a series of frames. Each pixel has a start level and a target level, and the table says what to apply in each frame.
- **STD.** The name of the standard full-quality waveform the firmware uses for a "full refresh". On this firmware it has 86 frames and takes about one second. It flashes the screen.
- **Level.** The panel's internal grey/colour state per pixel, a number from 0 to 31. Level 0 is black.
- **Co-drive.** Our name for an extra voltage that we add to pixels that would otherwise be held at 0 V, to cancel the drift of the shared electrode (section 4).
- **xochitl.** The reMarkable user interface program. It draws everything on the screen.
- **XOVI.** A third-party framework that loads extra libraries into xochitl at start-up.
- **ggsense.** The small library described here. It is loaded into xochitl through XOVI.

## 4. Root cause, as measured

### 4.1 The shared electrode follows the mean drive

An e-ink panel has one electrode shared by all pixels (the "common electrode"). A pixel feels the voltage between its own electrode and this common one.

Our measurements showed that during a refresh the common electrode follows the **average drive of the whole frame**. Call this average `m(t)`. A pixel that the waveform leaves at 0 V does not stay still. It sees `-m(t)` relative to the common electrode. So white areas that are "held" while text elsewhere changes get slightly pushed, and they end up lilac or darker.

The stock firmware holds every pixel whose level does not change. In a page turn about 43 to 56 percent of the page keeps its level, so a lot of the page is exposed to this drift.

### 4.2 Two kinds of white

The panel has two white levels that look the same to the eye:

- **Level 30:** plain page white.
- **Level 28:** the white inside dithered or shaded areas (white plus single coloured dots), as used in pale table rows and boxes in designed colour PDFs.

The STD waveform treats the two as one state for almost all targets. But a pixel that changes from 28 to 30 or from 30 to 28 counts as "changed". It gets the full white-refresh row, which drives it hard towards dark for about 26 frames and then back to white. It is white before and after.

On the colour PDF we measured (state buffers of two consecutive pages), **21 to 27 percent of all pixels flip between the two whites at every page turn**. These flips produce about **75 percent of the turn's mean drive**. With the flips the peak mean drive was -10.3 V and the sum was 446 V x frames. Without them it was -4.0 V and 114 V x frames. So the biggest source of drift on a colour page is pixels that are white both before and after.

### 4.3 A refresh that changes content leaves a trace

A full refresh of a screen that already shows the final image ("in place") erases ghosts. This was confirmed by eye on the panel.

A full refresh that **changes** the content (old page to new page) does not. Pixels that go from old text to white take the direct path from the old level to the new one and keep a faint dark trace of the old text. The stock flash at view changes (Settings, library) behaves the same way.

Black to white is also special: the waveforms for 0 to 30 and 30 to 0 are the same in all five main waveform families we looked at (30 frames, one direction, 564 V x frames). Black and white are never "washed". Only a same-level refresh equalises them.

### 4.4 Pixels held at 0 V during a full refresh get tinted

We first designed the flip fix as a permanent change: the 28/30 flip rows of the table written to 0 V and left there. This was refuted by measurement before it was installed. With the flip rows at 0 V, a full refresh next to pixels that take plus or minus 24 V left pale areas deep blue. The camera measure was 67.9 against 1.6 with the stock rows. So the flip rows may be at 0 V only during flash-free turns. They must be back at their stock values before any full refresh.

## 5. How these things were measured

Only what the project records support:

- **Camera rig.** A fixed camera photographed the tablet under constant light. A number was computed from the photo: the standard deviation of a high-pass filter at letter scale, on white pixels only, in parts per thousand. A clean in-place refresh gives about 1.5 to 1.64 (the floor). Values of 3 to 5 mean a visible ghost. The camera resolves about 2.6 to 2.7 panel pixels per camera pixel, so very fine detail is not visible.
- **Journal of the display driver.** xochitl writes its display updates (region, update class, timing) to the system journal. The library reads these lines to know what the screen is doing. We also used the journal to time page turns and passes.
- **Waveform table reading.** We read the waveform file and the live tables in xochitl's memory. We read the per-pixel state buffer of the display controller to see which pixels flip between 28 and 30.
- **Lesson recorded.** Early camera checks looked at average tone of large boxes. They missed a heavy ghost that was visible in real reading at letter scale. After that, ghosting was judged at letter scale and in real reading, and a final judgement was left to the owner's eye.

Camera numbers are evidence, not proof of what the eye sees. The faint trace of the old text that matters most here was **not reliably visible to the camera**.

## 6. How ggsense works

ggsense is a shared library loaded into xochitl at start-up through XOVI. It does the following:

1. **Observes.** It hooks the logging function so it sees xochitl's display-update lines. It also reads the frame image that xochitl keeps, after checking the firmware build ID.
2. **Classifies each page turn.** It samples the page being left and decides whether it "holds colour" (a coloured heading, picture or highlight) or is black-and-white or sparse colour. A scanned black-and-white book has dithered greys from all primaries in balance, which we do not count as colour. A page counts as colour when the unbalanced part of its colour samples is at least 25 percent of the colour samples and at least 30 of 10000 samples overall. On the 109 page buffers on record, scanned pages were 6.3 to 6.9 percent and colour pages of a text book were 66.7 to 83.8 percent.
3. **Changes how the turn is driven.** The next section explains how.
4. **Stays passive elsewhere.** In any process other than xochitl (for example the PDF renderer) it does nothing.

Small additional parts are a QML clock that runs inside the UI and tells the library when the screen has settled, and a watchdog that unloads the library if xochitl crashes after install.

## 7. The chosen behaviour (version 1.8.32 with the start-up extras)

The engine alone behaves like 1.8.30. Four small start-up extras (`tools/*.js`, sent after every start of xochitl; see the README, "Start-up extras") change some of its settings and add a button. This section says what happens with all four installed and what happens without them.

### 7.1 Black-and-white and sparse-colour pages

With the `bwturn` extra (`set tf_chroma 0`) these pages turn the way colour pages do: one full refresh with the white tail (7.2), with no visible trace. A blank page still turns without a flash. The owner asked for this and kept it.

Without the extra (the build's default, `tf_chroma 25`), these page turns get **no flash**. The library uses co-drive:

- Cells of the waveform for held pixels get a small voltage that cancels `m(t)`.
- The 28-to-30 and 30-to-28 flip rows are held at 0 V for the length of the turn, so the white-to-white flips stop making drift. They are put back to stock about 6 seconds after the last turn, and at once before any full refresh.
- A companion update covers the strip outside the changed rectangle.

The result on the camera is at the floor for scanned black-and-white pages.

### 7.2 Pages that hold colour: one full refresh with a white tail

When the page being left holds colour, the turn goes out as **one full STD refresh**. There is no second pass and no white intermediate screen.

To get rid of the faint trace, ggsense has changed the STD table in RAM. In the 22 to 28 C table there are 43 "ink to white" rows (from a level of 0 to 26 to a target of 28 or 30). Their frames 68 to 71 and 74 to 76 are idle in the stock table. We write **-12 V** into those frames. This is 3 cells per row, 129 cells in total (frames 68 to 71 and 74 to 76 fall into three words per row). The extra pushes the leftover of the old text towards white inside the same refresh.

It is written once per table, when the library first sees an STD update. It is not written to any other table.

### 7.3 View changes

At view changes (closing a document, opening or closing Settings, the keyboard, the page overview) the stock firmware flashes while changing content. A refresh that changes content does not erase the old content, so the old view can leave a trace.

- **Without the extras (`defer 1`, set by the clock).** ggsense withdraws that flash, the new view is drawn over the old one (a visible ghost), and **one** full-screen in-place refresh follows 0.6-0.9 s later. That refresh erases the trace. The owner found "ghost first, then a hard flash" after closing a document worse than the alternative below.
- **With the `viewchange` extra (`defer 0`).** xochitl's own full refresh stays at the moment of the change, so the first frame of the new view goes out as one full refresh. A full refresh that changes the content can keep a faint trace of the old view. For Settings it did: the library's side menu stayed visible behind the Settings page, and closing Settings left the whole Settings page as a heavy ghost on the library.
- **With the `settingskb` extra (ggsense 1.8.31 or later).** `vs_open` is a bit mask (1 = opening Settings, 2 = closing it, 3 = both). For these view changes the deferred cycle comes back even with `defer 0`: xochitl's refresh is withdrawn, the new view is drawn by its own update, and one whole-screen in-place clean follows once nothing has changed for `vs_quiet` seconds (logged 0.41-0.52 s after the change). It is still one flash, only later and on the finished view, so it erases the trace. If Settings is closed before the opening clean ran, one clean serves both. Every other view change keeps `defer 0`.
- **Keyboard.** When the on-screen keyboard opened over a menu, the menu stayed visible inside the keyboard. With `kb_open_full 1` (also from `settingskb`), the opening gets one whole-screen in-place clean about 0.5 s later (logged 0.43-0.71 s), before the first key. It never runs while typing or with the pen near, and a key press drops it. The clean when the keyboard closes is unchanged.

Only an in-place full refresh erases a trace; the flash-free ways we tried earlier left traces.

### 7.3a The refresh button

The `ggrefresh` extra adds a button to the document toolbar, right below undo/redo, with the interface's own counter-clockwise-arrows icon. A tap is one full in-place refresh of the screen (at most one every 2.5 s, not while the tablet sleeps). It is a pure QtQuick overlay: none of xochitl's own items is changed. It moves and hides with the toolbar and is made again after every start of xochitl.

### 7.4 No extra flash for menus, popups, typing, writing

ggsense does not add any flash for menus, popups, typing or writing. It never cleans while the pen is near or while you type: at the on-screen keyboard the only cleans are the ones in 7.3 (when it opens, before the first key, and when it closes).

## 8. The STD table format, as found

For the temperature range 22 to 28 C the STD table has 86 frames. Each cell is a 16-bit word that holds 5 frames of 3 bits each.

```
word index = (frame / 5) * 1025 + (from << 5 | to)
```

`from` and `to` are the start and target levels (0 to 31). The 3-bit code in each slot means:

| Code | Voltage |
|---|---|
| 0 | idle (no code) |
| 1 | -24 V |
| 2 | -12 V |
| 3 | -6 V |
| 4 | +6 V |
| 5 | +12 V |
| 6 | +24 V |
| 7 | 0 V |

A long ink-to-white row in this table looks like this: frames 47 to 59 at -24 V, 60 to 64 at -12 V, a few -6 and -12 V frames, four idle frames (68 to 71), two more pulses, then twelve idle frames (74 to 85). Shorter rows (from the grey edge pixels of letters, levels 15, 19 and 21 to 30) finish by frame 29 and are idle after that. The white tail uses the idle frames 68 to 71 and 74 to 76.

## 9. Honest limits

- **One temperature table only.** The white tail exists only in the 22 to 28 C table. At 28 C and above the panel uses another table (the tables of 28 to 50 C also have 86 frames, but they are different objects with different rows). Colour pages then turn with a plain single full refresh, with its faint trace. The panel in our tests sat at 25 to 27.5 C, close to the limit.
- **RAM only.** The change is in memory. The library re-applies it after each start. Nothing on the panel's storage and nothing in the firmware files is modified. A reboot or an xochitl restart returns the stock table until the library has seen an STD update again.
- **Long-term effect not measured.** The tail adds -12 V frames to ink-to-white pixels. The net charge is about -84 V x frames per ink-to-white transition. Every STD update, menus included, now drives those seven frames. We did not measure what that does to DC balance or the panel over weeks or months.
- **Firmware specific.** The library finds functions by address and checks the build ID. On any other firmware build it stays passive.
- **One person, one device.** The final judgement was made by one person's eye on one device. Several rounds in which the camera said "clean" did not agree with the eye.
- **The camera is not a reliable judge here.** It could not see the faint trace reliably.
- **Measured at about 26 C.** Nothing was tried at 28 C or above.

## 10. Rolling back

`bash uninstall.sh --now` on the computer removes the auto-start and switches the tablet back to its stock software with one restart of the interface. That restart also brings back the stock waveform table, because the change exists only in the memory of the running program. Single behaviours can be switched at runtime, see [SETTINGS.md](SETTINGS.md).

## See also

- [WHAT-WE-TRIED.md](WHAT-WE-TRIED.md): every approach we tried and why it was not chosen.
- [INSTALL.md](INSTALL.md), [SETTINGS.md](SETTINGS.md), [TROUBLESHOOTING.md](TROUBLESHOOTING.md), [BUILDING.md](BUILDING.md)
