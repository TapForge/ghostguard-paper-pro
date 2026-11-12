# What we tried

This is the honest history of the fix, in order, including the approaches that did not work. The goal, in the words of the device owner (translated):

> Page turns should be fast, leave no trace, and have no hard flash.

All tests ran on one reMarkable Paper Pro (firmware 3.29.1). The words "camera" and "eye" matter in the tables below.

- **Camera** means a fixed camera and a number computed from its photos. It is a measurement, but it could not reliably see the faintest traces.
- **Eye** means the judgement of one person (the owner) reading real documents. In the end this was the only judge that mattered. It is one opinion.

For the terms (STD, co-drive, waveform, xochitl) see [HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## 1. Summary table

| # | Version / date | Idea | Result | Why not kept |
|---|---|---|---|---|
| 1 | v7.1 (early) | Tie a clean to the moment of a view change; shadow mode | Did nothing useful | The stock flash at a view change changes content, so it does not erase the trace. Review also found four logic bugs |
| 2 | 1.3 (8 Nov) | Find the screen frame by tracking images | Crashed xochitl three times | It read freed memory. Fixed in 1.4 by reading only the framebuffer's own image after checks |
| 3 | 1.5 | Withdraw the stock flash at a view change, do one in-place clean once the screen settles | Good on Settings (eye) | Kept as the base for view changes |
| 4 | 1.6 + QML clock 10b | A need-based clean after large updates in documents, decided by a residue model | Fewer flashes, but partial boxes | Model saw only brightness; boxes left seams; timing wrong |
| 5 | 1.7 + clock 11 (9 Nov) | More triggers, more cleans | More flashes and more traces (a flash even when opening a side menu) | Rolled back within hours |
| 6 | 1.8 to 1.8.4 | Triggers from xochitl's own log lines; every clean over the whole screen | No more seams or lilac strips | Partial cleans stretch overlapping updates, so all became full-screen |
| 7 | 1.8.4 | A whole-screen clean after every page turn | Clean, but a flash on every page | Owner: unusable for reading |
| 8 | (setting) | Page cleans off | Old pages piled up after four turns | Not acceptable |
| 9 | 1.8.5 / 1.8.6 | Clean only after N turns or above a threshold | 1.8.5 never installed; a high threshold still left traces | 1.8.6 cleaned any turn with a trace, at once. Still a flash on many pages |
| 10 | 1.8.8 to 1.8.17 | Co-drive for black-and-white turns | No flash on b/w pages; small trace left | Kept for b/w |
| 11 | 1.8.18 to 1.8.21 | Co-drive for STD (colour) turns, then a 48-setting shape sweep | Even tones, but letter-scale ghost on colour pages | The sweep could not beat the default |
| 12 | 1.8.22 | Clean after every colour turn | Cleaned, but flashed on scanned b/w books too | Owner: a flash on every page is very bad |
| 13 | 1.8.23 | No clean; per-turn white tail written into the co-driven table | Some gain, not enough | Never reached the clean floor |
| 14 | 1.8.25 | Hold the two-whites flip rows at 0 V during turns | Page background still patchy white/yellowish (eye) | Rejected |
| 15 | `turn_full 1` | Turn as one full refresh, no tail | Very faint trace of the old text (eye) | Not good enough |
| 16 | `cd_std 2` | Flash-free turn, then an in-place clean | Cleans fully, but the page visibly darkens or ghosts first | Rejected twice |
| 17 | 1.8.26 / 1.8.27 | Double pass: full-refresh turn plus a second in-place full refresh about 1.1 s later | Clean, "very good" | The second flash on a page that is already visible bothers the owner; 1.8.27 felt harsher |
| 18 | 1.8.29 | White intermediate: old page, white, new page | Cleans | Owner did not like the feel |
| 19 | (experiments) | Soft pulses, second co-drive, shorter tables as a flash-free second pass | Left the trace | Rejected |
| 20 | **1.8.30** | One full refresh with a white tail in the STD table | Fast, no visible trace to the eye, no second flash | **Chosen** |
| 21 | 1.8.30 + extra `bwturn` | Black-and-white pages turn like colour pages (`tf_chroma 0`) | One full refresh, no visible trace | Owner asked for it; **kept** |
| 22 | extra `ggrefresh` | A refresh button in the document toolbar | One full in-place refresh on a tap | **Kept** |
| 23 | extra `viewchange` | View change with the stock refresh at the change (`defer 0`) | No more "ghost first, then a hard flash" after closing a document, but Settings kept traces | **Kept**; Settings handled by 24 |
| 24 | 1.8.31 / 1.8.32 + extra `settingskb` | One in-place clean at Settings opening and closing and at keyboard opening | Owner: Settings clean, no trace | **Kept** |

## 2. The long history in short paragraphs

### 2.1 Cleaning after the fact (v7.1 to 1.7)

The first idea was to wipe the screen with a flash right after something changed. The first tests showed what the stock firmware already did: at a view change (opening Settings, closing a document) it flashes while changing content. That flash does not erase the old image's trace. It only helps when the same image is refreshed in place.

So the early versions added their own in-place refreshes. They were triggered from the user interface (a QML clock), from guesses about what the screen was doing, and from a model of "residue" that looked only at brightness. A root-cause review in early November showed the problems:

- The trigger looked at the bounding box of an update, not the real area changed.
- The model missed tint changes (blue, lilac), so a box ended exactly where the tint began.
- Boxes were never widened, so partial cleans left visible seams.
- Cleans came at the wrong time, sometimes over an update that was still being drawn.
- The stock flash's own change was counted as new residue, which caused extra, unneeded cleans.

Version 1.7 with clock 11 made this worse: it flashed even on opening a side menu, and traces grew. It was rolled back.

### 2.2 Using xochitl's own events (1.8 to 1.8.4)

From 1.8 the library read triggers from xochitl's own log lines (document close, gesture end, page turn, keyboard). In a test we measured that a clean of less than the whole screen delays and stretches every update that touches it, including that update's pixels outside the clean. This explained a lilac tint around a dialog. From 1.8.4 every automatic clean covers the whole screen.

A band clean for the keyboard (1.8.1) was retired for the same reason.

### 2.3 Page turns: flash on every page, or traces pile up

Page turns were the hard case. In stock firmware a page turn is a partial update with no clean at all.

- With a clean after every page turn (1.8.4), the owner said it was unusable for reading.
- With page cleans off, four page turns in a PDF with tables left old pages over each other in bands.
- A threshold in between (1.8.5, never installed) was too high: three turns back left traces again.
- 1.8.6 cleaned every turn that left a trace, 0.2 s after the page appeared. It was installed but still a flash on many pages.

### 2.4 Co-drive: the flash-free path

The root-cause work (see HOW-IT-WORKS) found that the shared electrode follows the mean drive. Co-drive (1.8.8) writes a small compensating voltage into the cells that hold unchanged pixels, plus a companion update for the strip outside the changed rectangle. On black-and-white pages this removed the flash and the camera measured a small remaining trace. Later versions (1.8.15 to 1.8.17) added a clean with a softer waveform and a guard against a known bad case.

For colour pages (the STD waveform) 1.8.18 added the same idea. It evened out tone over wide areas.

### 2.5 The 48-setting sweep

To tune STD co-drive we tried 48 settings of its shape (gains, level masks, frame windows, carry-over of dose), 10 page turns each, measured by camera. No setting beat the defaults. Almost all of them moved along one single "dose" axis: more drive towards white made the page bluer, more drive towards dark darkened the page's edge frame. The default sat at the knee of that curve. One setting that used only 24 V pulses was the only real second degree of freedom, but it made the next clean look yellower and uneven, so it was rejected. The sweep was not repeated.

### 2.6 Letter-scale ghosts on colour pages

On 10 to 11 November the owner read a colour PDF and saw the sentences of the previous page on every page. Our camera checks had measured the tone of large boxes and had missed this. We recorded the lesson: judge ghosting at letter scale in real reading.

The reason is physical: a pixel that held ink was driven to white, and its neighbours were held. Their optical difference is not removable with a smooth compensating drive. Only a refresh of every pixel in place equalises them.

Version 1.8.22 cleaned after every colour page turn. This cleaned, but it also flashed on scanned black-and-white books, because their dithered greys looked like colour to the sampling. The owner rejected a flash at every page.

### 2.7 The two whites and flip rows (1.8.25)

The owner asked whether something was wrong with the whole approach. There was. The panel has two whites (level 30 and level 28). 21 to 27 percent of the pixels flip between them at every turn on a designed colour PDF, which makes about 75 percent of the mean drive (see HOW-IT-WORKS). Version 1.8.25 held those flip rows at 0 V during page turns. By the camera it was good (4 turns: 1.85, 8 turns: 1.96 against 3.85 to 4.69 before). By the owner's eye, the background of colour documents was still patchy, white next to yellowish. It was rejected.

## 3. The final rounds

These were all judged by the owner's eye on colour pages, with the camera as a second opinion where it could help.

### 3.1 The numbers that led to the double pass

Camera, six page turns per variant, share of the previous page that shows in the new page (positive: old ink shows dark; negative: light; the eye sees it from about 0.003 to 0.004):

| Variant | Mean +- sd |
|---|---|
| 1.8.25 flash-free | -0.0128 +- 0.0057 |
| Single full-refresh turn | +0.0043 +- 0.0031 |
| Single full-refresh turn plus one in-place full clean | -0.0010 +- 0.0008 (least of all) |
| `cd_std 2` | -0.0027 (with the dark picture in between) |

So a full refresh that changes the page leaves a faint dark trace of the old black text, and one more full pass in place removes it. This is what the double pass did.

### 3.2 Each approach in the last rounds

| Approach | What the eye said | Verdict |
|---|---|---|
| Flash-free co-driven turn (1.8.25) | Patchy white/yellowish background remains | Rejected |
| Single full-refresh turn without the tail | Very faint trace of the old text | Not good enough |
| Flash-free turn followed by an in-place clean (`cd_std 2`) | Cleans fully, but the page visibly darkens or ghosts before the clean | Rejected twice |
| Double pass: full-refresh turn plus a second in-place full refresh about 1.1 s later (1.8.26 / 1.8.27) | Clean, "very good". But the second flash on a page that is already visible is bothersome, and 1.8.27 felt harsher | Rejected |
| White intermediate: old page, white, new page (1.8.29) | Cleans, but the owner did not like the feel | Rejected |
| Soft pulses, a second co-drive, short tables as a flash-free second pass | Left the trace | Rejected |
| **One full-refresh turn with the white tail in the STD table (1.8.30)** | Fast (about one refresh), no visible trace, no second flash | **Chosen** |

### 3.3 Notes on each

- **Double pass (1.8.26 / 1.8.27).** The second pass cannot start before the panel has finished the first one. Even with fine clock ticks the gap was about 1.1 s, which is the floor we found. The second flash is on a page the reader already sees. When the owner turned pages fast (about 2.4 s per page), the two flashes ran almost back to back, which the owner called harsher. This is a hypothesis; it was not proved.
- **White intermediate (1.8.29).** Before the page change, every pixel of the frame is replaced by white so the first full refresh goes old page to white, and the page arrives in a second full refresh. This cleans, because the second pass starts from pure white. The new page appears about 1 s later than before. The owner tried it by hand and did not like it. We did not ask why: a blank white screen, a late page, or two hard flashes are all possible reasons.
- **Flash-free second passes.** Several attempts to do the second pass without a flash (soft pulses, an extra co-drive, shorter tables) either left the trace or gained nothing. No single pass reached the clean floor in our camera tests. Longer tails (8 frames did what 15 did), -24 V instead of -12 V, -6 V, extra reset frames and a push to black before the reset made no difference.

## 4. What we chose, and why

The tail is the only change that reached the owner's goal. It does not add a pass. It makes the one full refresh that already happens do the job of the second:

- In the 22 to 28 C STD table, the 43 ink-to-white rows are idle in frames 68 to 71 and 74 to 76. We write -12 V there (129 cells).
- A turn away from a colour page is a normal, single full-refresh turn. The tail pushes the leftover of the old text towards white inside the same refresh.
- In earlier camera tests the tail, written by hand into a co-driven turn, removed part of the trace but not all of it. For the full-refresh turn the camera already read the plain single refresh as clean, so it could not judge the tail. The owner's eye could, and judged the result good.

Black-and-white and sparse-colour pages stay on the flash-free co-drive path in the engine itself; the `bwturn` start-up extra, which the owner asked for and kept, turns them like colour pages instead (row 21). Closing a document keeps xochitl's own refresh at the change (`defer 0`, row 23). Settings opening and closing, and the keyboard opening, get one in-place clean about half a second later (row 24), which the owner found clean. The document toolbar has a refresh button (row 22). Menus, popups, typing and writing get no added flash.

## 5. What was not tested, and what may go wrong

- Only one device, one person's eye.
- The tail applies only to the 22 to 28 C table. At 28 C and above colour turns have a plain single full refresh with the faint trace.
- The change is in RAM and re-applied after each start.
- The long-term effect of the extra -12 V frames (about -84 V x frames per ink-to-white transition) was not measured.
- A few ideas were never tried: a third pass, a softer second waveform (FAST) for the second pass, and testing at 28 C or above.

See [HOW-IT-WORKS.md](HOW-IT-WORKS.md) for the mechanism.
