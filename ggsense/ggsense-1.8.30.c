// ggsense.c - GhostGuard pixel-residue sensor for the reMarkable Paper Pro (xochitl 3.29.1)
//
// Loaded into xochitl with LD_PRELOAD (after xovi.so). It interposes one public Qt symbol only:
//   * qInstallMessageHandler()  -> keeps our handler in front of xochitl's, so we see the
//                                  rm.framebuffer.updates debug lines (update region + update class)
//                                  synchronously, and a tiny command channel from QML
//                                  (console.log("GGQ <id> <cmd> ...")).
// The pixels are read from the frame image of the EPFramebuffer object itself (s_instance + 0x98, the
// 1620x2160 RGB32 image with a 6528-byte stride), after checking the xochitl build-id. 1.3 instead took
// the most recent 1620x2160 RGB32 QImage built on external memory; opening a PDF builds such an image
// for the rendered page and frees it a moment later, so 1.3 read freed memory and crashed xochitl.
// In any process other than /usr/bin/xochitl (child processes inherit LD_PRELOAD, e.g. the PDF
// renderer) the library stays completely passive.
//
// Model (per 2x2-pixel sample): every update is compared with our copy of what the panel shows. A
// sample that changes in a partial update accumulates "residue" = |luma change|, weighted by the update
// class (its waveform family) and by direction (dark->light counts fully, light->dark by beta). A
// complete (QFlags 0x1, STD or sleep path) refresh clears the residue only of the samples whose value
// it did NOT change: those are driven through the same-value sequence of the waveform, which erases
// ghosts (seen on the panel: an in-place clean removes them). Samples whose value changes inside the
// complete refresh go directly from the old to the new value and keep a trace of the old image (seen on
// the panel: the stock Settings open/close refresh leaves the previous screen visible); they accumulate
// residue with weight w_std. A sample is a ghost sample when its residue is >= tau * 255 and it is
// currently shown light (ghosts are visible on light backgrounds). Cells (16x16 px) whose content
// changed in the last quiet_s seconds are ignored (animations, scrolling in progress). The decision
// ("rec": none/soon/now) is made from the number of ghost samples and of hot cells.
//
// Optional regional clean ("GGQ <id> rclean x y w h", only when QML asks for it):
// EPFramebuffer::swapBuffers() widens every QFlags(0x1) update to the full screen (xochitl 3.29.1
// @0x85e380), but the Acep2 implementation underneath (vtable slot 23, @0x86a9f0) sends exactly the
// rectangles it is given with the STD waveform. The clean calls that implementation the same way
// the stock BlinkNow path does (framebuffer mutex held, current screen-mode map, flags 0x1), but
// with a sub-rectangle, and on content that is already shown (in place), which is what erases ghosts.
// It runs only after checking the xochitl build-id, the function bytes, the object's vtable, that the
// object's image is the frame we read, and that we are on its thread. Any mismatch disables it and the
// caller falls back to stock full-screen cleans.
//
// 1.5, one in-place flash per view change ("defer", off until QML turns it on, see deferred_clean): the
// stock flash that xochitl puts on the first frame of a new view (Settings, dialogs) is withdrawn, and
// the changed area gets one STD in place once the screen has settled. 1.5.1 adds the rules from the
// review (see the list above defer_live): wait for the newest view's first frame, never clean while the
// user writes, let a stock in-place flash stand in for the clean, never take another stock flash, and end
// cycles that drew nothing.
//
// 1.6, need-based clean in documents ("GGQ <id> auto <why>", see auto_try): the stock never cleans while
// reading (a PDF page turn is one partial "content" update), so the traces of every earlier page pile up. The
// QML clock asks once a large document update has settled; the residue model decides and the hot area gets one
// STD in place. Also the review fixes: every view change starts its own ink state, a view being written on keeps
// its clean for up to TR_INK_MAX_AGE, with the pen near the clean also waits TR_INK_QUIET after the last update,
// and turning defer off gives a withdrawn flash back while the newest view has not drawn. Parameters from text
// are clamped before any conversion.
//
// 1.8, the cleans follow xochitl's own events instead of a QML guess (root-cause report 5.8; measured on the
// tablet 2026-11-09): every clean of ours is ONE in-place STD of the WHOLE screen (no partial boxes: their edges
// stayed visible), never within stock_gap s of a stock full refresh, never while the screen, the ScreenDriver or
// the pen is busy, never for menus or typing. The triggers, each off until the QML clock turns it on:
//  - sd_close: the document closes (ScreenDriver "visibleChanged: false" -> " - clearing on next update" sets the
//    BlinkLater byte right after its log line, @0x2b1ebc): that stock flash would change the content (document ->
//    library) and leave the document behind; it is withdrawn like a view change's, the library is drawn by its
//    normal update, and once it settled the screen gets one STD in place. The sleep screen hides the document view
//    the same way: when rm.batterymanager logs "Changing display state from Normal to <sleep>" (36 ms before the
//    sleep frame, measured) a withdrawn flash is given back to that frame and the cycle ends without a clean;
//  - gesture_clean: after a document gesture (swipe page turn, zoom, scroll: mono/A2 frames) the ScreenDriver
//    measures how much the image changed and skips its own clean below dirty 0.1 (" - doing nothing..."), so
//    its mono -> full redraw goes out as a normal update and the old page and the A2 frames' grey and colour
//    errors stay. With dirty >= gesture_min the BlinkLater byte is set at that line, exactly what its own
//    " - full update.." branch does (@0x2b12e0): the redraw that follows (same page, now in full greys) goes
//    out as ONE full STD in place, at the stock time, with no extra update. When it cleans itself nothing is
//    added. Only after a gesture (a lasso or selection mono state is never promoted), never with the pen near;
//    the byte is taken back when a new gesture, the pen or a document close comes before the redraw;
//  - page_clean: a page turn in a document is one partial content update of the page (no flash, no clean in
//    stock), so the traces of the earlier pages pile up; the frame right after the ScreenDriver's "finalizeNewPage:"
//    (within 0.6 s) whose content covers >= page_min of the screen, outside a gesture and not its closing redraw,
//    asks the residue model, and "soon"/"now" gives one clean when quiet;
//  - kb_clean: the on-screen keyboard (a wide A2 or Overlay area at the bottom) is covered by the next view's
//    normal update when it closes, which leaves its black area and the dialog behind: one clean when quiet.
// A request never runs with the pen near or while the on-screen keyboard is up: it waits (and ages out after
// AUTO_PEN_MAX_AGE if the pen stays). Ink drawn after the request (the user writes) drops it, and so does our own
// full clean or a stock full refresh after it. A keyboard shown again before its clean drops a keyboard request.
// A page frame of a document being opened is left to xochitl's own in-place STD of the open.
//
// 1.8.1, kb_open_clean (off until the QML clock turns it on, see kbo_try): the on-screen keyboard is drawn in ONE
// frame with a fast waveform (A2, or the PEN waveform of Overlay mode), which changes the content of its band
// without driving out what was there: the "+" menu stayed visible inside the keyboard of the New notebook dialog
// (user's photo IMG_1752, 2026-11-09 08:54:56 UTC; same frame shape in every keyboard opening of the journals of
// 8-9 Nov, 5 of 5, and no other frame of ~73,000 had that shape). Right after that frame, before the first key
// press, the keyboard band alone gets ONE STD in place. Its edges are the keyboard's own border and the screen's
// edges, so no seam can show (the seams of 1.6/1.7 came from boxes cutting through the picture). Any other update
// after the keyboard frame (a key press, a popup, ink, the keyboard redrawn, a complete refresh) drops it: only
// the text cursor (a minor ui update outside the band) may come first. It never runs while the display sleeps or
// a gesture or a promoted redraw is due, and it is dropped KBO_MAX_AGE s after the keyboard frame.
//
// 1.8.2 (review of 1.8.1, confirmed on the journals): the band's STD lasts ~1 s, so a clean that starts late runs
// into the first key presses (fastest real first key: +1.18 s after the keyboard frame; the clean came at +0.69 s
// with the pen away, but at +1.95 s with the pen near, because the QML clock holds its first ticks back while the
// pen is near). Now the clean is decided once, right after the keyboard frame: a tick with the pen near drops it
// (no flash under the pen), KBO_MAX_AGE is 1 s (no late start from a heartbeat or a pen-leave tick), and it never
// runs within stock_gap s of a stock full refresh (the 1.8 rule; a second flash right after a stock one).
//
// 1.8.3 (user's photo IMG_1753, 2026-11-09: with 1.8.2 the New notebook dialog showed old traces and a lilac tint
// OUTSIDE the keyboard band). Measured on the tablet with the SWTCON per-pixel state and its busy state (tools/
// stsample.c, tools/swidle.c): the dialog's own update keeps the panel busy for 1.25 s (Overlay keyboard; 2.5 s with a
// UI keyboard); the band clean started at +0.39 s while that update was still being driven, stretched the busy time
// to 1.78 s (the dialog's phase steps: 1.14 -> 1.56 s, tools/stphase.py) and spoiled the pixels of that update outside
// the band. The box clean of IMG_1743 (0.59 s after a
// full-screen ui update) was the same mistake. Rule K1 of the root-cause report ("never clean while an update is
// still being driven") is now mechanical: a clean of less than the whole screen is sent only while xochitl's SWTCON
// is idle, read exactly as the stock EPFramebuffer::sync() reads it (see swtcon_idle); otherwise it waits
// ("inflight") for a later tick. A view change whose regional clean found the panel busy for TR_IDLE_MAX s gets the
// whole screen instead (a full in-place STD covers every pixel an in-flight update was driving). kb_open_clean is
// retired (the option is refused): the dialog's update ends >= 1.25 s after the keyboard frame, the first key
// presses come from +1.18 s on, and a ~1 s STD of the band would run into them. The keyboard's trace is erased by
// the full clean when it closes (kb_clean), as before 1.8.1. Full-screen cleans keep their 1.8 timing; the SWTCON
// state and the panel temperature are logged with every clean and shown in the status file.
//
// 1.8.4 (measured on the tablet 2026-11-09, tools/test_collide.sh, testruns/collide_1109_1531): the other order is not
// safe either. Our STD of the keyboard band went out with the panel idle and a key was pressed 0.32 s later. The key's
// update (one swapBuffers) had parts inside the band (the keyboard overlay, two ui strips) and outside it (the text
// field: an animation part and a 1 px ui column). The parts inside the band and the ui column outside it waited until
// the band was released (+0.92-0.96 s after our clean) and then went straight to their final levels: the key's press
// highlight was never drawn. The animation part outside the band started at once but reached its end steps ~0.5 s
// late (1.29-1.47 s after its start instead of 0.78-0.99 s with the key alone; E1 and E1b agree, E3 is the key alone),
// on the same transition as the released parts. So a clean of less than the whole screen delays or stretches every
// update that has a part inside it, that update's pixels outside it included, whichever came first; the idle gate of
// 1.8.3 only covers the updates already in flight. Not measured: whether an update entirely outside the clean is
// stretched, and the optical effect of E1. With a whole-screen STD every pixel of every update is inside it: one in
// flight is covered (IMG_1743: the in-flight pixels inside our box ended clean) and one that comes during it should
// wait like the band's parts did in E1. That is an inference: cases F0-F2 and K0 of tools/test_collide.sh measure it
// (results in GhostGuard-raporu.md, section 5.10). Replayed through 1.8.3 and 1.8.4, the 14 real journals of 8-9 Nov
// give 53 view-change cleans, all of them whole-screen; the page and keyboard cleans always were.
// Hence every automatic clean covers the whole screen: tr_full is 0 (a view change's clean used to be regional below
// 35 % of the screen) and "set tr_full" above 0 is refused. "GGQ <id> rclean" with a smaller rectangle stays for
// tests and diagnostics (gated as in 1.8.3).
// Also (review of 1.8.3, CF-3): a clean refused because the display sleeps no longer logs the panel state of an
// earlier clean.
//
// 1.8.5 (the user, 2026-11-09 17:15 UTC, after reading a book with 1.8.4: "when I turn a page while reading a book a full
// flash fires, and that is very impractical"; journals 16:51:40-16:51:56 and 17:15:00-17:15:10: seven and five page turns,
// each followed by our whole-screen clean 0.25-0.75 s later). Reading comes first, and so does a readable page: with
// the page cleans off (17:17 UTC) four page turns of a PDF with tables, from a clean open (17:36:31-17:36:44), left
// the old pages over each other in bands across the whole page (IMG_1760, 17:36:57; "after the page-turn flash was switched
// off the screen looked like this a few pages later"). The model held ghost=191977 samples for that screen; one
// page turn after a clean leaves 18837-62412 (the eight page cleans of 17:11-17:15). So the clean comes neither on
// every page nor on a fixed count, but when the residue of the pages turned since the last full refresh (ours or
// stock's) reaches page_px samples (default 80000: about every second dense page, every third or fourth light one):
// - a page change (a page turn, or the redraw that ends a document gesture) is counted; the first page_gap - 1 changes
//   after a full refresh never clean (default 2: never two flashes on consecutive pages);
// - from then on the model decides when the request is tried (ghost >= page_px and at least min_cells hot cells);
// - the page_every-th change since the last full refresh cleans whatever the model says (default 6; 0 = no cap): the
//   model sees only luma, so a tint it misses is still washed out at that rate;
// - a page turn sooner than page_flip s (2.0) after the previous change is leafing: a clean that falls due meanwhile
//   waits until the turning stopped (page_settle s, 1.5, without any update), so it lands on the page the user
//   stopped at, never between two turns;
// - a gesture's skipped clean is promoted to the full STD under the same rule (count, model, cap), so a slow finger
//   turn or a pan no longer flashes every time.
// page_every 1 cleans after every page change (as 1.8.4 promoted every gesture redraw; its page turns asked the model,
// rec soon/now: page_gap 1, page_every 0, page_px 6000). The flash itself is the same whole-screen STD in place as
// before; only when it comes changed. The count and the outcome are logged with each page change and shown
// in the status file ("page":[count, gap, cap, px, not due, after leafing, model said no]).
//
// 1.8.6, every page turn that leaves a trace is cleaned, at once (2026-11-09 18:38 UTC). With page_px 80000, three
// page turns back (18:38:15-18:38:21, ghost 10740, 47297, 77426: no clean) left the old pages over each other again
// (IMG_1761; "it did not work ... not good at all"). On this panel one page turn already leaves a visible
// imprint, and only a clean in place removes it. So a page turn gets its own threshold, turn_px (default 6000: any
// turn that leaves a trace, as 1.8.4), and its clean goes out page_quiet s (0.2) after the page's frame, not
// auto_quiet (0.4), with the model counting the page's own cells (window page_quiet): the flash starts while the
// panel is still drawing the page (~0.7 s), not after the user started reading it. Leafing still waits page_settle.
// The redraw that ends a document gesture keeps page_px (it rarely cleans); page_gap and page_every count both.
// Defaults: page_gap 1, turn_px 6000, page_quiet 0.2; status "page": [..., turn_px, page_quiet in ms].
//
// 1.8.7 - 1.8.17 (the co-drive of page turns, the clean with TEXT targets, ...) are described where their code is:
// see "1.8.8 co-drive" and the notes from there on.
//
// 1.8.18, the co-drive of STD page turns (2026-11-10; the user: "my PDF books still ghost very heavily
// and only a hard flash clears it", colour PDF A). A page with colour is turned as an STD, which
// holds every pixel that keeps its level at 0 V just like the GLR: measured 10.5 % darker held white after one
// turn. 1.8.17 answered each such turn with the full-screen clean. Now the hold cells of the STD table get the
// co-drive of that turn's own mean drive (from the levels sampled before and after it), with the strip's companion
// as an STD: measured 0.1 %. See "1.8.18: the co-drive of an STD page turn". Keys cd_std (1; 0 = as 1.8.17),
// cds_gain (1.07), cds_gpos (0.43; 0.25 from 1.8.20), cds_lead (4), cds_every (0); status "cd" has a 16th number (co-driven STD turns).
//
// 1.8.19, the turns 1.8.18 still answered with the clean (2026-11-10, journals of the reading-pace runs on colour PDF A:
// 2 of 5 turns forward and 2 of 7 back flashed):
//  - xochitl draws a page's pending tiles as one or two further frames 0.2-0.5 s after the turn's own, inside the
//    0.6 s that make a frame a "page turn". Each got a turn id of its own; its update drew nothing large (found
//    nothing: marked) or replaced the request before it (marked). They are the same turn now (sd_newpage_n): the
//    turn's id, no GLR pre-arm, and a later frame that draws nothing or next to nothing is not a failure;
//  - the levels are sampled for every turn's frame, not only away from a page with colour: a turn to a page with
//    colour, or away from blacks an STD left at level 2, is an STD too and is co-driven like the others (where
//    nothing but the turn's own updates is on the screen, or the companion went out as an STD);
//  - an update in its last frames on the STD table no longer stops the co-drive of the one that follows it (it
//    reads only frames that never get a code);
//  - an STD frame whose levels and whose own change both say "next to nothing" needs none: confirmed, no clean;
//  - the helper logs the ways a turn's request ends that were silent (nothing found, given up, GLR table shared).
//
// 1.8.22, colour pages (2026-11-11). The user read a colour PDF with 1.8.21 and saw the sentences of the page before
// on every page: the STD co-drive evens out the tint of wide areas, not the letters (a pixel that held ink was
// driven to white, its neighbours were held; only a full refresh IN PLACE makes them equal, see finalize_pending).
//  - cd_std is 2 by default (1 since 1.8.18): an STD page turn away from or to a page with colour is not co-driven
//    and gets the page clean right after its frame, as in 1.8.17 (nothing is written or queued for a turn away from
//    such a page: no cells, no companions, no levels). Black and white turns keep what 1.8.21 did: the GLR its
//    co-drive, and the STD between two pages without colour (after a clean that left level 2, a warm panel) the STD
//    co-drive. 1 = every STD turn is co-driven (1.8.21); 0 = none is (1.8.17: a black and white STD chain is cleaned
//    again, but for the one right after a full refresh).
//  - turn_px_col (400 samples): the model's threshold for the clean of such a turn (cd_std 2, colour before or after
//    it), in place of turn_px (6000). On the tablet (11 Nov 01:28 UTC) the turn from a sparse colour page (a few table
//    rows and a paragraph) to a dense one counted ghost=5724 and got no clean: several lines of the old page stayed
//    under the new one. The journals' turns that changed next to nothing (a page number: ghost <= 96, hot <= 7) stay
//    below it and below min_cells. Black and white turns, cd_std 0 / 1 and a value >= turn_px keep turn_px.
//    While the user leafs, the one page request that waits belongs to the page the turning stops on (1.8.14): it
//    keeps the mark of a colour turn it was armed for, and takes the mark of one that comes while it waits (that
//    turn's residue is still on the screen, whatever the pages after it are).
//    No clean is armed for a turn once a menu or popup opened or ink was drawn after it (review round 4; in the
//    code since 1.8.14, routine with cd_std 2: the failure of a newer turn waited behind the page request, a menu or
//    a pen stroke dropped that request, and the next tick armed a second one: a flash over the open menu, or after
//    the stroke. The same when the helper's answer came only after the menu). Review round 5: the same again when
//    that request aged out (the pen hovering: the second request's clean came up to 16 s after the turn) or a
//    gesture began: a page request that is given up stood for every turn so far (page_given_up).
//  - turn_full (0 until 1.8.25; 2 since 1.8.26): 1 = at the ScreenDriver's "finalizeNewPage:" (~50 ms before the
//    turn's frame; the frame's own "swapBuffers:" line is logged after the byte was read, too late) the BlinkLater
//    byte is set when the page on the screen holds colour, so the turn's frame itself goes out as the full STD: one
//    flash, no clean after it. That refresh CHANGES the content, so by every earlier measurement it leaves a trace
//    of the old page (the stock flash at a view change did); the key is there for the eye to judge how much. Not
//    with the pen near, in a gesture or mono state, with the keyboard up, or while a view change or a request of
//    ours waits; the take-backs of the gesture end's promoted byte hold for it too. The promoted frame is handed
//    to nobody (no cells, no companions, no id); a turn to or from a page the samples call colourless falls under
//    the rules above. Review: not at a document's open (its "finalizeNewPage:" comes 16-48 ms after
//    "visibleChanged: true", with the library still on the screen; xochitl's own in-place STD follows); the frame
//    that takes the byte is the turn's only if its changed samples span a page-sized box (a small frame that drew
//    first leaves the turn to the usual way); the later frames of such a turn never arm the page clean; a byte no
//    frame took within 1 s is taken back by a helper thread too (a quiet screen has no clock tick for up to 20 s).
//
// 1.8.21, keys for the SHAPE of an STD turn's co-drive (2026-11-10; all defaults give 1.8.20's cells code for code).
// With the two gains alone no setting leaves both the margins (held white, 100 % level 30) and the page's own white
// neutral: cds_gain 0.80 makes the page neutral and the margins 3-8 % darker, 1.07 leaves a blue cast (B +3..+8 %,
// R -1..-2 %); cd_max 6 and cds_lead 0 change nothing. To find what does:
//  - cds_k27, cds_k29 (1, 1): the drive of the hold cells of column 27 and of column 29, as a factor (0: no code);
//  - cds_k28 (1): a further factor for the rows from levels 28 and 29;
//  - cds_lv (0): the levels the codes may use, bit 0 = 6 V, bit 1 = 12 V, bit 2 = 24 V (0: as cd_max says);
//  - cds_carry (1): 0 = the dose of the frames the generator has passed already is dropped, not driven after them;
//  - cds_w0, cds_w1, cds_wk, cds_wadd (0, 0, 1, 0): in the frames w0 <= f < w1 the drive is c * wk + wadd (V).
//
// 1.8.20, cds_gpos 0.43 -> 0.25 (2026-11-10; the user: "the ghost looks darker in the photos than
// we expected"). 0.43 was fitted on the whole screen's mean after ONE turn. White that is held on every turn (the
// page margins, 100 % level 30) kept a little of each turn: R/G -2 % after 2 turns, -5..-7 % after 20 (a dark lilac
// frame round the content, whose white is redrawn every few turns and stayed at -1 %). By region after 20 turns
// (5 back + 5 forward, twice, 26 C; top / right / left margin, R G B per mille against a fresh clean):
//   0.43: -50 -43 +5 | -68 -52 +7 | -43 -28 +23      0.33: -37 -24 +12 | -31 -12 +54 | -21 +6 +69
//   0.25: -28 -16 +27 | -22 -3 +66 | -11 +8 +72      0.10: -34 -17 +35 | -11 +5 +78 | -4 +13 +82
// The held white answers to the co-drive's net dose (the table's own mean drive sums to ~0): with 0.25 its
// luminance stays within -1.5..+1 % of the clean's, as the content's (+0.3 %); what is left is a blue cast of the
// whole page (B +3..+7 %), not a frame. cds_gain stays 1.07.
//
// 1.8.23, no clean per page, and a white tail (2026-11-11). The user read with 1.8.22: a flash after every page turn,
// on a scanned black and white PDF too (its anti-aliasing greys are "colour" to the samples), "it spoils the experience,
// I do not want this".
//  - cd_std is 1 again by default: every STD turn is co-driven, none gets the page clean (2 is still there as a key).
//  - cds_wt (7 frames; 0 = off): the letters a co-driven STD turn leaves (1.8.22's reason) are pixels the table drove
//    ink -> white next to white that was held. In the STD table (22-28 C, 86 frames) such a row is of one of two
//    kinds. Long (40 rows: from the colours and the black of a colour page, 2 4 8 12 16 20 24 ..., to 28 / 30):
//    ... -24 x 13, -12 x 5, -6, -12, -6, four idle frames (68-71), -12, -6, twelve idle frames (74-85). Short (15,
//    19, 21 -> 30, the greys at the edge of a letter): done by frame 29, idle to 76 / 78, a few -12 / -6 after that.
//    For the length of a co-driven STD turn the rows from a level below 27 to 28 / 30 that drive towards white or
//    idle from frame 47 on, drive at least once there and idle in 68-71 and 74-76 get -12 V in the first cds_wt of
//    those seven frames below len - CDS_TAIL, written when the turn is armed (they are 60 frames ahead of the
//    generator then) and put back with the hold cells; the co-drive is worked out from the rows as they are driven,
//    tail included. Black -> white (0 -> 30, idle from frame 30 on) is left as it is: black text came out clean.
//    Measured 11 Nov at 25.9 C (camera, colour PDF B 53 -> 54, patches written by hand, two or three runs each; sd of
//    the letter-scale high-pass on white, per mille, floor 1.64; darkest 1 %): stock 4.4-4.7 / -17.5; the long rows
//    alone 4.2-4.4 / -16.4 (but the tint of the ex-colour areas -40..-50 %); the three short rows alone 3.7 / -13;
//    both 3.4-3.8 / -11.5. A scanned black and white page (dithered 2 4 8 ... 24 on 28 / 30) is at the floor with
//    and without the tail (1.5-1.7): no overshoot. A part of the trace, not all of it: no single pass reaches the
//    clean's floor (8 frames did what 15 did; -24 V no more; -6 V, two more reset frames, a push to black before the
//    reset: nothing). Only the measured table gets the tail: the rule must take exactly its 43 rows, the short ones
//    and 0 -> 28 among them. The STD tables of 28-50 C have 86 frames too, and the rule alone would take 2, 3, 41
//    and 0 of their rows (second review); they and every other table are co-driven as before, without a tail. So is
//    a turn that needs no co-drive (the tail never goes without it).
//    The turn counts as co-driven once every hold code was read, as in 1.8.22: when the wait for the tail's last
//    frame ends early (the node gone, the list unreadable for three walks, the time limit, co-drive switched off)
//    only the tail was cut short and there is no clean (second review: the wait to frame 77 instead of the last
//    hold code + 1 would have given a spurious page clean after a helper stall of 60-120 ms at the turn's end).
//
// 1.8.25 (2026-11-11): the two whites. The user: "arka plan izi ... hala cozulmemis. acaba yontemimizde mi sorun var
// gozumuzden kacan bir nokta var bence". There was. A designed colour PDF draws plain page white as level 30 and the
// white inside its pale-shaded rows and boxes (dithered: white plus single colour dots) as level 28. The STD table
// treats the two as one state (its rows from 28 and from 30 are the same for every target but 31), yet a pixel that
// goes 28 -> 30 or 30 -> 28 "changes": it takes the white refresh row (+24 V x 26, then the reset; the row of
// 28 -> 28 and 30 -> 30) although it is white before and after. Measured 11 Nov on colour PDF B 54..58 (state
// buffers of both pages): 21-27 % of all pixels flip between the two whites at every turn and make about 75 % of the
// turn's mean drive (peak -10.3 V and sum 446 V x frames with them, -4.0 V and 114 without). That mean drive is what
// tints the held white and what the co-drive has to fight. Camera, the flip rows written to 0 V by hand
// (tools/tabpoke), sd of the letter-scale high-pass on white, per mille, clean floor 1.5:
//   one turn 57 -> 58, co-drive on: stock 3.65 / 3.80 (the old page's lines show), flip rows 0 V 1.48 / 1.60;
//   four turns at 2.5 s: stock co-drive 3.85 / 4.69; flip rows 0 V with the co-drive in the hold cells only 15-22
//   (lilac bands where the pale rows were: those pixels now took the swing alone, the held ones had their co-drive);
//   flip rows 0 V and no co-drive at all 2.46, the toolbar column white.
// So the flip must be treated exactly as a held pixel: 0 V from the table and the co-drive's codes with the hold cells.
//  - cds_flip (1; 0 = the stock rows): the rows 28 -> 30 and 30 -> 28 of the STD table are at 0 V only while pages
//    are being turned. First design: written once and left so. Refuted the same day, before it was installed: a
//    complete refresh reads the same rows, and with them at 0 V a full STD (turn_full 1, 57 -> 58) left the pale rows
//    deep blue: hp 67.9 against 1.6 with the stock rows (the flips then sit the refresh out next to pixels that take
//    +-24 V). So: cdf_hold writes them at the swapBuffers line of a turn expected as an STD (GUI thread, before
//    xochitl queues the update; not when anything waits to be accepted or still reads a driven frame of the rows),
//    turns that follow keep them, and they go back to stock 6 s after the last turn (cdf_tick; the page number hides
//    3.5-4.4 s after a turn with an update of the same table), at 9 s whatever runs, and at once, before the update
//    is queued, at every swapBuffers line with the complete-refresh flag, at both of our cleans, at a test update
//    and at sleep (cdf_line, cdf_full). Rows going back under a running update only whiten its flips more (the row
//    drives towards dark first); rows going to 0 V under one would leave them dark, hence the checks in cdf_hold.
//    Only a table that shows the two whites are one state: both flip rows equal to the refresh rows 28 -> 28 and
//    30 -> 30, those two equal, the rows from 28 and from 30 equal for every target 0..27 and 29, the hold cells
//    free (cdf_kind). A cell that cannot be put back switches cds_flip off.
//  - a co-driven STD turn that finds the flip rows at 0 V writes its codes into them as well (the hold groups from 28
//    and from 30, column 27; cdf_put) and takes them out with the hold cells. Its mean drive is the one without the
//    flips; when that is below cd_minv the turn "needs no co-drive" (it is not a turn that failed: no page clean).
//  - only the measured table: the STD table of 22-28 C (the white tail's signature, cds_wt_mark: 86 frames, its 43
//    ink -> white rows). The stock STD tables of all nine temperature ranges pass cdf_kind (review, from the stock
//    waveform file), but none of the others was measured, and from 28 C on the kernel's TMODE pass (-72 V x frames on
//    pixels that "changed" to white, 1.5 s after an STD update) would hit the flips and not the held white next to
//    them. There the rows stay stock.
// What it costs: a pixel that flips between the two whites is not refreshed by a page turn (it is held, like the
// 43-56 % of the page that keeps its level); every complete refresh refreshes it as before.
//
// 1.8.26, the double pass on pages with colour (2026-11-12). By the user's eye the flash-free turns of 1.8.25 still
// left the background of a colour document patchy (white next to yellowish). cd_std 2 (the turn's frame, then the
// page clean queued over it) "cleaned completely", but showed the page dark, with the old page's trace, before it
// came clean: rejected. turn_full 1 (the turn's frame itself as the full STD) had left "very little" trace, and the
// user's proposal was to do that clean twice. Camera, colour PDF B 50..56, six turns per variant, the share of the
// page before that shows in the new page (letter-scale fit; positive: the old ink shows dark, negative: light; the
// eye sees it from about 0.003-0.004), mean +- sd:
//   1.8.25, flash-free                        -0.0128 +- 0.0057
//   turn_full 1                               +0.0043 +- 0.0031
//   turn_full 1 and one full clean in place   -0.0010 +- 0.0008 (the least of everything tried)
//   cd_std 2                                  -0.0027 (with the dark picture in between)
// Soft pulses before or after the turn, a second co-drive and shorter tables failed or gained nothing. So: a full
// STD that CHANGES the page leaves a faint dark trace of the old black text, and one more full pass IN PLACE takes it
// away (what finalize_pending found for a view change).
//  - turn_full is 2 by default: as 1, and when the turn's frame went out as the full STD its second pass is armed
//    (arm_second_pass): one clean of the whole screen in place (do_clean, as every clean of ours), without the
//    model's gate. It goes out at the first clock tick at which page_quiet has passed since the last update and the
//    SWTCON is idle: 1.1-1.5 s after the turn's frame, two flashes and about 2.5 s per turn in all. While the user
//    leafs (the turn before this one was less than page_flip ago) it waits for page_flip without an update, 3 s at
//    most: quick turns get one flash each and one second pass after the last. A turn that is promoted while a second
//    pass waits takes that pass over.
//    The pass is dropped, never held back, when the pen comes near or ink is drawn (the user writes: no flash
//    then), a menu or popup frame is drawn, a stock full refresh other than the turn's own went over the screen, the
//    view closes, the display sleeps, the next turn gets no full STD (its page is of the flash-free kind, see
//    tf_chroma), or 8 s passed. A helper that answers "busy" or "pending" leaves it waiting, as a second pass.
//  - tf_idle (1): the second pass waits until the SWTCON has finished the turn's frame; without a reading of it, for
//    1.4 s after the last update. 0 = it is queued right behind the frame (the look of cd_std 2: the pixels of a
//    clean that is queued over a running update start at different times).
//  - tf_chroma (25 %) and tf_chroma_min (30 of 10000 samples): which page gets the full STD turn. The user's verdict
//    on 1.8.22 with a scanned black-and-white book was that a flash at every turn spoils reading it. Such a scan's
//    greys are dithered from all the primaries in balance; a coloured picture, a heading or a highlight is mostly
//    one primary of a triple (cd_state_chroma: the part that is not balanced, per cell of an 8 x 4 grid). Over the
//    109 state buffers on record: scanned pages 6.3-6.9 % of their colour samples, the colour pages of a text book
//    66.7-83.8 %. A turn is promoted only when that part is at least tf_chroma % of the colour samples and
//    tf_chroma_min / 10000 of all the samples read; every other turn is 1.8.25's, flash-free. tf_chroma 0 = any
//    colour sample will do (1.8.22).
//    The buffer at "finalizeNewPage:" holds the page being LEFT: the turn from a black-and-white page onto a colour
//    page is flash-free, the one away from a colour page gets the double pass.
// 1.8.25's flip rows are back at stock before either pass is queued (cdf_line at the frame's complete-refresh flag,
// cdf_full at our clean). The rules of turn_full 1 hold: not with the pen near, in a gesture or mono state, with the
// keyboard up, at a document's open, or while a view change or another request of ours waits.
// What it costs: two flashes and about 2.5 s at every turn away from a page with colour; the pen near right after
// such a turn drops the second pass (the faint trace of one full STD then stays until the next turn); a notebook
// page with enough coloured ink is a colour page, and a colour scan of yellowed paper may be one. Measured at 26 C
// only: nothing here was tried at 28 C or more.
//
// 1.8.30, the white tail for good (2026-11-12). The user's eye on the white intermediate: it cleans, but not that way.
// Their wish: fast, no trace, no hard flash. Tried by hand on 1.8.29 and taken by their eye ("great"): the turn
// away from a page with colour as ONE full STD (turn_full 1, the default now), with the white tail of 1.8.23 (-12 V in
// the frames 68-71 and 74-76 of the 43 ink -> white rows) standing in the STD table itself, so that the full refresh
// of the turn takes the old page's black text out as it goes: no second pass, no white screen. wt_static 1 (the
// default): whenever an STD update is seen (the helper's STD node, or the node of a promoted turn, which wts_kick's
// short-lived thread looks for) the table object is checked against the signature of the table the tail was measured
// on (the STD of 22-28 C: 43 rows, the short ones among them) and the tail is written into it, once per table; it is
// never taken out again while wt_static is 1 (cd_restore_chk does not know these cells). The per-turn tail of a
// co-driven STD turn (cds_wt) is not written then: it stands there already. cdf_meas takes the table with the tail as
// the measured one, so the flip rows are still held on co-driven turns. wt_static 0 takes the tail out at the next
// STD update seen. What is not known: the STD tables of 28 C and more are other tables and get no tail (a colour turn
// is then the plain single full STD, with its faint trace); every STD update (menus too) drives the tail's 7 frames
// in ink -> white pixels, and what that does over months was not measured.
//
// 1.8.29, the white intermediate (2026-11-12). The user's eye on the double pass: the second flash, on the page
// that is already there, is the one that disturbs; no flash-free pass cleans on this panel. Their own proposal of
// 11 Nov: clear every pixel while the page changes. turn_full 3 (the default now; 2 = 1.8.26's double pass): the turn
// is promoted as before, and at its frame's "swapBuffers:" line (on the GUI thread, inside
// EPFramebuffer::swapBuffers(), the frame's lock held, before xochitl converts the frame) the frame image is covered
// with one value that converts like plain white (WH_SENT; the true pixels wait in wh_shadow, and the model reads
// them through luma_at): the full STD takes the whole screen to white. The page goes out as our own full-screen
// clean (do_rclean un-hides the frame first) at the first clock tick at which the SWTCON is idle (wh_tick; about
// 1.15 s after the turn with clock 14f): old page -> white -> new page, two complete refreshes, none of them on a
// page that is already shown.
//  - wh_active: the frame image is covered. wh_owed: the screen has not shown the page yet; cleared only by a
//    complete refresh of the whole screen (ours: wh_clean_sent; xochitl's own: wh_swap_hook). wh_active => wh_owed.
//  - Frames that come while the page is hidden are absorbed (wh_cover moves their pixels to the shadow), so leafing
//    stays white until the first idle tick, which shows the newest page; no turn is promoted while a page is owed.
//    A complete refresh of xochitl's own, the display asleep or a Sleep part in the map un-hide at once. Every clean
//    of ours while the page is owed is widened to the whole screen and is the page's arrival (wh_clean_gate); it
//    waits ("inflight") while the SWTCON drives, WH_HOLD at most.
//  - wh_hide refuses, and the turn gets 1.8.26's double pass, when the clock is not alive (defer_live), the SWTCON
//    does not read the frame image itself (wh_frame_ok), there is no memory, or a check failed before (wh_off: the
//    marker's value was found in the frame, or the panel's states were not white after the turn, wh_state_white).
//  - A watchdog thread (WH_WD after the turn) sets the BlinkLater byte when the page still has not followed, so
//    that xochitl's next frame goes out as a complete refresh; the 20 s "hb" tick is the last resort.
// What it costs: the page appears about 1 s later than with the double pass (the turn as a whole takes about as
// long); quick leafing shows white until a short pause; a pen stroke or a menu right after the turn appears with the
// page; 14 MB more memory. Not measured: whether white -> page leaves no trace (the user's eye decides). "set
// turn_full 2" at runtime is 1.8.26's behaviour. Host tests: T50 (test_white_turn) and the fuzz's picture checks;
// the watchdog thread runs on the tablet only.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <link.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#define GG_VERSION "ggsense 1.8.30"
#ifndef RUNDIR
#define RUNDIR "/run/ghostguard"
#endif

// ---------------------------------------------------------------- Qt ABI pieces (Qt 6)
typedef struct {
    int version;
    int line;
    const char *file;
    const char *function;
    const char *category;
} QMessageLogContext;

typedef struct {
    void *d;
    const uint16_t *ptr;
    long long size;
} QString;

typedef void (*QtMessageHandler)(int type, const QMessageLogContext *ctx, const QString *msg);

// ---------------------------------------------------------------- geometry / model
#define SW 1620
#define SH 2160
#define CELL 16
#define SUB 2 // one sample per SUBxSUB block
#define PW (SW / SUB)
#define PH (SH / SUB)
#define NS ((size_t)PW * PH)
#define GW ((SW + CELL - 1) / CELL) // 102
#define GH ((SH + CELL - 1) / CELL) // 135
#define NCELL (GW * GH)
#define CELL_OF(px, py) ((((py) * SUB) / CELL) * GW + ((px) * SUB) / CELL)

enum { CL_UI, CL_CONTENT, CL_PEN, CL_MONO, CL_ANIM, CL_OVERLAY, CL_N };
static const char *cl_name[CL_N] = {"ui", "content", "pen", "mono", "anim", "overlay"};

typedef struct { int x, y, w, h; } Rect;
#define MAXRECTS 96

typedef struct {
    double w[CL_N];     // residue weight per update class (waveform family)
    double beta;        // weight of light->dark changes relative to dark->light
    double tau;         // residue (in full black->white transitions) from which a sample counts as ghost
    int light_min;      // a ghost sample must currently be shown at least this light (0..255)
    int cell_min;       // ghost samples (of 64) that make a cell hot
    int min_cells;      // hot cells needed for any recommendation
    int soon_px;        // ghost samples for "soon"
    int now_px;         // ghost samples for "now"
    double quiet_s;     // cells changed more recently than this are ignored (animations)
    double regional_max; // regional clean only if the hot bbox covers at most this fraction of the screen
    double w_std;       // residue weight of samples whose value changes inside a complete (STD/sleep) refresh
    int defer;          // 1: move the stock view-change flash to after the screen settled (needs the QML clock)
    double tr_quiet;    // seconds without any screen update before that in-place clean
    double tr_full;     // changed area (fraction of the screen) from which the clean covers the whole screen
                        // (1.8.4: 0, every view change gets the whole screen; only the tests set it directly)
    int tr_pad;         // pixels added around the changed area
    double auto_quiet;  // need-based clean: seconds without updates first; cells changed earlier count as settled
    int auto_pad;       // pixels added around the hot area
    int clean_wf;       // waveform of our in-place cleans: 2 = stock STD path (default); see do_clean
    int clean_text;     // 1.8.15: 1 = a clean of a page without colours is sent with TEXT targets (see clean_text_ok)
    int debug;          // 1: also forward rm.framebuffer.updates lines and GGQ lines to the journal
    // 1.8 triggers (see the header); all off until the QML clock turns them on
    int sd_close;       // document close: withdraw the stock flash, one in-place clean after the library settled
    int testcmd;        // 1.8.7: 1 = the test commands "send" and "bleach" are accepted (0 at every start)
    int gesture_clean;  // after a document gesture whose ScreenDriver clean was skipped: its redraw becomes a full STD
    double gesture_min; // the ScreenDriver's dirty value from which a skipped clean is promoted
    int page_clean;     // after a page turn (content update >= page_min) when the model sees residue
    double page_min;    // fraction of the screen a content update must cover to count as a page turn
    int kb_clean;       // after the on-screen keyboard closed
    double stock_gap;   // s: no automatic clean this soon after a stock full refresh
    // 1.8.1 (see the header)
    int kb_open_clean;  // the keyboard band once, right after the keyboard was drawn, before the first key press
    double kbo_quiet;   // s without other updates (minor ones aside) after the keyboard frame before that clean
    // 1.8.5 (see the header): reading comes first
    int page_gap;       // page changes since a full refresh before a page clean may come (1: after any change)
    int page_px;        // ghost samples (model) from which a counted page change cleans
    int page_every;     // the page_every-th change since a full refresh cleans whatever the model says (0: no cap)
    double page_flip;   // s: a page turn sooner than this after the previous page change is leafing
    double page_settle; // s without any update before a clean that fell due while the user was leafing
    // 1.8.6 (see the header)
    int turn_px;        // ghost samples (model) from which a due page turn cleans (gesture redraws keep page_px)
    int turn_px_col;    // 1.8.22: the same for a turn cd_std 2 left without co-drive for a colour (the lower of the two counts)
    double page_quiet;  // s without updates before a page turn's clean (not leafing); also the model's window for it
    // 1.8.8 co-drive (see "1.8.8 co-drive" below)
    int codrive;        // 0 off, 1 co-drive; a turn keeps its clean unless the helper confirms (1.8.13), 2 co-drive and the clean (tests)
    double cd_gain;     // gain of the co-drive towards white (erase-heavy turns)
    double cd_gpos;     // gain towards dark (draw-heavy turns); 0 = never
    double cd_minv;     // V: peak of the mean drive below which a turn is left alone
    int cd_area;        // px: smallest update (bounding box / node) that counts as a page turn
    int cd_comp;        // 1 = drive the strip the turn's update leaves out too
    int cd_max;         // V: largest co-drive level (6, 12 or 24)
    int cd_early;       // 1.8.10: 1 = a pre-armed page turn's companion updates are sent at once (0: once its node shows)
                        // 1.8.12: 2 = queued by the GUI thread inside the turn's own swapBuffers (same batch, same frame)
    int cd_pre;         // 1.8.9: 1 = write the co-drive before the update is queued (0: only once its node shows)
    // 1.8.18 co-drive of STD page turns (see "1.8.18: the co-drive of an STD page turn" below)
    int cd_std;         // the co-drive of an STD page turn: 0 none (1.8.17), 1 every one (1.8.21), 2 only between pages without
                        // colour (1.8.22: a turn away from or to a page with colour gets the page clean)
    int turn_full;      // 1.8.22: 1 = a page turn away from a page with colour goes out as a full STD itself;
                        // 1.8.26: 2 = the same, and one in-place full clean follows once the panel is idle (the second pass)
    double cds_gain;    // its gain towards white, on the mean drive of the whole screen
    double cds_gpos;    // its gain towards dark
    int cds_lead;       // frames between the progress of the turn's node and the first frame that is written
    int cds_every;      // N > 0: every Nth co-driven STD turn (counted from the last failed one) gets the page clean all the same (0: none)
    // 1.8.21: the shape of that co-drive (see the header; cds_shape, cd_quant_m)
    double cds_k27, cds_k29; // factor on the drive of the hold cells of column 27 / of column 29
    double cds_k28;          // a further factor for the rows from levels 28 and 29
    int cds_lv;              // the codes' levels (bit 0 = 6 V, 1 = 12 V, 2 = 24 V); 0: as cd_max says
    int cds_carry;           // 1 = the dose of the frames before the first written one is driven after them (0: dropped)
    int cds_w0, cds_w1;      // a window of frames, w0 <= f < w1, in which the drive is c * cds_wk + cds_wadd
    double cds_wk, cds_wadd;
    int cds_wt;              // 1.8.23: idle frames at the end of the ink -> white rows that get -12 V in a co-driven STD turn (0: none)
    int wt_static;           // 1.8.30: 1 the white tail stands in the STD table of 22-28 C for good (0: taken out, cds_wt per turn)
    int cds_flip;            // 1.8.25: 1 the STD table's rows 28 -> 30 and 30 -> 28 are at 0 V while pages are turned (0: always the stock rows)
    // 1.8.26 (see the header): which pages get the full STD turn, and when its second pass goes out
    int tf_chroma;           // % of the page's colour samples that must be unbalanced primaries (0: any colour sample, as in 1.8.22)
    int tf_chroma_min;       // and that unbalanced part must be at least this many 1/10000 of the sampled pixels
    int tf_idle;             // 1 the second pass waits until the panel is idle (0: it is queued right behind the turn's frame)
} Config;

static Config cfg = {
    .sd_close = 0,
    .gesture_clean = 0,
    .testcmd = 0,
    .gesture_min = 0.02,
    .page_clean = 0,
    .page_min = 0.5,
    .kb_clean = 0,
    .stock_gap = 3.0,
    .kb_open_clean = 0,
    .kbo_quiet = 0.4,
    .page_gap = 1, // 1.8.6
    .page_px = 80000,
    .page_every = 6,
    .page_flip = 2.0,
    .page_settle = 1.5,
    .turn_px = 6000,
    .turn_px_col = 400, // 1.8.22
    .page_quiet = 0.2,
    .codrive = 1, // 1.8.13
    .cd_gain = 1.0,
    .cd_gpos = 0.4, // 1.8.13 (chains of 26-28 turns stay within 1-2 %)
    .cd_minv = 0.7,
    .cd_area = 1000000,
    .cd_comp = 1,
    .cd_max = 12,
    .cd_pre = 1,
    .cd_early = 2,
    .cd_std = 1, // 1.8.23 (2 in 1.8.22: a clean after every turn of a page with colour or greys, which the user will not have)
    .wt_static = 1, // 1.8.30
    .turn_full = 1, // 1.8.30: one full STD with the white tail in the table (wt_static); 3 was 1.8.29: the full STD turn goes out white and the page follows (2 in 1.8.26: the turn and a second
                    // pass in place; 0 in 1.8.22 .. 1.8.25)
    .cds_gain = 1.07,
    .cds_gpos = 0.25, // 1.8.20 (0.43 before)
    .cds_lead = 4,
    .cds_every = 0,
    .cds_k27 = 1.0, .cds_k29 = 1.0, .cds_k28 = 1.0, // 1.8.21: all of these leave 1.8.20's co-drive as it is
    .cds_lv = 0,
    .cds_carry = 1,
    .cds_w0 = 0, .cds_w1 = 0, .cds_wk = 1.0, .cds_wadd = 0.0,
    .cds_wt = 7, // 1.8.23
    .cds_flip = 1, // 1.8.25
    .tf_chroma = 25, .tf_chroma_min = 30, .tf_idle = 1, // 1.8.26
    .defer = 0,
    .tr_quiet = 0.2,
    .tr_full = 0.0, // 1.8.4 (see the header): a regional STD stretches the updates that run outside it
    .tr_pad = 16,
    .auto_quiet = 0.4,
    .auto_pad = 16,
    .clean_wf = 2,
    .clean_text = 1,
    .w_std = 1.0,
    .w = {1.0, 0.5, 1.0, 0.8, 1.5, 1.0},
    .beta = 0.35,
    .tau = 1.0,
    .light_min = 150,
    .cell_min = 6,
    .min_cells = 12,
    .soon_px = 6000,
    .now_px = 24000,
    .quiet_s = 2.0,
    .regional_max = 0.35,
    .debug = 0,
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t *fb;    // RGB32 frame of the EPFramebuffer object (see fb_refresh)
static long long fb_bpl;
static int gg_off;           // 1 in any process other than /usr/bin/xochitl: fully passive
static uint8_t *prev;        // our copy of the displayed luma, one byte per sample
static uint16_t *res;        // accumulated residue per sample (luma units, weighted)
static uint8_t *chg_mark;    // scratch: 1 = sample changed by the complete refresh being applied
static uint32_t *chg_idx;    // samples changed by the update being parsed
static int16_t *chg_d;       //   and their luma change (new - old)
static size_t chg_n;
static int prev_valid;
static double cell_touch[NCELL]; // last time the cell's content changed
static uint8_t touched[NCELL];

// pending update being assembled from log lines
static int pend_active;
static int pend_flags;
static int pend_complete;    // 1 = STD, 2 = sleep
static int pend_merged;
static double pend_t;
static int pend_sd_busy; // the ScreenDriver was in a gesture or its mono state when this frame was sent
static Rect pend_rects[MAXRECTS]; // region of the update being parsed
static int pend_nrects;
static Rect pend_cls_rects[CL_N][MAXRECTS];
static int pend_cls_n[CL_N];
static int pend_tr;          // a view change was waiting for its clean when this update began
static double pend_vc_t;     //   the newest view change at that time (a later one makes this update older)
static int pend_after_vc;    //   that view change had drawn a frame before this update
static Rect pend_tr_box;     //   and what it had redrawn before this update

// statistics
static unsigned long n_updates, n_complete, n_std, n_sleep, n_partial, n_full, n_regional;
static unsigned long n_rclean_ok, n_rclean_fail, n_cmds, n_sim;
static unsigned long n_clean_wf[16]; // our successful cleans per waveform (see do_clean); [3] = wf 2 with TEXT targets
static int last_clean_text;          // 1.8.15: the last clean went out with TEXT targets (set by do_rclean)
static int last_text_block;          // 1.8.16: 2 = it kept the stock clean because of an STD black (level 2) in the states
static unsigned long n_std_chg; // samples whose value changed inside complete refreshes (they keep residue)
static unsigned long chg_cls[CL_N]; // changed samples of partial updates, per update class
static double t_last_update, t_last_complete, t_last_full;
static double t_last_major; // the last update that was not a minor ui update (see minor_ui_update)
static double proc_ms_total, proc_ms_max;

// view changes whose stock flash was moved to after the screen settled (see deferred_clean)
static int tr_active;      // a view change waits for its in-place clean
static double tr_t0;       // when it was announced
static Rect tr_box;        // everything updated on screen since then
static int tr_withdrawn;   // its stock flash was withdrawn in time
static int tr_late;        // a stock full refresh went out after it anyway
static char tr_what[48];
static double hb_t;        // last sign of life of the QML clock
static unsigned long n_tr, n_wd, n_wd_miss, n_wd_late, n_tr_clean, n_tr_fail;
static double tr_vc_last;  // the newest view change of the cycle (a joining one too)
static int tr_inked;       // ink was drawn while the clean waited (the user writes on the new view)
static int tr_kb_held;     // the clean waited for the on-screen keyboard: the cycle is kept like a view written on
static unsigned long n_tr_absorbed, n_tr_aged;
#define TR_FRAME_MAX 3.0   // s: a view change whose first frame has not come by then drew nothing
#define TR_INK_QUIET 1.5   // s without updates before the clean when ink was drawn on the new view
#define TR_HB_QUIET 3.0    // s without updates before the 20 s heartbeat (not the settle clock) may clean
#define TR_MAX_AGE 30.0    // s after the newest view change: an older cycle is dropped (its flash would no longer belong to it)
#define TR_INK_MAX_AGE 600.0 // s: the same for a cycle whose view is being written on (the clean waits for a pause)

// need-based clean (1.6, "GGQ <id> auto <why>", see auto_try)
static int auto_req;       // a request waits for the screen to settle
static double auto_t;      // when it came
static char auto_why[16];
static unsigned long n_auto, n_auto_clean, n_auto_none, n_auto_aged, n_auto_fail;
#define AUTO_MAX_AGE 4.0   // s: a request the screen never settled for is dropped (the next burst sends a new one)
#define AUTO_PEN_MAX_AGE 8.0 // s: the same while the pen is near (a request never runs while it is near)

// 1.8 (see the header)
enum { AK_NONE, AK_PAGE, AK_GESTURE, AK_KB, AK_QML };
static const char *ak_name[] = {"none", "page", "gesture", "keyboard", "qml"};
static int auto_kind;       // what the waiting request is for (AK_*)
static int auto_inked;      // ink was drawn after the request
static int auto_pen_held;   // the pen held this request back at least once: it may wait AUTO_PEN_MAX_AGE
static double auto_quiet_need; // 1.8.5: s without updates the waiting request needs (auto_quiet; page_settle after leafing)
// 1.8.5 (see the header): page changes since the last full refresh, ours or stock's
static int n_pchg;             // page turns and gesture redraws counted since then
static double t_pchg_prev;     // when the previous page change was drawn
static int auto_page_cap;      // the waiting page request was armed by the cap (page_every): no model gate
static int auto_page_col;      // 1.8.22: the waiting page request is for a turn left without co-drive for a colour (turn_px_col)
static long auto_page_id;      // 1.8.22: the turn whose answer armed the waiting page request (cd_fail_tick), 0: none
static int auto_page_idle;     // 1.8.26: the waiting page request is the second pass of a turn that went out as the full
                               // STD (turn_full 2). It can be stale while no request waits: read it together with auto_req
static double auto_own_full;   // 1.8.26: when that turn's own frame was sent (its t_stock_full once it is finalized), 0: none
static unsigned long n_page_wait, n_page_flip; // page changes that were not due / due cleans that waited for leafing
static unsigned long n_page_none; // due page changes the model found clean enough (ghost < page_px)
static int sd_visible;      // the document view is visible (ScreenDriver "visibleChanged")
static int sd_gesture;      // a document gesture is in progress ("gestureModeChanged: true")
static int sd_mono;         // the ScreenDriver draws in its mono state ("full -> mono" .. "mono -> full")
static int sd_tiles;        // the document view still renders tiles ("pendingTilesChanged: true")
static int sleeping;        // the display left the Normal state (rm.batterymanager), until it is Normal again
static double sleep_soon_t; // the light-sleep timer fired (the sleep screen follows)
static double t_stock_full; // the last stock full-screen complete refresh (ours never pass through on_update_line)
static double t_our_full;   // the last full-screen clean of ours
static double t_popup;      // 1.8.22: the last update that opened something over the screen (popup_track's shape)
static double t_ink;        // 1.8.22: the last ink drawn (not a pen tap on the UI)
static int tr_sd;           // the waiting cycle includes a document close (ScreenDriver)
static Rect kb_box;         // the on-screen keyboard's area while it is shown (A2 or Overlay, wide, at the bottom)
static int kb_on;
static uint8_t kb_cov[NCELL]; // cells of kb_box redrawn by ui/content updates since the keyboard was last drawn
static unsigned long n_sd_close, n_sd_skip, n_sleep_back, n_sleep_frame, n_wake;
static unsigned long n_arm[5], n_auto_stock, n_auto_ink, n_auto_covered;
static double t_wake;
static double sd_dirty = -1.0; // the ScreenDriver's last "clean the screen, dirty=" value, until its decision
static double sd_full_t;       // the ScreenDriver left its mono state ("mono -> full"); its full redraw follows (~50 ms)
static double sd_newpage_t;    // the ScreenDriver logged "finalizeNewPage:" (a page turn; its frame follows in ~50 ms)
static double sd_newpage_prev_t; // 1.8.26: the one before it (0: none): two of them within page_flip are leafing
static long sd_newpage_n;      // 1.8.19: how many of them: the frames of one turn share the number
static long cd_np_n;           // 1.8.19, under mu: the sd_newpage_n of the turn that got the newest co-drive id
static long cd_track_id;       // 1.8.19, under mu: the turn page_track counted last
static double sd_open_t;       // a document was opened (the view became visible, not at a wake); its in-place STD follows
static int sd_tiles_open;      // sd_tiles came from that open (no "pendingTilesChanged: true" line): it lapses after 2 s
static int sd_pen;             // the ScreenDriver's own pen state ("penCloseChanged: true/false", logged also while the
                               // document view is hidden, j17499); used only while it is visible, reset at sleep
static int sd_mono_gesture;    // a gesture took part in the current mono state (selection mono states do not count)
static int promote_live;       // our promoted BlinkLater byte has not been taken by a frame yet
static int tr_promoted;        // a waiting view-change cycle ends in the promoted redraw (at the frame that takes it)
#define TURN_FULL_MAX_AGE 1.0  // s: a turn's promoted byte no frame took by then is taken back at the next tick
static long turn_full_np;      // 1.8.22, under mu: the sd_newpage_n of the turn whose frame was promoted (turn_full);
                               // 0 once a frame took the byte or the byte in flight is the gesture end's
static unsigned long n_turn_full, n_turn_full_took; // turns promoted that way / whose frame took the byte
static unsigned long n_second_pass; // 1.8.26: second passes that went out (turn_full 2)
static double turn_full_t;     // under mu: when that byte was set (its own age: a later finalizeNewPage moves sd_newpage_t)
static long turn_full_done_n;  // under mu: the sd_newpage_n of the last turn whose frame went out as that full STD
static int pend_turn_full;     // under mu: the pending frame is a later frame of that turn (the refresh covers it)
static const char *unpromote_why = ""; // what took the promoted byte back (for the log line)
static unsigned long n_unpromote; // promoted bytes taken back (a new gesture, the pen, a document close came first)
static unsigned long n_popup;  // requests dropped because a menu or popup opened after them
static unsigned long n_minor_upd; // minor ui updates (minor_ui_update): they do not delay a view change's clean
static unsigned long n_promote, n_promote_skip; // skipped gesture cleans promoted to a full STD / left (dirty too low)

// 1.8.1 keyboard-open clean (see the header and kbo_try)
static int kbo_req;            // the keyboard band waits for its clean
static double kbo_t;           // the keyboard's frame
static Rect kbo_box;           // the band to clean (kb_box of that frame)
static unsigned long n_kbo, n_kbo_clean, n_kbo_drop, n_kbo_fail;
#define KBO_MAX_AGE 1.0        // s after the keyboard frame: a clean that has not started by then is dropped (1.8.2:
                               // was 4 s; a ~1 s STD started later runs into the first key presses)

// 1.8.3 SWTCON gate (see the header and swtcon_idle)
static int last_sw = -2;          // SWTCON at the last clean sent or held back: 1 idle, 0 driving, -1 unknown, -2 not read
static unsigned long n_inflight;  // regional cleans held back because the panel was still being driven
static unsigned long n_upgrade;   // view-change cleans made full-screen after waiting TR_IDLE_MAX for the panel
static unsigned long n_full_busy; // full-screen cleans of ours sent while the panel was still being driven (log only)
static double tr_wait_t;          // the waiting cycle's clean first found the panel busy (0: not yet)
#define TR_IDLE_MAX 3.0           // s a regional view-change clean waits for the panel; then it covers the whole screen

static QtMessageHandler next_handler;  // xochitl's handler (or Qt default)
static QtMessageHandler qt_default_h;  // Qt's own default handler, as found at load time
static QtMessageHandler (*real_install)(QtMessageHandler);
static int our_installed;

#ifdef GG_REPLAY
static double replay_now; // the journal replay tool sets the time of each line
static double now_s(void) { return replay_now; }
#else
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

static void glog(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef GG_REPLAY
    fprintf(stderr, "%12.3f GGSENSE %s\n", replay_now, buf);
#else
    fprintf(stderr, "GGSENSE %s\n", buf);
#endif
    fflush(stderr);
}

// ---- 1.8.29: the white intermediate of a colour-page turn (turn_full 3) ------------------------------------------
// A full STD that changes the content leaves a faint trace of the old page, and the second pass that removes it is
// a flash on a page the reader already sees (1.8.26). Here the turn's own full STD goes out with a white frame
// instead (old page -> white) and the page follows as the next complete refresh (white -> page): two passes as
// before, none of them on a page that is already there. While the page is held back, the frame xochitl converts
// holds WH_SENT (a white whose unused alpha byte is 0; the painter writes 0xff there, and the converter does not
// read that byte) and the true pixels wait in wh_shadow. The frame is written only with the EPFramebuffer's lock
// held: in the swapBuffers hook (xochitl holds it) and in do_rclean.
#define WH_SENT 0x00ffffffu
#define WH_HOLD 3.0          // s after the turn: from here the page goes out even if something still looks busy
#define WH_WD 3.6            // s after the turn: no tick brought the page -> the watchdog makes the next frame complete
static uint32_t *wh_shadow;  // the true pixels behind the sentinels (the frame's layout; allocated once, never freed)
static int wh_active;        // the frame holds sentinels
static int wh_owed;          // the panel shows white where the page belongs: a complete full-screen refresh is owed
static int wh_off;           // a check failed: no white intermediate until the next start (the double pass instead)
static double wh_t0;         // when the page was hidden
static int wh_pend;          // ticks in a row that found a BlinkLater byte in the way of the page
static int wh_checked;       // this hide's self-check has run
static unsigned long n_wh_hide, n_wh_absorb, n_wh_reveal, n_wh_stock, n_wh_early, n_wh_wd, n_wh_bad, n_wh_no;
static long wh_chk_n = -1, wh_chk_bad = -1; // the last self-check: samples read, samples that were not white

static inline int luma_at(const uint8_t *row, int x) {
    const uint8_t *p = row + x * 4; // little endian 0xffRRGGBB -> B G R X
    // 1.8.29: a pixel the white intermediate holds back is read as xochitl drew it (the model follows the page)
    if (__atomic_load_n(&wh_active, __ATOMIC_RELAXED) && p[3] == 0 && p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && wh_shadow)
        p = (const uint8_t *)wh_shadow + (p - fb);
    return (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
}

// frame lock held: the true pixels return to the frame
static void wh_unhide(void) {
    if (!__atomic_load_n(&wh_active, __ATOMIC_ACQUIRE)) return;
    if (fb && wh_shadow) {
        for (int y = 0; y < SH; y++) {
            uint32_t *f = (uint32_t *)(uintptr_t)(fb + (long long)y * fb_bpl);
            const uint32_t *s = (const uint32_t *)((const uint8_t *)wh_shadow + (long long)y * fb_bpl);
            for (int x = 0; x < SW; x++)
                if (f[x] == WH_SENT) f[x] = s[x];
        }
    }
    __atomic_store_n(&wh_active, 0, __ATOMIC_RELEASE);
}

// frame lock held: every pixel xochitl has drawn moves behind a sentinel. first: the frame held no sentinel yet,
// so one found there is xochitl's own (never seen; it reads back as white). Returns the pixels moved, or the
// strays found when first.
static long wh_cover(int first) {
    long n = 0;
    for (int y = 0; y < SH; y++) {
        uint32_t *f = (uint32_t *)(uintptr_t)(fb + (long long)y * fb_bpl);
        uint32_t *s = (uint32_t *)((uint8_t *)wh_shadow + (long long)y * fb_bpl);
        if (first) {
            for (int x = 0; x < SW; x++) {
                if (f[x] == WH_SENT) { s[x] = 0xffffffffu; n++; }
                else { s[x] = f[x]; f[x] = WH_SENT; }
            }
        } else {
            for (int x = 0; x < SW; x++)
                if (f[x] != WH_SENT) { s[x] = f[x]; f[x] = WH_SENT; n++; }
        }
    }
    return n;
}

// do_rclean, frame lock held, right before its send. A clean while the page is owed is the page's arrival: it
// covers the whole screen and reads the true frame. A refusal while the white refresh is still on the panel.
static const char *wh_clean_gate(Rect *r, int sw, int *reveal) {
    *reveal = 0;
    if (!__atomic_load_n(&wh_owed, __ATOMIC_ACQUIRE)) { wh_unhide(); return NULL; }
    if (sw == 0 && now_s() - wh_t0 < WH_HOLD) return "inflight";
    r->x = 0; r->y = 0; r->w = SW; r->h = SH;
    wh_unhide();
    *reveal = 1;
    return NULL;
}
static void wh_clean_sent(void) {
    __atomic_store_n(&wh_owed, 0, __ATOMIC_RELEASE);
    n_wh_reveal++;
}
static int wh_hide(double t0);                      // defined behind the panel's helpers
static void wh_swap_hook(const char *m, int flags); // defined in front of on_update_line

static void ensure_prev(void) {
    if (!prev) {
        prev = malloc(NS);
        res = calloc(NS, sizeof *res);
        chg_mark = calloc(NS, 1);
        chg_idx = malloc(NS * sizeof *chg_idx);
        chg_d = malloc(NS * sizeof *chg_d);
        if (!prev || !res || !chg_mark || !chg_idx || !chg_d) {
            free(prev); free(res); free(chg_mark); free(chg_idx); free(chg_d);
            prev = NULL; res = NULL; chg_mark = NULL; chg_idx = NULL; chg_d = NULL;
            return;
        }
    }
    if (!prev_valid && fb) {
        for (int py = 0; py < PH; py++) {
            const uint8_t *row = fb + (long long)(py * SUB) * fb_bpl;
            uint8_t *pr = prev + (size_t)py * PW;
            for (int px = 0; px < PW; px++) pr[px] = (uint8_t)luma_at(row, px * SUB);
        }
        prev_valid = 1;
        chg_n = 0;
    }
}

// 64-bit edges: coordinates come from log text and from QML, and must never wrap
static int clip_rect(Rect *r) {
    long long x0 = r->x, y0 = r->y, x1 = (long long)r->x + r->w, y1 = (long long)r->y + r->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    if (x1 <= x0 || y1 <= y0) return 0;
    r->x = (int)x0;
    r->y = (int)y0;
    r->w = (int)(x1 - x0);
    r->h = (int)(y1 - y0);
    return 1;
}

// bounding box of a and b (b clipped first; an empty a takes b)
static void rect_union(Rect *a, Rect b) {
    if (!clip_rect(&b)) return;
    if (a->w <= 0 || a->h <= 0) { *a = b; return; }
    int x0 = a->x < b.x ? a->x : b.x, y0 = a->y < b.y ? a->y : b.y;
    int x1 = a->x + a->w > b.x + b.w ? a->x + a->w : b.x + b.w;
    int y1 = a->y + a->h > b.y + b.h ? a->y + a->h : b.y + b.h;
    *a = (Rect){x0, y0, x1 - x0, y1 - y0};
}

// 1.8.3: the rectangle (clipped) is the whole screen: the only clean that may go out while the panel is being driven
static int rect_full(Rect r) { return clip_rect(&r) && r.x == 0 && r.y == 0 && r.w == SW && r.h == SH; }

// sample index range covering a pixel rectangle (at least one sample per axis)
static void sample_range(Rect r, int *px0, int *px1, int *py0, int *py1) {
    *px0 = (r.x + SUB - 1) / SUB; *px1 = (r.x + r.w + SUB - 1) / SUB;
    *py0 = (r.y + SUB - 1) / SUB; *py1 = (r.y + r.h + SUB - 1) / SUB;
    if (*px1 > PW) *px1 = PW;
    if (*py1 > PH) *py1 = PH;
    if (*px1 <= *px0) { *px0 = r.x / SUB; *px1 = *px0 + 1; }
    if (*py1 <= *py0) { *py0 = r.y / SUB; *py1 = *py0 + 1; }
}

// 1.8.8 co-drive: luma that went up / down in the update being scanned (sum over its samples), and its counters
static unsigned long long cd_up, cd_dn;
enum { CD_REQ, CD_ARM, CD_MISS, CD_STD, CD_SMALL, CD_BAD, CD_COMP, CD_COMPFAIL, CD_ABORT, CD_PRE, CD_EARLY, CD_STDCD, CD_N };
static unsigned long n_cd[CD_N];
static int cd_last;             // +-6 V frames of the last co-drive (negative: towards white)
static double cd_lfe, cd_lfd;   // erased / drawn part of the screen in the last large update
static long cd_fail_ms;         // 1.8.10: when cd_fail_id was written (ms on the now_s clock)
static int cd_std_next;         // 1.8.10: a full refresh went over the screen: xochitl draws the next turn as an STD
static int cd_text_next;        // 1.8.15: that full refresh was our clean with TEXT targets: the next turn is expected as a GLR
static void cd_request(double fe, double fd, Rect bb, int turn, int full);
static void cdf_line(int full);        // 1.8.25 (the flip rows, see "the two whites"): every swapBuffers line
static void cdf_full(const char *why); // 1.8.25: a complete refresh is about to be sent: the stock rows, now
static void wts_kick(void);            // 1.8.30: a promoted turn: its STD table gets the white tail (wt_static)

// 1.8.13: a page turn keeps its clean unless the helper confirms the turn's co-drive. 1.8.8-1.8.12 had it the other
// way round (no clean unless the helper marked a failure), and an STD turn set no mark: turns on to and off a colour
// page (xochitl draws them as an STD whenever the state buffer holds a colour level) went without co-drive and
// without clean, the white of the next pages fell by 20-60 % (survey a2, 10 Nov). Every turn handed to the helper
// gets an id; the helper answers with cd_ok_id (co-driven in full, or a GLR that needed none, or the one black and
// white STD right after a full refresh, see cd_main) or cd_fail_id; cd_fail_tick arms the page clean for a failure and
// for a turn that got no answer within 2 s. A turn the helper was not asked about keeps the 1.8.6 path in page_track.
static long cd_turn_id;         // GUI thread: the id of the last page turn handed to the helper
static long cd_turn_ms;         // ... and when (ms on the now_s clock)
static long cd_ok_id;           // the last turn that needs no clean
static long cd_fail_id;         // the last turn that went without its co-drive (cd_fail_ms: when)
static long cd_col_id;          // 1.8.22: the last turn cd_std 2 left without co-drive because the page before or after
                                // it holds a colour (written before that turn's cd_fail_id): turn_px_col decides its clean
static long cd_fail_seen;       // under mu: the highest turn id that is dealt with (its failure armed a clean or was
                                // dropped, or a full refresh went over the screen after it)
static long cd_pend_id;         // under mu: the turn page_track waits an answer for (0: none), since cd_pend_ms
static long cd_pend_ms;
// 1.8.24: how long page_track waits for the helper's answer. It was 2 s; a page turn that waits behind another update
// (the page number's hide, 1 s) is found up to 1.5 s after its frame and confirmed when its last coded frame has
// passed, 0.8 s later: the answer of a good turn would come after the limit.
#define CD_PEND_MS 3000
static long pend_cd_turn;       // 1.8.14, under mu: the id the pending frame got from the helper (0: it was not asked)
static double cd_turn_prev_t;   // 1.8.14, under mu: when the previous turn handed to the helper was drawn
static int cd_leaf;             // ... and whether the last one followed it sooner than page_flip (the user leafs)

// compare frame vs prev inside r; record changed samples (unless sync_only) and sync prev
static void scan_rect(Rect r, int sync_only, double t) {
    if (!clip_rect(&r)) return;
    int px0, px1, py0, py1;
    sample_range(r, &px0, &px1, &py0, &py1);
    for (int py = py0; py < py1; py++) {
        const uint8_t *row = fb + (long long)(py * SUB) * fb_bpl;
        uint8_t *pr = prev + (size_t)py * PW;
        for (int px = px0; px < px1; px++) {
            int n = luma_at(row, px * SUB);
            int o = pr[px];
            if (n == o) continue;
            if (n > o) cd_up += (unsigned)(n - o); else cd_dn += (unsigned)(o - n);
            int c = CELL_OF(px, py);
            cell_touch[c] = t;
            if (!sync_only && chg_n < NS) {
                chg_idx[chg_n] = (uint32_t)((size_t)py * PW + px);
                chg_d[chg_n] = (int16_t)(n - o);
                chg_n++;
                touched[c] = 1;
            }
            pr[px] = (uint8_t)n;
        }
    }
}

static int cell_in_rects(int c, Rect *rs, int n) {
    int cx = (c % GW) * CELL + CELL / 2, cy = (c / GW) * CELL + CELL / 2;
    for (int i = 0; i < n; i++)
        if (cx >= rs[i].x - CELL / 2 && cx < rs[i].x + rs[i].w + CELL / 2 && cy >= rs[i].y - CELL / 2 &&
            cy < rs[i].y + rs[i].h + CELL / 2)
            return 1;
    return 0;
}

static void clear_residue_rect(Rect r) {
    if (!clip_rect(&r)) return;
    int px0, px1, py0, py1;
    sample_range(r, &px0, &px1, &py0, &py1);
    for (int py = py0; py < py1; py++) memset(res + (size_t)py * PW + px0, 0, (size_t)(px1 - px0) * sizeof *res);
}

// complete refresh: clear the residue of the samples in r that it did not change (chg_mark == 0)
static void clear_residue_rect_unchanged(Rect r) {
    if (!clip_rect(&r)) return;
    int px0, px1, py0, py1;
    sample_range(r, &px0, &px1, &py0, &py1);
    for (int py = py0; py < py1; py++) {
        uint16_t *rr = res + (size_t)py * PW;
        const uint8_t *mk = chg_mark + (size_t)py * PW;
        for (int px = px0; px < px1; px++)
            if (!mk[px]) rr[px] = 0;
    }
}

static void debug_complete(const char *kind, long long area);

// A stock complete refresh while a view change waits for its in-place clean (see deferred_clean). As the
// first frame of the newest view it is the stock flash that went out anyway (it changes the content, so
// the clean is still needed; noted at the swapBuffers line). Later, a full-screen one that changes almost
// nothing in the area the view change redrew drives that area in place, which is exactly what the clean
// would do (e.g. ScreenDriver's "clear on next update" after a document's tiles): the cycle ends there.
static void tr_on_complete(long long area) {
    // the update must belong to the waiting cycle: an update is applied at the NEXT swapBuffers line, so a
    // view change may have been announced after this update went out
    if (!tr_active || !pend_tr || pend_vc_t != tr_vc_last || !pend_after_vc) return;
    if (area < (long long)SW * SH * 95 / 100) return;
    Rect b = pend_tr_box;
    if (!clip_rect(&b)) return;
    int px0, px1, py0, py1;
    sample_range(b, &px0, &px1, &py0, &py1);
    size_t inbox = 0, nbox = (size_t)(px1 - px0) * (size_t)(py1 - py0);
    for (size_t k = 0; k < chg_n; k++) {
        int px = (int)(chg_idx[k] % PW), py = (int)(chg_idx[k] / PW);
        if (px >= px0 && px < px1 && py >= py0 && py < py1) inbox++;
    }
    if (inbox * 100 >= nbox) return;
    tr_active = 0;
    n_tr_absorbed++;
    glog("view change %s: the stock flash %.0f ms later went over it in place (%zu of %zu samples changed), no clean",
         tr_what, (pend_t - tr_t0) * 1000.0, inbox, nbox);
}

// ---------------------------------------------------------------- 1.8 triggers (called with mu held)
static void force_pending(void);

// A clean request from one of xochitl's own events (see the header). A page request (model decides) never
// replaces a waiting gesture or keyboard request (those clean without asking the model).
static void arm_auto(int kind, const char *why) {
    if (sleeping || kind <= AK_NONE || kind > AK_QML) return;
    n_arm[kind]++;
    if (auto_req && kind == AK_PAGE && auto_kind != AK_PAGE) return;
    auto_req = 1;
    auto_kind = kind;
    auto_inked = 0;
    auto_pen_held = 0;
    auto_quiet_need = cfg.auto_quiet;
    auto_page_cap = 0;
    auto_page_col = 0;
    auto_page_id = 0;
    auto_page_idle = 0; // 1.8.26
    auto_own_full = 0;
    auto_t = now_s();
    n_auto++;
    snprintf(auto_why, sizeof auto_why, "%s", why);
}

// 1.8.26: the stock full refresh a waiting request has to respect. The second pass of a turn that went out as the
// full STD follows that very frame: for it the turn's own frame is neither "a stock full refresh that went over
// the screen" nor one to keep stock_gap away from. Any later stock full refresh counts as for every request.
static double auto_stock_t(void) {
    return auto_req && auto_page_idle && auto_own_full > 0 && t_stock_full <= auto_own_full ? 0.0 : t_stock_full;
}

// 1.8.26 (turn_full 2): the turn's frame (sent at t0) went out as the full STD; arm its second pass, one in-place
// full clean without the model's gate. It goes out once the panel is idle again (auto_try); while pages are being
// leafed through it waits for page_flip without updates, so quick turns get one flash each and one second pass at
// the end. 1 = armed (also over a waiting page request: the newest turn's pass is the one that counts).
static int arm_second_pass(double t0, int leaf) {
    unsigned long a0 = n_auto;
    arm_auto(AK_PAGE, "page (2nd pass)"); // auto_why holds 15 characters
    if (n_auto == a0) return 0; // asleep, or a gesture's or the keyboard's request waits: that one decides
    auto_page_cap = 1;
    auto_page_idle = 1;
    auto_own_full = t0;
    double q = cfg.page_quiet;
    if (leaf) {
        double f = cfg.page_flip < 3.0 ? cfg.page_flip : 3.0; // the settle clock ticks for 4 s after an update
        if (f > q) q = f;
    }
    auto_quiet_need = q;
    return 1;
}

// 1.8.5 (see the header): one page change (a page turn, or the redraw that ends a document gesture) drawn at t.
// 0: not due (fewer than page_gap changes since the last full refresh); 1: due if the model sees ghost >= page_px;
// 2: the cap (the page_every-th change or later), due whatever the model says. *leafing: a page turn sooner than
// page_flip after the previous change (a gesture's redraw never is: it comes at the stock time, after the finger
// stopped).
static int page_due(double t, int turn, int *leafing) {
    *leafing = turn && t_pchg_prev > 0 && t >= t_pchg_prev && t - t_pchg_prev < cfg.page_flip;
    t_pchg_prev = t;
    if (n_pchg < 1000000) n_pchg++;
    int cap = cfg.page_every > 0 && n_pchg >= cfg.page_every;
    if (!cap && n_pchg < cfg.page_gap) {
        n_page_wait++;
        return 0;
    }
    if (*leafing) n_page_flip++;
    return cap ? 2 : 1;
}

// 1.8.5: a full-screen refresh went over the screen (ours, or a stock one): the page count starts again
static void page_reset(void) {
    n_pchg = 0;
    // 1.8.9: whatever a missed co-drive left is gone too. 1.8.14: so is every turn handed to the helper so far,
    // whatever it still answers for it (an answer that came after the refresh armed a second clean)
    long f = __atomic_load_n(&cd_fail_id, __ATOMIC_ACQUIRE), id = __atomic_load_n(&cd_turn_id, __ATOMIC_ACQUIRE);
    cd_fail_seen = id > f ? id : f;
    cd_pend_id = 0;
    __atomic_store_n(&cd_std_next, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&cd_text_next, 0, __ATOMIC_RELEASE);
}

// 1.8.22 (review round 5): a waiting page request is given up without a decision (it aged out, the user writes, the
// display sleeps, a gesture began). It stood for every turn so far (page_track kept it for the page shown now), so
// their failures and open questions are dealt with, as after a full refresh: none arms a second request at the next
// tick. Not a request a full refresh has covered: a turn after that refresh still gets its own. Under mu, before
// auto_req is cleared.
static void page_given_up(void) {
    if (!auto_req || auto_kind != AK_PAGE || auto_stock_t() >= auto_t || t_our_full >= auto_t) return;
    long f = __atomic_load_n(&cd_fail_id, __ATOMIC_ACQUIRE), id = __atomic_load_n(&cd_turn_id, __ATOMIC_ACQUIRE);
    if (id < f) id = f;
    if (f > cd_fail_seen || cd_pend_id)
        glog("the page clean that is given up stood for every turn so far: no second request for them");
    if (id > cd_fail_seen) cd_fail_seen = id;
    cd_pend_id = 0;
}

static long long rects_area(const Rect *rs, int n) {
    long long a = 0;
    for (int i = 0; i < n; i++) {
        Rect r = rs[i];
        if (clip_rect(&r)) a += (long long)r.w * r.h;
    }
    return a;
}

// The on-screen keyboard is shown when one update draws a wide A2 or Overlay area (per-region class lines) in the
// lower part of the screen; key presses and the focused text field are small. It closed once ui/content updates
// have redrawn 80% of that area: then one clean (kb_clean).
static void kb_track(void) {
    static const int kcl[2] = {CL_ANIM, CL_OVERLAY};
    static const int ncl[2] = {CL_UI, CL_CONTENT};
    Rect bb = {0, 0, 0, 0};
    long long a = 0;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < pend_cls_n[kcl[k]]; i++) {
            Rect r = pend_cls_rects[kcl[k]][i];
            // only the lower part: the focused text field above it is drawn with A2 in the same class region
            if (!clip_rect(&r) || r.y < SH / 2 - 100) continue;
            a += (long long)r.w * r.h;
            rect_union(&bb, r);
        }
    if (a >= (long long)SW * SH / 10 && bb.w >= 1400) {
        int was_on = kb_on;
        kb_box = bb;
        kb_on = 1;
        memset(kb_cov, 0, sizeof kb_cov);
        if (auto_req && auto_kind == AK_KB) { // shown again before its clean: never clean while it is up
            auto_req = 0;
            n_auto_covered++;
        }
        // 1.8.1: just shown (not redrawn while up, e.g. the shift state after the first key): its band gets one
        // clean before the first key press (kbo_try); kbo_track drops it at any other update.
        // 1.8.2: only an OPENING frame arms it. In every real opening the dialog behind the keyboard is drawn in ui
        // in the same frame (~2.4 Mpx); the keyboard redrawn while up carries ~350 px of ui (the shift state after
        // the first key, 2026-11-09 09:23:32.011). kb_on can be cleared under a keyboard that stays up (sleep frame,
        // wake STD, a stock full refresh); such a redraw while typing must never arm a clean.
        int opening = rects_area(pend_cls_rects[CL_UI], pend_cls_n[CL_UI]) >= (long long)SW * SH / 8;
        if (!was_on && cfg.kb_open_clean && !sleeping && opening) {
            kbo_req = 1;
            kbo_t = pend_t;
            kbo_box = bb;
            n_kbo++;
        }
        return;
    }
    if (!kb_on) return;
    int cx0 = kb_box.x / CELL, cy0 = kb_box.y / CELL;
    int cx1 = (kb_box.x + kb_box.w + CELL - 1) / CELL, cy1 = (kb_box.y + kb_box.h + CELL - 1) / CELL;
    if (cx1 > GW) cx1 = GW;
    if (cy1 > GH) cy1 = GH;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < pend_cls_n[ncl[k]]; i++) {
            Rect r = pend_cls_rects[ncl[k]][i];
            if (!clip_rect(&r)) continue;
            for (int cy = cy0; cy < cy1; cy++) {
                int py = cy * CELL + CELL / 2;
                if (py < r.y || py >= r.y + r.h) continue;
                for (int cx = cx0; cx < cx1; cx++) {
                    int px = cx * CELL + CELL / 2;
                    if (px >= r.x && px < r.x + r.w) kb_cov[cy * GW + cx] = 1;
                }
            }
        }
    long tot = 0, cov = 0;
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            tot++;
            cov += kb_cov[cy * GW + cx];
        }
    if (tot > 0 && cov * 10 >= tot * 8) {
        kb_on = 0;
        if (cfg.kb_clean) arm_auto(AK_KB, "keyboard");
    }
}

// 1.8.9: the helper could not co-drive the page turn that was just drawn (or only late: see cd_main): this page gets
// the clean a turn got before 1.8.8, if the model sees a trace. 1.8.13: the same for a turn the helper gave no answer
// for within 2 s. 1.8.14: a single turn's clean goes out page_quiet after the page's frame as in 1.8.6 (it waited
// page_settle, 1.5 s: the flash came after the user had started reading); only while the user leafs it waits until
// the turning stopped. A failure or an open question nobody could take for 10 s (no document on the screen, a
// gesture, another request waiting) is dropped: a clean that long after its turn would look unprovoked.
// From the tick, under mu.
static void cd_fail_tick(void) {
    long f = __atomic_load_n(&cd_fail_id, __ATOMIC_ACQUIRE);
    if (cfg.codrive != 1 || !cfg.page_clean) { cd_pend_id = 0; if (f > cd_fail_seen) cd_fail_seen = f; return; }
    long nowms = (long)(now_s() * 1000.0);
    // answered (confirmed, or its failure was dealt with), or too old to belong to what is on the screen: nothing waits
    if (cd_pend_id && (__atomic_load_n(&cd_ok_id, __ATOMIC_ACQUIRE) >= cd_pend_id || cd_fail_seen >= cd_pend_id ||
                       nowms - cd_pend_ms > 10000)) cd_pend_id = 0;
    int fail = f > cd_fail_seen;
    if (fail && nowms - __atomic_load_n(&cd_fail_ms, __ATOMIC_RELAXED) > 10000) { cd_fail_seen = f; fail = 0; }
    int why = fail ? 1 : cd_pend_id && nowms - cd_pend_ms >= CD_PEND_MS ? 2 : 0;
    long c = __atomic_load_n(&cd_col_id, __ATOMIC_ACQUIRE);
    // a page request that waits, and that no full refresh has covered meanwhile (that one is dropped at its tick)
    // 1.8.26: not the second pass of a full STD turn (it asks no model: there is no threshold a mark could lower)
    int waits = auto_req && auto_kind == AK_PAGE && !auto_page_idle && t_stock_full < auto_t && t_our_full < auto_t;
    // 1.8.22: it takes the colour mark that comes with a later frame of the turn it was armed for (no new failure
    // comes with that frame: the turn has failed already)
    if (waits && !auto_page_col && auto_page_id && c == auto_page_id) {
        auto_page_col = 1;
        glog("the waiting page clean takes the colour mark: a later frame of its turn was left without co-drive for a colour");
    }
    if (!why) return;
    // 1.8.22: the turn that waits here was left without co-drive for a colour (cd_std 2), or a newer turn was (the
    // page on the screen is then that turn's) unless the helper confirmed that newer turn. Never an older turn's
    // mark: a marked turn that passed all the same (a GLR that needed nothing) must not lower another turn's threshold
    long id = why == 1 ? f : cd_pend_id;
    int col = c == id || (c > id && __atomic_load_n(&cd_ok_id, __ATOMIC_ACQUIRE) < c);
    // a page request that waits already (an earlier turn of this leafing) is for the page shown now: it takes the mark
    if (col && waits && !auto_page_col) {
        auto_page_col = 1;
        glog("the waiting page clean takes the colour mark: a turn left without co-drive for a colour came after it");
    }
    if (sleeping || auto_req || !sd_visible || sd_gesture || sd_mono) return; // it waits (10 s at most, see above)
    // the failure is dealt with; so is the open question: an answer that comes late for it is not a new failure
    if (why == 1) { cd_fail_seen = f; if (cd_pend_id <= f) cd_pend_id = 0; }
    else { if (cd_pend_id > cd_fail_seen) cd_fail_seen = cd_pend_id; cd_pend_id = 0; }
    // 1.8.22 (review round 4): the user opened a menu or popup, or wrote, after the newest turn (its hand-over or its
    // frame, whichever is later): no clean may land on that. This also holds for a turn whose request such an update
    // dropped while a newer turn's answer was still out or waited behind it
    double tt = (double)__atomic_load_n(&cd_turn_ms, __ATOMIC_RELAXED) / 1000.0;
    if (cd_turn_prev_t > tt) tt = cd_turn_prev_t;
    if (t_popup > tt || t_ink > tt) {
        n_auto_covered++;
        glog("page turn %s: %s after it, no clean",
             why == 1 ? "without its co-drive" : "whose co-drive the helper did not confirm",
             t_popup > tt ? "a menu or popup opened" : "ink was drawn");
        return;
    }
    arm_auto(AK_PAGE, why == 1 ? "page (no co-drive)" : "page (co-drive unconfirmed)");
    if (auto_req && auto_kind == AK_PAGE) {
        auto_quiet_need = cd_leaf ? cfg.page_settle : cfg.page_quiet;
        auto_page_col = col;
        auto_page_id = id;
    }
    glog("page turn %s: the model decides about a clean %s%s",
         why == 1 ? "without its co-drive" : "whose co-drive the helper did not confirm",
         cd_leaf ? "once the turning stopped" : "right after the page's frame",
         !col ? "" : cfg.turn_px_col < cfg.turn_px ? " (a colour page: turn_px_col)" : " (a colour page)");
}

// 1.8.22: the area (in pixels) of the box around the samples the newest frame changed (scan_rect's record)
static long long chg_box_area(void) {
    if (!chg_n) return 0;
    int x0 = PW, x1 = -1, y0 = PH, y1 = -1;
    for (size_t k = 0; k < chg_n; k++) {
        int px = (int)(chg_idx[k] % PW), py = (int)(chg_idx[k] / PW);
        if (px < x0) x0 = px;
        if (px > x1) x1 = px;
        if (py < y0) y0 = py;
        if (py > y1) y1 = py;
    }
    return (long long)(x1 - x0 + 1) * SUB * (long long)(y1 - y0 + 1) * SUB;
}

// A page turn: one partial content update over most of the screen while the document view is visible and no
// gesture runs (gestures are handled from the ScreenDriver's own decision).
static void page_track(void) {
    // the ScreenDriver state when the frame was sent (it is finalized later, at the next update line or tick)
    if (!cfg.page_clean || !sd_visible || sd_gesture || sd_mono || pend_sd_busy) return;
    // 1.8.22 (turn_full): a later frame of a turn whose own frame went out as the full STD: that refresh covers it
    // (it took no id: with no turn handed over yet it would count as "not asked" and arm the page clean)
    if (pend_turn_full) return;
    // the ScreenDriver's redraw that ends a gesture (most of the page as content) belongs to the gesture: it is either
    // promoted to a full STD (gesture_clean) or deliberately left alone; it is not a page turn
    if (sd_full_t > 0 && pend_t >= sd_full_t && pend_t - sd_full_t < 0.5) return;
    // only a page turn: the ScreenDriver logs "finalizeNewPage:" ~50 ms before its frame (measured on every page turn
    // in the journals of 8-9 Nov); other large content updates (undo, tiles, ...) never ask
    if (sd_newpage_t <= 0 || pend_t - sd_newpage_t > 0.6) return;
    if (pend_t < sd_newpage_t) {
        // 1.8.15: a turn's frame that is older than the newest "finalizeNewPage" (two turns within one tick) still
        // was a turn: the next one is leafing
        if (cfg.codrive == 1 && pend_cd_turn && pend_t > cd_turn_prev_t) cd_turn_prev_t = pend_t;
        return;
    }
    // an open (or a wake) draws its first page before xochitl's own in-place STD of it: that STD covers it
    if (sd_open_t > 0 && pend_t >= sd_open_t && pend_t - sd_open_t < 10.0 && t_stock_full < sd_open_t) return;
    if ((double)rects_area(pend_cls_rects[CL_CONTENT], pend_cls_n[CL_CONTENT]) < cfg.page_min * SW * SH) return;
    // 1.8.5 (see the header): counted; from the page_gap-th change on the model decides, the page_every-th cleans
    // anyway; while the user leafs, a due clean waits until the turning stopped
    // 1.8.8: with the co-drive a page turn needs no clean, unless the helper could not do its part since the last one
    // 1.8.13: ... if the helper confirms the turn's co-drive: its answer decides (cd_fail_tick)
    // 1.8.26: a second pass still waits (the user turned on at once) and this turn got no full STD (its page holds no
    // real colour, the pen was near, another frame took the byte): the page that pass was for is gone, and this turn
    // is the flash-free one of 1.8.25, which gets no flash after it. Without page_given_up: this turn's own verdict
    // still counts (cd_fail_tick).
    if (auto_req && auto_kind == AK_PAGE && auto_page_idle) {
        auto_req = 0;
        n_auto_covered++;
        glog("auto %s: the user turned on with a turn that got no full STD: no second pass", auto_why);
    }
    if (cfg.codrive == 1) {
        long id = pend_cd_turn; // 1.8.14: this frame's own id (it was the last id given within 3 s: a turn that was
                                // not handed over took the answer to the turn before it)
        if (id) {
            if (id != cd_track_id) { // 1.8.19: a later frame of the same turn (its pending tiles) is not another turn
                cd_leaf = cd_turn_prev_t > 0 && pend_t >= cd_turn_prev_t && pend_t - cd_turn_prev_t < cfg.page_flip;
                cd_turn_prev_t = pend_t;
                cd_track_id = id;
            }
            if (id != cd_pend_id && cd_fail_seen < id) { cd_pend_id = id; cd_pend_ms = (long)(now_s() * 1000.0); }
            // 1.8.14: a clean that waits for an earlier turn of this leafing belongs to the page shown now: it must not
            // age out (AUTO_MAX_AGE) while the user turns on, its failure is marked as dealt with
            // 1.8.15: not a request a full refresh has covered meanwhile (it is dropped at its tick); and only while
            // the user leafs does it wait for the turning to stop, a single later turn keeps the short quiet time
            // 1.8.22: the request keeps its colour mark (auto_page_col) over the turns that follow: see turn_px_col
            if (auto_req && auto_kind == AK_PAGE && t_stock_full < auto_t && t_our_full < auto_t) {
                auto_t = now_s();
                auto_quiet_need = cd_leaf ? cfg.page_settle : cfg.page_quiet;
            }
            return;
        }
        glog("page turn the co-drive helper was not asked about: the page clean decides");
    }
    int leafing = 0;
    int due = page_due(pend_t, 1, &leafing);
    if (!due) {
        glog("page turn %d since the last full refresh (the first clean may come at %d), no clean", n_pchg, cfg.page_gap);
        return;
    }
    arm_auto(AK_PAGE, leafing ? "page (leafing)" : "page"); // a waiting gesture or keyboard request is kept
    if (auto_req && auto_kind == AK_PAGE) {
        auto_page_cap = due == 2;
        auto_quiet_need = leafing ? cfg.page_settle : cfg.page_quiet; // 1.8.6: right after the page's frame
    }
    glog("page turn %d since the last full refresh: %s%s", n_pchg,
         due == 2 ? "the cap, cleans whatever the model says" : "the model decides once the page settled",
         leafing ? "; the user leafs: the clean waits until the turning stopped" : "");
}

// The user opened something over the screen after a request was made (a menu, a popup, the document toolbar): one
// partial update of ui class over >= 5% of the screen and not the whole screen, with no pen, mono, animation or
// overlay area and almost no content (< 1%: icons). A waiting request is dropped: no flash may land on a menu
// (review round 2: a request the pen held went off on the side menu opened meanwhile). Checked before the
// triggers, so the update that makes a request never drops it.
static void popup_track(void) {
    long long ui = rects_area(pend_cls_rects[CL_UI], pend_cls_n[CL_UI]);
    if (ui < (long long)SW * SH / 20 || ui >= (long long)SW * SH * 95 / 100) return;
    if (pend_cls_n[CL_PEN] || pend_cls_n[CL_MONO] || pend_cls_n[CL_ANIM] || pend_cls_n[CL_OVERLAY]) return;
    if (rects_area(pend_cls_rects[CL_CONTENT], pend_cls_n[CL_CONTENT]) >= (long long)SW * SH / 100) return;
    // 1.8.22: remembered also with no request waiting: a page turn's clean is not armed after it (cd_fail_tick)
    // (review round 5: a frame that took a turn's id itself was handed over after pend_t was taken: it counts as
    // after that turn)
    double tp = pend_cd_turn ? now_s() : pend_t;
    if (tp > t_popup) t_popup = tp;
    if (!auto_req || pend_t <= auto_t) return;
    auto_req = 0;
    n_auto_covered++;
    n_popup++;
    glog("auto %s: a menu or popup opened over the screen (ui %lld px), no clean", auto_why, ui);
}

static int minor_ui_update(void);

static void kbo_drop(const char *why, double t) {
    kbo_req = 0;
    n_kbo_drop++;
    glog("keyboard open: %s %.0f ms after the keyboard was drawn, no clean", why, (t - kbo_t) * 1000.0);
}

// 1.8.1: a partial update while the keyboard band waits for its clean. Only the text cursor may come before it: a
// minor ui update (minor_ui_update) that does not touch the band. Anything else drops the request: a key press
// (Overlay or A2 key, or the text it types), the keyboard redrawn, a popup, ink, the dialog closing. Called before
// kb_track, so the keyboard's own frame never drops the request it makes.
static void kbo_track(void) {
    if (!kbo_req) return;
    if (!minor_ui_update()) {
        int key = 0;
        for (int c = CL_PEN; c < CL_N; c++) key |= pend_cls_n[c] > 0;
        kbo_drop(key || !pend_cls_n[CL_UI] ? "a key press or ink came first" : "the screen changed first", pend_t);
        return;
    }
    for (int i = 0; i < pend_nrects; i++) {
        Rect r = pend_rects[i];
        if (!clip_rect(&r)) continue;
        if (r.x < kbo_box.x + kbo_box.w && kbo_box.x < r.x + r.w && r.y < kbo_box.y + kbo_box.h && kbo_box.y < r.y + r.h) {
            kbo_drop("something was drawn on the keyboard first", pend_t);
            return;
        }
    }
}

static void partial_triggers(void) {
    popup_track();
    kbo_track();
    kb_track();
    page_track();
}

// A frame drawn with the Sleep screen mode. xochitl logs " - sleep" only on its flagged path (the string's one
// reference, 0x86b624, is reached only with flag bit 0 set, 0x86aa68), so this frame always had its full refresh.
static void sleep_frame_seen(int flags) {
    n_sleep_frame++;
    kb_on = 0;
    if (kbo_req) kbo_drop("the sleep screen came", pend_t);
    if (!(flags & 1)) glog("WARNING: sleep frame logged without the full flag (unexpected for 3.29.1)");
}

// A minor update: ui class only and under 1% of the screen in all. After a document closes, sync icons redraw every
// 0.3-0.6 s for seconds: the status-bar indicator (QRegion(size=18, bounds=(1394,44 178x32))) and the closed
// document's entry in the library list (bounds=(34,686 278x37), journal 2026-11-08 21:21). Such an update is not part
// of a view's drawing: it neither delays a view change's clean nor counts as the new view's first frame (review round
// 2, liveness-3: these icons kept the clean of a document close waiting until the side menu or the "+" menu the user
// opened next was on the screen, and the clean flashed over it). Menus, popups and dialogs are larger (the smallest
// seen, the "+" menu, is 6%); key presses and the text cursor are drawn with other classes.
static int minor_ui_update(void) {
    if (pend_merged || !pend_cls_n[CL_UI] || pend_nrects <= 0) return 0;
    for (int c = 0; c < CL_N; c++)
        if (c != CL_UI && pend_cls_n[c]) return 0;
    long long a = 0;
    for (int i = 0; i < pend_nrects; i++) {
        Rect r = pend_rects[i];
        if (clip_rect(&r)) a += (long long)r.w * r.h;
    }
    return a < (long long)SW * SH / 100;
}

// apply the assembled update to the model
static void finalize_pending(void) {
    if (!pend_active) return;
    pend_active = 0;
    if (pend_complete || (pend_flags & 1)) {
        // A complete refresh erases the residue of the samples it drives in place (value unchanged).
        // Samples it changes go straight from the old to the new value and keep a trace of the old image.
        long long area = 0;
        for (int i = 0; i < pend_nrects; i++) area += (long long)pend_rects[i].w * pend_rects[i].h;
        if (cfg.debug) debug_complete(pend_complete == 2 ? "sleep" : "std", area);
        tr_on_complete(area);
        // 1.8.1: a stock complete refresh (STD path) after the keyboard frame: never a second flash after it
        if (kbo_req && pend_complete != 2) kbo_drop("a stock flash came first", pend_t);
        for (size_t k = 0; k < chg_n; k++) chg_mark[chg_idx[k]] = 1;
        for (int i = 0; i < pend_nrects; i++) clear_residue_rect_unchanged(pend_rects[i]);
        float ws = (float)cfg.w_std, bs = (float)cfg.beta;
        for (size_t k = 0; k < chg_n; k++) {
            uint32_t s = chg_idx[k];
            chg_mark[s] = 0;
            int d = chg_d[k];
            float add = d > 0 ? (float)d * ws : (float)(-d) * bs * ws;
            unsigned v = res[s] + (unsigned)(add + 0.5f);
            res[s] = (uint16_t)(v > 65535 ? 65535 : v);
        }
        n_std_chg += chg_n;
        if (area >= (long long)SW * SH * 95 / 100) {
            n_full++;
            t_last_full = pend_t;
            t_stock_full = pend_t; // our own cleans never come through here (see model_clean_rect)
            kb_on = 0;             // the keyboard area (if any) was just redrawn by the stock flash
            page_reset();          // 1.8.5: the page count starts again (an open, a zoom, a promoted redraw, ...)
        } else {
            n_regional++;
        }
        if (pend_complete == 2) sleep_frame_seen(pend_flags);
        chg_n = 0;
        memset(touched, 0, sizeof touched);
        n_complete++;
        if (pend_complete == 2) n_sleep++; else n_std++;
        t_last_complete = pend_t;
        t_last_major = pend_t;
        return;
    }
    n_partial++;
    if (minor_ui_update()) n_minor_upd++;
    else t_last_major = pend_t;
    // weight per touched cell from the update class that covers it
    static float cw[NCELL];
    static uint8_t ccl[NCELL];
    for (int c = 0; c < NCELL; c++) {
        if (!touched[c]) continue;
        int cls = CL_UI;
        if (pend_cls_n[CL_PEN] && cell_in_rects(c, pend_cls_rects[CL_PEN], pend_cls_n[CL_PEN])) cls = CL_PEN;
        else if (pend_cls_n[CL_CONTENT] && cell_in_rects(c, pend_cls_rects[CL_CONTENT], pend_cls_n[CL_CONTENT])) cls = CL_CONTENT;
        else if (pend_cls_n[CL_MONO] && cell_in_rects(c, pend_cls_rects[CL_MONO], pend_cls_n[CL_MONO])) cls = CL_MONO;
        else if (pend_cls_n[CL_ANIM] && cell_in_rects(c, pend_cls_rects[CL_ANIM], pend_cls_n[CL_ANIM])) cls = CL_ANIM;
        else if (pend_cls_n[CL_OVERLAY] && cell_in_rects(c, pend_cls_rects[CL_OVERLAY], pend_cls_n[CL_OVERLAY])) cls = CL_OVERLAY;
        if (pend_merged && cls == CL_UI) cls = CL_CONTENT;
        cw[c] = (float)cfg.w[cls];
        ccl[c] = (uint8_t)cls;
        touched[c] = 0;
    }
    float beta = (float)cfg.beta;
    for (size_t k = 0; k < chg_n; k++) {
        uint32_t s = chg_idx[k];
        int px = (int)(s % PW), py = (int)(s / PW);
        int c = CELL_OF(px, py);
        float w = cw[c];
        chg_cls[ccl[c]]++;
        int d = chg_d[k];
        float add = d > 0 ? (float)d * w : (float)(-d) * beta * w;
        unsigned v = res[s] + (unsigned)(add + 0.5f);
        res[s] = (uint16_t)(v > 65535 ? 65535 : v);
    }
    chg_n = 0;
    partial_triggers();
}

// A regional clean we issued ourselves. Its update lines are logged while our handler is running, and
// Qt prints such nested messages straight to stderr without calling the installed handlers, so they
// never reach on_update_line(): apply the clean to the model here.
static void model_clean_rect(Rect r) {
    finalize_pending();
    if (!clip_rect(&r)) return;
    double t = now_s();
    if ((long long)r.w * r.h >= (long long)SW * SH * 95 / 100) {
        t_our_full = t;
        page_reset(); // 1.8.5
        // 1.8.15: a clean with TEXT targets leaves no STD-family level behind: xochitl draws the next turn as a GLR
        if (__atomic_load_n(&last_clean_text, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&cd_std_next, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&cd_text_next, 1, __ATOMIC_RELEASE);
        }
    }
    if (fb) {
        ensure_prev();
        if (prev_valid) scan_rect(r, 1, t);
    }
    if (res) clear_residue_rect(r);
    n_regional++;
    n_complete++;
    n_std++;
    t_last_complete = t;
}

// ---------------------------------------------------------------- QRegion text parsing
// Formats:  QRegion(0,0 1620x2160)
//           QRegion(size=3, bounds=(543,726 821x29) - [(543,726 93x23), (1308,726 56x23), (543,749 93x6)])
static int parse_region(const char *s, Rect *out, int max) {
    const char *q = strstr(s, "QRegion(");
    if (!q) return 0;
    q += 8;
    int n = 0;
    const char *lst = strstr(q, "- [");
    if (strncmp(q, "size=", 5) == 0 && lst) {
        const char *p = lst + 3;
        while (n < max) {
            const char *o = strchr(p, '(');
            if (!o) break;
            Rect r;
            if (sscanf(o + 1, "%d,%d %dx%d", &r.x, &r.y, &r.w, &r.h) == 4) out[n++] = r;
            p = o + 1;
        }
        if (n == max) { // too many: fall back to bounds
            const char *b = strstr(q, "bounds=(");
            Rect r;
            if (b && sscanf(b + 8, "%d,%d %dx%d", &r.x, &r.y, &r.w, &r.h) == 4) { out[0] = r; n = 1; }
        }
        return n;
    }
    Rect r;
    if (sscanf(q, "%d,%d %dx%d", &r.x, &r.y, &r.w, &r.h) == 4) { out[0] = r; return 1; }
    return 0;
}

static int fb_refresh(void);

// 1.8.29, at a "swapBuffers:" line (GUI thread, inside swapBuffers with the frame's lock held, before xochitl
// converts the frame): while the page is held back, whatever xochitl has drawn since moves behind the sentinels,
// so the panel keeps its white. A complete refresh, or the way to sleep, gets the true frame back first; a
// complete refresh of the whole screen is the page's arrival.
static int wh_map_sleep(void); // the screen-mode map holds a Sleep part (defined with the panel's helpers)
static void wh_swap_hook(const char *m, int flags) {
    if (!__atomic_load_n(&wh_owed, __ATOMIC_ACQUIRE)) return;
    char fs[40];
    snprintf(fs, sizeof fs, "QRegion(0,0 %dx%d)", SW, SH);
    int full = (flags & 1) && strstr(m, fs) != NULL;
    double ms = (now_s() - wh_t0) * 1000.0;
    if (__atomic_load_n(&wh_active, __ATOMIC_ACQUIRE)) {
        if (!(flags & 1) && !__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE) && !wh_map_sleep()) {
            if (fb && wh_shadow) wh_cover(0);
            n_wh_absorb++;
            return;
        }
        wh_unhide();
        n_wh_early++;
        if (!full) {
            glog("white intermediate: %s %.0f ms after the turn: the frame is the page again, its full-screen refresh follows",
                 (flags & 1) ? "a complete refresh of a part" : "the way to sleep", ms);
            return;
        }
    } else if (!full) return;
    __atomic_store_n(&wh_owed, 0, __ATOMIC_RELEASE);
    n_wh_stock++;
    glog("white intermediate: a complete full-screen refresh of xochitl's own brought the page %.0f ms after the turn", ms);
}

// A frame while a view change waits for its clean: collect what it redraws, and note a stock full refresh
// that went out as the first frame of the newest view anyway (the helper thread lost the race). A frame
// that comes TR_FRAME_MAX after a view change that drew nothing belongs to something else: that view
// change changed nothing on screen, so it needs no clean, and the cycle ends here.
static void tr_collect(const char *m, int flags, int after_vc) {
    if ((tr_box.w <= 0 || tr_box.h <= 0) && now_s() - tr_vc_last > TR_FRAME_MAX) {
        tr_active = 0;
        n_tr_aged++;
        glog("view change %s drew nothing within %.0f s: no clean", tr_what, TR_FRAME_MAX);
        return;
    }
    Rect rs[MAXRECTS];
    int k = parse_region(m, rs, MAXRECTS);
    for (int i = 0; i < k; i++) rect_union(&tr_box, rs[i]);
    if ((flags & 1) && !after_vc && !tr_late) {
        tr_late = 1;
        n_wd_late++;
    }
}

static void on_update_line(const char *m) {
    // m is the message text of an rm.framebuffer.updates line
    if (strncmp(m, "swapBuffers:", 12) == 0) {
        if (promote_live && tr_active && tr_promoted) { // this frame took our promoted byte: its full STD is the clean
            tr_active = 0;
            n_tr_absorbed++;
            glog("view change %s: the ScreenDriver's full STD after the gesture stood in for the clean", tr_what);
        }
        int took = promote_live;
        promote_live = 0; // this frame took a promoted BlinkLater byte, if one was set (GUI thread: no frame in flight)
        tr_promoted = 0;
        finalize_pending();
        int flags = 0;
        const char *f = strstr(m, "QFlags(");
        if (f && f[7] == '0') flags = (int)strtol(f + 7, NULL, 16);
        // 1.8.22 (turn_full): the frame that took the byte a page turn set, logged as the complete refresh it became
        int tfull = took && (flags & 1) && turn_full_np && turn_full_np == sd_newpage_n;
        turn_full_np = 0;
        // 1.8.25: a complete refresh must read the stock flip rows (this line comes before xochitl queues the update)
        cdf_line(flags & 1);
        int after_vc = t_last_update > tr_vc_last; // the newest view change has drawn a frame already
        Rect box_before = tr_box;
        if (tr_active) tr_collect(m, flags, after_vc);
        if (!fb_refresh()) return;
        wh_swap_hook(m, flags); // 1.8.29: before anything reads the frame, and before xochitl converts it
        double t0 = now_s();
        ensure_prev();
        if (!prev_valid) return;
        int n = parse_region(m, pend_rects, MAXRECTS);
        pend_nrects = n;
        pend_active = 1;
        pend_flags = flags;
        pend_complete = 0;
        pend_merged = 0;
        pend_tr = tr_active;
        pend_vc_t = tr_vc_last;
        pend_after_vc = after_vc;
        pend_tr_box = box_before;
        pend_t = t0;
        pend_sd_busy = sd_gesture || sd_mono;
        pend_cd_turn = 0;
        pend_turn_full = 0;
        chg_n = 0;
        memset(pend_cls_n, 0, sizeof pend_cls_n);
        // complete refreshes record their changes too: changed samples keep residue (see finalize_pending)
        cd_up = cd_dn = 0;
        for (int i = 0; i < n; i++) scan_rect(pend_rects[i], 0, t0);
        if (cfg.codrive && n > 0) { // 1.8.8: a large update with unequal erase / draw parts: ask the co-drive helper
            Rect bb = {0, 0, 0, 0};
            for (int i = 0; i < n; i++) rect_union(&bb, pend_rects[i]);
            double fe = (double)cd_up / (255.0 * (double)NS), fd = (double)cd_dn / (255.0 * (double)NS);
            if ((long long)bb.w * bb.h >= cfg.cd_area) {
                cd_lfe = fe; cd_lfd = fd;
                // the most a frame's mean drive can be; the helper decides from the table's rows (cd_arm)
                double pk = 24.0 * (fe * cfg.cd_gain > fd * cfg.cd_gpos ? fe * cfg.cd_gain : fd * cfg.cd_gpos);
                // a page turn (see page_track; 1.8.10: with its view of the ScreenDriver: no gesture, no pen-only mode)
                int turn = sd_visible && !sd_gesture && !sd_mono && sd_newpage_t > 0 && t0 >= sd_newpage_t && t0 - sd_newpage_t <= 0.6;
                // 1.8.19: a later frame of a turn that has its id (xochitl draws the page's pending tiles as further
                // frames 0.2-0.5 s after the turn's own): the same turn, one id, one verdict
                if (turn && cd_np_n == sd_newpage_n) turn = 2;
                if (turn == 2 && turn_full_done_n == sd_newpage_n) pend_turn_full = 1;
                if (tfull && turn == 1) {
                    // 1.8.22 (turn_full): the turn's frame is the full STD itself. Nothing is handed to the helper (no
                    // cells, no companions, no id, so no verdict and no clean after it); its later frames are later
                    // frames of the newest id, which the refresh covers (page_reset, when this frame is finalized).
                    // A frame that changes nothing, or only a small spot (review: a status mark, the page number's
                    // box), is not the turn's (something else drew first and took the byte: an in-place flash of the
                    // old page); the turn's own frame then goes the usual way and gets its clean.
                    if (chg_box_area() >= cfg.cd_area) {
                        cd_np_n = turn_full_done_n = sd_newpage_n;
                        n_turn_full_took++;
                        wts_kick(); // 1.8.30: the STD table this turn runs on gets the white tail (or is checked for it)
                        if (cfg.turn_full >= 2) {
                            // 1.8.26: one full STD that changes the page leaves a faint trace of the old page's black
                            // text; one in-place full pass right after it takes that away (see the header). Leafing:
                            // the turn before this one was less than page_flip ago
                            int leaf = sd_newpage_prev_t > 0 && sd_newpage_t - sd_newpage_prev_t < cfg.page_flip;
                            // 1.8.29 (turn_full 3): the full STD goes out white, the page follows as the next complete
                            // refresh (wh_hide refuses when the clock is not alive or a check failed: the double pass)
                            if (cfg.turn_full >= 3 && wh_hide(t0))
                                glog("page turn: its frame goes out white as the full STD (turn_full 3): no co-drive, the page follows as the next complete refresh");
                            else if (arm_second_pass(t0, leaf))
                                glog("page turn: its frame went out as the full STD (turn_full 2): no co-drive, the second pass follows %s",
                                     leaf ? "once the turning stopped" : "when the panel is idle");
                            else
                                glog("page turn: its frame went out as the full STD (turn_full 2): no co-drive, no second pass (another request waits)");
                        } else
                            glog("page turn: its frame went out as the full STD (turn_full): no co-drive, no clean after it");
                    } else {
                        glog("page turn: another frame took the turn's full STD before the page was drawn (turn_full)");
                    }
                } else
                // 1.8.13: every page turn is handed over (codrive 1): the helper sees which waveform xochitl took for it
                // 1.8.29: not for a frame the white intermediate holds back (the panel gets white on white: no drive)
                if ((pk >= cfg.cd_minv || (turn && cfg.codrive == 1)) && !__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE) &&
                    !__atomic_load_n(&wh_active, __ATOMIC_ACQUIRE)) {
                    n_cd[CD_REQ]++;
                    long was = __atomic_load_n(&cd_turn_id, __ATOMIC_ACQUIRE);
                    cd_request(fe, fd, bb, turn, flags & 1);
                    long is =__atomic_load_n(&cd_turn_id, __ATOMIC_ACQUIRE);
                    if (is != was) { pend_cd_turn = is; cd_np_n = sd_newpage_n; }
                    else if (turn == 2) pend_cd_turn = is;
                }
            }
        }
        n_updates++;
        t_last_update = now_s();
        double ms = (t_last_update - t0) * 1000.0;
        proc_ms_total += ms;
        if (ms > proc_ms_max) proc_ms_max = ms;
        return;
    }
    if (!pend_active) return;
    if (strstr(m, " - full update")) { pend_complete = 1; return; }
    if (strncmp(m, " - sleep", 8) == 0) { pend_complete = 2; return; }
    if (strstr(m, "merged UI/Content")) { pend_merged = 1; return; }
    int cls = -1;
    if (strncmp(m, " - ui ", 6) == 0) cls = CL_UI;
    else if (strncmp(m, " - content", 10) == 0) cls = CL_CONTENT;
    else if (strncmp(m, " - pen", 6) == 0) {
        cls = CL_PEN;
        // ink (" - pen .......", "(clamp)", "(color)", "(ghost)", "mclamp", "mdither"), not a pen tap on the UI
        if (strncmp(m, " - pen (ui)", 11) != 0) {
            if (tr_active) tr_inked = 1;
            if (auto_req) auto_inked = 1; // the user writes: the request is dropped (auto_try)
            t_ink = now_s();              // 1.8.22: ... and no page turn's clean is armed after it (cd_fail_tick)
        }
    }
    else if (strncmp(m, " - mono", 7) == 0) cls = CL_MONO;
    else if (strncmp(m, " - animation", 12) == 0 || strstr(m, "partial animation") || strstr(m, "full screen animation")) cls = CL_ANIM;
    else if (strncmp(m, " - overlay", 10) == 0) cls = CL_OVERLAY;
    if (cls < 0) return;
    int k = pend_cls_n[cls];
    if (k < MAXRECTS) pend_cls_n[cls] += parse_region(m, pend_cls_rects[cls] + k, MAXRECTS - k);
}

// ---------------------------------------------------------------- status / decisions
typedef struct {
    long ghost;         // ghost samples in quiet cells
    long ghost_active;  // ghost samples in cells that are still changing (ignored)
    int hot, maxcell, x0, y0, x1, y1;
    double light;       // mean luma (0..1) of the counted ghost samples
} Score;

static uint16_t gcount[NCELL];

// quiet: cells whose content changed less than this many seconds ago are "active" (still changing), not ghosts
static Score compute_score_q(double quiet) {
    Score s = {0, 0, 0, 0, SW, SH, 0, 0, 0};
    memset(gcount, 0, sizeof gcount);
    if (!prev_valid || !res) return s;
    static uint32_t glight[NCELL];
    memset(glight, 0, sizeof glight);
    unsigned thr = (unsigned)(cfg.tau * 255.0 + 0.5);
    int lmin = cfg.light_min;
    for (int py = 0; py < PH; py++) {
        const uint16_t *rr = res + (size_t)py * PW;
        const uint8_t *pr = prev + (size_t)py * PW;
        int cy = ((py * SUB) / CELL) * GW;
        for (int px = 0; px < PW; px++) {
            if (rr[px] >= thr && pr[px] >= lmin) {
                int c = cy + (px * SUB) / CELL;
                gcount[c]++;
                glight[c] += pr[px];
            }
        }
    }
    double now = now_s();
    double lsum = 0;
    for (int c = 0; c < NCELL; c++) {
        if (!gcount[c]) continue;
        if (cell_touch[c] > 0 && now - cell_touch[c] < quiet) {
            s.ghost_active += gcount[c];
            continue;
        }
        s.ghost += gcount[c];
        lsum += glight[c];
        if (gcount[c] > s.maxcell) s.maxcell = gcount[c];
        if (gcount[c] >= cfg.cell_min) {
            s.hot++;
            int cx = c % GW, cy = c / GW;
            int x0 = cx * CELL, y0 = cy * CELL;
            int x1 = x0 + CELL > SW ? SW : x0 + CELL, y1 = y0 + CELL > SH ? SH : y0 + CELL;
            if (x0 < s.x0) s.x0 = x0;
            if (y0 < s.y0) s.y0 = y0;
            if (x1 > s.x1) s.x1 = x1;
            if (y1 > s.y1) s.y1 = y1;
        }
    }
    if (s.ghost) s.light = lsum / (s.ghost * 255.0);
    return s;
}

static Score compute_score(void) { return compute_score_q(cfg.quiet_s); }

static const char *rec_of(const Score *s) {
    if (s->hot < cfg.min_cells) return "none";
    if (s->ghost >= cfg.now_px) return "now";
    if (s->ghost >= cfg.soon_px) return "soon";
    return "none";
}

// 1.8.5 (see the header): does the model see enough residue for a page clean that is due by the count (not by the
// cap)? Its own threshold: soon_px/now_px gate the QML requests and stay as they were. 1.8.6: px is turn_px for a page
// turn, page_px for a gesture's redraw.
static int page_model_ok(const Score *s, int px) { return s->hot >= cfg.min_cells && s->ghost >= px; }

// calibration aid: what the model held just before a complete refresh (stock or ours) cleared it
static void debug_complete(const char *kind, long long area) {
    Score s = compute_score();
    glog("complete %s %.0f%%: before ghost=%ld active=%ld hot=%d maxCell=%d rec=%s", kind,
         100.0 * (double)area / ((double)SW * SH), s.ghost, s.ghost_active, s.hot, s.maxcell, rec_of(&s));
}

static int write_file(const char *path, const char *data, size_t len) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, len);
    close(fd);
    if (w != (ssize_t)len) return -1;
    return rename(tmp, path);
}

static int xo_state; // regional clean capability: 0 not checked yet, 1 verified, -1 unsupported
static char xo_why[96] = "";
static int swtcon_idle(void); // 1.8.3 (see there)
static double xo_temp(void);

static void write_status(const char *qid, const char *rc) {
    finalize_pending();
    Score s = compute_score();
    const char *rec = rec_of(&s);
    const char *scope = "full";
    if (s.hot) {
        double bbox = (double)(s.x1 - s.x0) * (s.y1 - s.y0) / ((double)SW * SH);
        if (bbox <= cfg.regional_max) scope = "regional";
    }
    double now = now_s();
    char tbuf[16] = "null"; // the panel temperature (1.8.3), JSON null when unknown
    double tc = xo_temp();
    if (!isnan(tc)) snprintf(tbuf, sizeof tbuf, "%.1f", tc);
    char buf[3584];
    int n = snprintf(buf, sizeof buf,
                     "{\"q\":\"%s\",\"ok\":%d,\"rec\":\"%s\",\"scope\":\"%s\",\"ghost\":%ld,\"active\":%ld,\"hot\":%d,"
                     "\"maxCell\":%d,\"bbox\":[%d,%d,%d,%d],\"light\":%.2f,\"updates\":%lu,\"partial\":%lu,"
                     "\"complete\":%lu,\"std\":%lu,\"sleep\":%lu,\"full\":%lu,\"regional\":%lu,"
                     "\"sinceUpdate\":%.2f,\"sinceClean\":%.1f,\"sinceFull\":%.1f,"
                     "\"rcap\":%d,\"rc\":\"%s\",\"rcOk\":%lu,\"rcFail\":%lu,\"sim\":%lu,\"why\":\"%s\","
                     "\"chg\":[%lu,%lu,%lu,%lu,%lu,%lu],\"stdChg\":%lu,"
                     "\"defer\":%d,\"tr\":%lu,\"wd\":%lu,\"wdMiss\":%lu,\"wdLate\":%lu,\"trClean\":%lu,\"trFail\":%lu,"
                     "\"trAbsorbed\":%lu,\"trAged\":%lu,\"trActive\":%d,"
                     "\"auto\":[%lu,%lu,%lu,%lu,%lu],\"autoReq\":%d,\"cleanWf\":%d,\"wfCleans\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],"
                     "\"v18\":{\"on\":[%d,%d,%d,%d],\"sd\":[%d,%d,%d,%d],\"sleeping\":%d,\"kb\":%d,\"kind\":\"%s\","
                     "\"sdClose\":%lu,\"sdSkip\":%lu,\"sleepBack\":%lu,\"sleepFrames\":%lu,\"wake\":%lu,"
                     "\"arm\":[%lu,%lu,%lu,%lu],\"drop\":[%lu,%lu,%lu],\"promote\":[%lu,%lu,%lu],\"popup\":%lu,\"minorUpd\":%lu,\"pen\":%d,"
                     "\"gmin\":%.3f},\"kbOpen\":[%d,%lu,%lu,%lu,%lu,%d],"
                     "\"sw\":{\"now\":%d,\"last\":%d,\"inflight\":%lu,\"upgrade\":%lu,\"fullBusy\":%lu,\"waiting\":%.2f},\"temp\":%s,"
                     "\"page\":[%d,%d,%d,%d,%lu,%lu,%lu,%d,%d],"
                     "\"cd\":[%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%.4f,%.4f,%lu,%lu,%lu],"
                     "\"wh\":[%d,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%ld,%ld],"
                     "\"procAvgMs\":%.2f,\"procMaxMs\":%.2f,\"fb\":%d,\"v\":\"%s\"}",
                     qid, fb && prev_valid ? 1 : 0, rec, scope, s.ghost, s.ghost_active, s.hot, s.maxcell,
                     s.hot ? s.x0 : 0, s.hot ? s.y0 : 0, s.hot ? s.x1 - s.x0 : 0, s.hot ? s.y1 - s.y0 : 0, s.light,
                     n_updates, n_partial, n_complete, n_std, n_sleep, n_full, n_regional,
                     t_last_update > 0 ? now - t_last_update : -1.0,
                     t_last_complete > 0 ? now - t_last_complete : -1.0, t_last_full > 0 ? now - t_last_full : -1.0,
                     xo_state, rc ? rc : "", n_rclean_ok, n_rclean_fail, n_sim, xo_why,
                     chg_cls[CL_UI], chg_cls[CL_CONTENT], chg_cls[CL_PEN], chg_cls[CL_MONO], chg_cls[CL_ANIM],
                     chg_cls[CL_OVERLAY], n_std_chg,
                     cfg.defer, n_tr, n_wd, n_wd_miss, n_wd_late, n_tr_clean, n_tr_fail, n_tr_absorbed, n_tr_aged,
                     tr_active, n_auto, n_auto_clean, n_auto_none, n_auto_aged, n_auto_fail, auto_req, cfg.clean_wf,
                     n_clean_wf[0], n_clean_wf[1], n_clean_wf[2], n_clean_wf[3], n_clean_wf[4], n_clean_wf[5],
                     n_clean_wf[6], n_clean_wf[7],
                     cfg.sd_close, cfg.gesture_clean, cfg.page_clean, cfg.kb_clean, sd_visible, sd_gesture, sd_mono, sd_tiles,
                     sleeping, kb_on, auto_req ? ak_name[auto_kind] : "", n_sd_close, n_sd_skip, n_sleep_back,
                     n_sleep_frame, n_wake, n_arm[AK_PAGE], n_arm[AK_GESTURE], n_arm[AK_KB], n_arm[AK_QML], n_auto_stock,
                     n_auto_ink, n_auto_covered, n_promote, n_promote_skip, n_unpromote, n_popup, n_minor_upd, sd_pen, cfg.gesture_min,
                     cfg.kb_open_clean, n_kbo, n_kbo_clean, n_kbo_drop, n_kbo_fail, kbo_req,
                     swtcon_idle(), __atomic_load_n(&last_sw, __ATOMIC_RELAXED), n_inflight, n_upgrade, n_full_busy,
                     tr_active && tr_wait_t > 0 ? now - tr_wait_t : 0.0, tbuf,
                     n_pchg, cfg.page_gap, cfg.page_every, cfg.page_px, n_page_wait, n_page_flip, n_page_none,
                     cfg.turn_px, (int)(cfg.page_quiet * 1000.0 + 0.5),
                     cfg.codrive, n_cd[CD_REQ], n_cd[CD_ARM], n_cd[CD_MISS], n_cd[CD_STD], n_cd[CD_SMALL], n_cd[CD_BAD],
                     n_cd[CD_COMP], n_cd[CD_COMPFAIL], n_cd[CD_ABORT], __atomic_load_n(&cd_last, __ATOMIC_RELAXED), cd_lfe, cd_lfd, n_cd[CD_PRE], n_cd[CD_EARLY], n_cd[CD_STDCD],
                     __atomic_load_n(&wh_active, __ATOMIC_RELAXED), __atomic_load_n(&wh_owed, __ATOMIC_RELAXED), wh_off,
                     n_wh_hide, n_wh_absorb, n_wh_reveal, n_wh_stock, n_wh_early, n_wh_wd, n_wh_bad, n_wh_no, wh_chk_n, wh_chk_bad,
                     n_updates ? proc_ms_total / n_updates : 0.0, proc_ms_max, fb ? 1 : 0, GG_VERSION);
    if (n > 0 && n < (int)sizeof buf) write_file(RUNDIR "/status.json", buf, (size_t)n);
}

// ghost samples per cell as a PGM (GW x GH, 255 = all 64 samples of the cell), for inspection
static void write_map(void) {
    finalize_pending();
    compute_score();
    static char out[64 + NCELL];
    int h = snprintf(out, 64, "P5\n%d %d\n255\n", GW, GH);
    for (int c = 0; c < NCELL; c++) {
        int p = gcount[c] * 4;
        out[h + c] = (char)(p > 255 ? 255 : p);
    }
    write_file(RUNDIR "/ghostmap.pgm", out, (size_t)h + NCELL);
}

static int restore_pending(void);

// Turning defer off (or on) while a view change waits for its clean: a withdrawn stock flash whose newest
// view has not drawn its first frame yet goes back to xochitl (that frame then gets it, exactly as in stock);
// once the view has drawn, giving it back would turn the next unrelated update into a full flash, so
// the cycle just ends. Called with mu held, from the GUI thread.
static void set_defer(int on) {
    on = on ? 1 : 0;
    if (on == cfg.defer) return;
    cfg.defer = on;
    if (!tr_active) return;
    int back = tr_withdrawn && t_last_update <= tr_vc_last ? restore_pending() : 0;
    tr_active = 0;
    glog("defer %d during view change %s: cycle ended%s", on, tr_what, back > 0 ? ", stock flash given back" : "");
}

static int clamp_i(double d, int lo, int hi) { return d < lo ? lo : d > hi ? hi : (int)d; }
static double clamp_d(double d, double lo, double hi) { return d < lo ? lo : d > hi ? hi : d; }
static int clean_wf_ok(int wf);

static void set_param(const char *k, const char *v) {
    double d = atof(v);
    if (isnan(d)) return;
    if (!strcmp(k, "beta")) cfg.beta = clamp_d(d, 0.0, 10.0);
    else if (!strcmp(k, "tau")) cfg.tau = clamp_d(d, 0.05, 200.0);
    else if (!strcmp(k, "light_min")) cfg.light_min = clamp_i(d, 0, 255);
    else if (!strcmp(k, "cell_min")) cfg.cell_min = clamp_i(d, 1, 64);
    else if (!strcmp(k, "min_cells")) cfg.min_cells = clamp_i(d, 0, NCELL);
    else if (!strcmp(k, "soon_px")) cfg.soon_px = clamp_i(d, 0, (int)NS);
    else if (!strcmp(k, "now_px")) cfg.now_px = clamp_i(d, 0, (int)NS);
    else if (!strcmp(k, "quiet_s")) cfg.quiet_s = clamp_d(d, 0.0, 3600.0);
    else if (!strcmp(k, "regional_max")) cfg.regional_max = clamp_d(d, 0.0, 1.0);
    else if (!strcmp(k, "w_std")) cfg.w_std = clamp_d(d, 0.0, 10.0);
    else if (!strcmp(k, "defer")) set_defer(d != 0.0);
    // the settle clock ticks every 250 ms for 2 s after an update: a longer quiet time would never be met
    else if (!strcmp(k, "tr_quiet")) cfg.tr_quiet = d < 0.1 ? 0.1 : d > 1.0 ? 1.0 : d;
    else if (!strcmp(k, "tr_full")) {
        // 1.8.4 (see the header): the whole screen only; a share above 0 is refused, so no clock can bring it back
        if (d != 0.0) glog("tr_full %s refused: %s cleans the whole screen only (a regional STD stretches the updates outside it)", v, GG_VERSION);
        cfg.tr_full = 0.0;
    }
    else if (!strcmp(k, "tr_pad")) cfg.tr_pad = clamp_i(d, 0, 256);
    // the clock sends "auto" after 0.5 s without updates and keeps ticking for 4 s: a longer quiet time is never met
    else if (!strcmp(k, "auto_quiet")) cfg.auto_quiet = clamp_d(d, 0.2, 2.0);
    else if (!strcmp(k, "auto_pad")) cfg.auto_pad = clamp_i(d, 0, 64);
    else if (!strcmp(k, "clean_text")) cfg.clean_text = clamp_i(d, 0, 1);
    else if (!strcmp(k, "clean_wf")) { if (clean_wf_ok(clamp_i(d, -1, 16))) cfg.clean_wf = clamp_i(d, -1, 16); }
    else if (!strcmp(k, "debug")) cfg.debug = clamp_i(d, 0, 1);
    else if (!strcmp(k, "sd_close")) cfg.sd_close = clamp_i(d, 0, 1);
    else if (!strcmp(k, "gesture_clean")) cfg.gesture_clean = clamp_i(d, 0, 1);
    else if (!strcmp(k, "testcmd")) cfg.testcmd = clamp_i(d, 0, 1);
    else if (!strcmp(k, "gesture_min")) cfg.gesture_min = clamp_d(d, 0.0, 0.1);
    else if (!strcmp(k, "page_clean")) cfg.page_clean = clamp_i(d, 0, 1);
    else if (!strcmp(k, "codrive")) cfg.codrive = clamp_i(d, 0, 2);
    else if (!strcmp(k, "cd_gain")) cfg.cd_gain = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cd_gpos")) cfg.cd_gpos = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cd_minv")) cfg.cd_minv = clamp_d(d, 0.1, 24.0);
    else if (!strcmp(k, "cd_area")) cfg.cd_area = clamp_i(d, 100000, SW * SH);
    else if (!strcmp(k, "cd_comp")) cfg.cd_comp = clamp_i(d, 0, 1);
    else if (!strcmp(k, "cd_max")) cfg.cd_max = d >= 24 ? 24 : d >= 12 ? 12 : 6;
    else if (!strcmp(k, "cd_pre")) cfg.cd_pre = clamp_i(d, 0, 1);
    else if (!strcmp(k, "cd_early")) cfg.cd_early = clamp_i(d, 0, 2);
    else if (!strcmp(k, "cd_std")) cfg.cd_std = clamp_i(d, 0, 2);
    else if (!strcmp(k, "turn_full")) cfg.turn_full = clamp_i(d, 0, 3); // 1.8.26: 2, 1.8.29: 3
    else if (!strcmp(k, "cds_gain")) cfg.cds_gain = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cds_gpos")) cfg.cds_gpos = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cds_lead")) cfg.cds_lead = clamp_i(d, 0, 30);
    else if (!strcmp(k, "cds_every")) cfg.cds_every = clamp_i(d, 0, 1000);
    else if (!strcmp(k, "cds_k27")) cfg.cds_k27 = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cds_k29")) cfg.cds_k29 = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cds_k28")) cfg.cds_k28 = clamp_d(d, 0.0, 3.0);
    else if (!strcmp(k, "cds_lv")) cfg.cds_lv = clamp_i(d, 0, 7);
    else if (!strcmp(k, "cds_carry")) cfg.cds_carry = clamp_i(d, 0, 1);
    else if (!strcmp(k, "cds_w0")) cfg.cds_w0 = clamp_i(d, 0, 320);
    else if (!strcmp(k, "cds_w1")) cfg.cds_w1 = clamp_i(d, 0, 320);
    else if (!strcmp(k, "cds_wk")) cfg.cds_wk = clamp_d(d, -3.0, 3.0);
    else if (!strcmp(k, "cds_wadd")) cfg.cds_wadd = clamp_d(d, -6.0, 6.0);
    else if (!strcmp(k, "cds_wt")) cfg.cds_wt = clamp_i(d, 0, 7); // 1.8.23
    else if (!strcmp(k, "cds_flip")) cfg.cds_flip = clamp_i(d, 0, 1); // 1.8.25
    else if (!strcmp(k, "wt_static")) cfg.wt_static = clamp_i(d, 0, 1); // 1.8.30
    else if (!strcmp(k, "tf_chroma")) cfg.tf_chroma = clamp_i(d, 0, 100); // 1.8.26
    else if (!strcmp(k, "tf_chroma_min")) cfg.tf_chroma_min = clamp_i(d, 0, 10000);
    else if (!strcmp(k, "tf_idle")) cfg.tf_idle = clamp_i(d, 0, 1);
    else if (!strcmp(k, "page_min")) cfg.page_min = clamp_d(d, 0.2, 1.0);
    // 1.8.5 (see the header); the settle clock ticks for 4 s after an update: a longer page_settle would never be met
    else if (!strcmp(k, "page_gap")) cfg.page_gap = clamp_i(d, 1, 50);
    else if (!strcmp(k, "page_px")) cfg.page_px = clamp_i(d, 0, (int)NS);
    else if (!strcmp(k, "page_every")) cfg.page_every = clamp_i(d, 0, 50);
    else if (!strcmp(k, "page_flip")) cfg.page_flip = clamp_d(d, 0.0, 10.0);
    else if (!strcmp(k, "page_settle")) cfg.page_settle = clamp_d(d, 0.4, 3.0);
    // 1.8.6 (see the header); below 0.2 the request would be tried before the page's frame is finalized
    else if (!strcmp(k, "turn_px")) cfg.turn_px = clamp_i(d, 0, (int)NS);
    else if (!strcmp(k, "turn_px_col")) cfg.turn_px_col = clamp_i(d, 0, (int)NS); // 1.8.22
    else if (!strcmp(k, "page_quiet")) cfg.page_quiet = clamp_d(d, 0.2, 2.0);
    else if (!strcmp(k, "kb_clean")) cfg.kb_clean = clamp_i(d, 0, 1);
    else if (!strcmp(k, "stock_gap")) cfg.stock_gap = clamp_d(d, 0.0, 10.0);
    else if (!strcmp(k, "kb_open_clean")) {
        // 1.8.3: retired (see the header): turning it on is refused, so an old clock cannot bring the band clean back
        if (d != 0.0) glog("kb_open_clean %s refused: retired in %s (the band clean ran into the dialog's own update)", v, GG_VERSION);
        cfg.kb_open_clean = 0;
        if (kbo_req) kbo_drop("kb_open_clean is retired", now_s());
    }
    // below KBO_MAX_AGE (1 s), with room for one more 250 ms tick of the settle clock
    else if (!strcmp(k, "kbo_quiet")) cfg.kbo_quiet = clamp_d(d, 0.2, 0.7);
    else {
        for (int i = 0; i < CL_N; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "w_%s", cl_name[i]);
            if (!strcmp(k, nm)) cfg.w[i] = clamp_d(d, 0.0, 10.0);
        }
    }
}

// ---------------------------------------------------------------- regional clean (xochitl 3.29.1 only)
// Facts from the 3.29.1 binary (see the GhostGuard notes):
//   build-id 7f2e34d3...3d660a13 (note descriptor at vaddr 0x2b8, text LOAD has p_offset == p_vaddr == 0)
//   EPFramebuffer::s_instance            @0x1861668
//   EPFramebufferAcep2 vtable addr point @0x1800ab0, slot 23 = swapBuffers_impl @0x86a9f0
//   object: +0x10 std::mutex (the lock every swapBuffers caller holds), +0x40 current EPScreenModeMap,
//           +0x98 QImage frame (RGB32)
//   swapBuffers_impl(const QRegion &, const EPScreenModeMap &, UpdateFlags): with flag 0x1 it sends the
//   region's rectangles with the STD waveform (map's Mono part wf3, the rest wf2, pixel mode 17).
#define XO_BUILDID_OFF 0x2b8
#define XO_S_INSTANCE 0x1861668
#define XO_VT_ACEP2 0x1800ab0
#define XO_IMPL 0x86a9f0
#define XO_IMPL_SLOT 23
#define FBO_MUTEX 0x10
#define FBO_MAP 0x40
#define FBO_IMAGE 0x98
#define XO_FRAME_BPL 6528 // 1632 px * 4: the frame image's stride (SWTCON init checks 1620/1632/2160)
#define MAP_SLEEP 0x28 // EPScreenModeMap = 7 QRegion d-pointers (Pen,Mono,Animation,UI,Content,Sleep,Overlay) + int[7]

// ---- 1.8.18: the co-drive's arithmetic. Pure (no xochitl, no clock): the host tests run exactly this code. ----
#define CD_MAXF 320
#define CD_NG (CD_MAXF / 5 + 1) // frame groups (5 frames of 3 bits in a u16 cell) of the longest table we take
static const int8_t cd_volt[8] = {0, -24, -12, -6, 6, 12, 24, 0};

// Sigma-delta: the drive c[] (V per frame) as table codes q[] (0 none, 1..3 = -24 / -12 / -6 V, 4..6 = +6 / +12 /
// +24 V) of at most maxv volts. Frames before `first` get no code (the generator is there already); their dose moves
// to the frames after them. More than the levels can give is dropped, not driven later. *accp is the carry in and
// out (V x frames). Returns the +-6 V frames (negative: towards white).
// 1.8.21: cd_quant_m is the same with the levels as a mask (lvm: bit 0 = 6 V, bit 1 = 12 V, bit 2 = 24 V; none: no
// code at all) and a choice for the frames before `first` (carry 0: their dose is dropped, the carry in stays).
static int cd_quant_m(const double *c, int nfr, int first, int lvm, int carry, double *accp, int8_t *q) {
    static const int lv[3] = {6, 12, 24};
    int maxv = lvm & 4 ? 24 : lvm & 2 ? 12 : 6;
    double acc = *accp, lim = 2.0 * maxv + 6.0;
    int nq = 0;
    if (!isfinite(acc)) acc = 0.0;
    for (int f = 0; f < nfr; f++) {
        q[f] = 0;
        if (f < first && !carry) continue;
        acc += c[f];
        if (!isfinite(acc)) acc = 0.0;
        if (acc > lim) acc = lim; else if (acc < -lim) acc = -lim;
        if (f < first) continue;
        double a = fabs(acc);
        int v = 0, neg = acc < 0;
        for (int i = 0; i < 3; i++)
            if (((lvm >> i) & 1) && fabs(a - lv[i]) < fabs(a - v)) v = lv[i];
        if (!v) continue;
        q[f] = (int8_t)(neg ? (v == 6 ? 3 : v == 12 ? 2 : 1) : (v == 6 ? 4 : v == 12 ? 5 : 6));
        acc += neg ? v : -v;
        nq += (neg ? -v : v) / 6;
    }
    *accp = acc;
    return nq;
}
static int cd_quant(const double *c, int nfr, int first, int maxv, double *accp, int8_t *q) {
    return cd_quant_m(c, nfr, first, maxv >= 24 ? 7 : maxv >= 12 ? 3 : 1, 1, accp, q);
}

// a table cell (frame group g) with the codes q[] in the frames the table leaves at 0 V
static uint16_t cd_cell(uint16_t old, const int8_t *q, int g, int nfr) {
    uint16_t val = old;
    for (int k = 0; k < 5 && g * 5 + k < nfr; k++)
        if (q[g * 5 + k] && ((old >> (3 * k)) & 7) == 0) val |= (uint16_t)((q[g * 5 + k] & 7) << (3 * k));
    return val;
}

// a row (idx = from << 5 | to) of a table read as a whole: the sum of its volts over the frames, and of their sizes
static int cds_rowsum(const uint16_t *cells, int nfr, int idx) {
    int sum = 0;
    for (int f = 0; f < nfr; f++) sum += cd_volt[(cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7];
    return sum;
}
static int cds_rowabs(const uint16_t *cells, int nfr, int idx) {
    int sum = 0;
    for (int f = 0; f < nfr; f++) sum += abs(cd_volt[(cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7]);
    return sum;
}

// The mean drive m[] (V, per frame) of an update: hist[from << 5 | to] of tot pixels take the table's row from -> to;
// a pixel that keeps its level is held (0 V). *chg: the part of the pixels that changes.
static void cds_mean(const uint32_t *hist, double tot, const uint16_t *cells, int nfr, double *m, double *chg) {
    double ch = 0.0;
    for (int f = 0; f < nfr; f++) m[f] = 0.0;
    *chg = 0.0;
    if (!(tot > 0.0)) return;
    for (int idx = 0; idx < 1024; idx++) {
        if (!hist[idx] || (idx >> 5) == (idx & 31)) continue;
        double w = hist[idx] / tot;
        ch += w;
        for (int f = 0; f < nfr; f++) m[f] += w * cd_volt[(cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7];
    }
    *chg = ch;
}

// the co-drive c[] of the mean drive m[]: gneg of it towards white, gpos towards dark; returns its peak (V)
static double cds_codrive(const double *m, int nfr, double gneg, double gpos, double *c) {
    double peak = 0.0;
    for (int f = 0; f < nfr; f++) {
        c[f] = m[f] * (m[f] < 0.0 ? gneg : gpos);
        if (fabs(c[f]) > peak) peak = fabs(c[f]);
    }
    return peak;
}

// 1.8.21: the STD co-drive c[] as one group of hold cells gets it (grp bit 0: column 29, else 27; bit 1: the rows
// from levels 28 and 29, else from 30 and 31): the window's factor and offset, then the group's factors. With the
// default keys out[] is c[].
static void cds_shape(const double *c, int nfr, int grp, double *out) {
    double k = (grp & 1 ? cfg.cds_k29 : cfg.cds_k27) * (grp & 2 ? cfg.cds_k28 : 1.0), wk = cfg.cds_wk, wadd = cfg.cds_wadd;
    int w0 = cfg.cds_w0, w1 = cfg.cds_w1;
    for (int f = 0; f < nfr; f++) {
        double v = c[f];
        if (f >= w0 && f < w1) v = v * wk + wadd;
        out[f] = v * k;
    }
}
static int cds_lvm(void) {
    int lv = cfg.cds_lv & 7, maxv = cfg.cd_max;
    return lv ? lv : maxv >= 24 ? 7 : maxv >= 12 ? 3 : 1;
}

// ---- 1.8.23: the white tail of a co-driven STD turn (see the header). Pure, as the rest of this block. ----
#define CDS_WT_NF 86   // the measured table's length (the STD at 22-28 C; the STD tables of 28-50 C have 86 frames too)
#define CDS_WT_NR 43   // and the number of its rows the rule below takes: the table's signature (see cds_wt_mark)
#define CDS_WT_F0 47   // from here on an ink -> white row drives towards white or idles, in every frame
#define CDS_WT_I0 68   // the tail's frames: 68..71 and 74..76 (idle in the long rows to 85, in the short ones to 76)
#define CDS_WT_I1 76
#define CDS_WT_ROWS 54 // from 0..26, to 28 / 30
static int cds_wt_idle(int f) { return (f >= CDS_WT_I0 && f <= 71) || (f >= 74 && f < CDS_WT_NF); }
// 1: the row idx of a table of nf frames ends as the measured ink -> white rows do: from CDS_WT_F0 on only towards
// white (-24 / -12 / -6) or idle, at least one frame driven, and idle in the frames a tail can take. A row that idles
// all the way from CDS_WT_F0 (black -> white, the rows the table does not use) is left alone.
static int cds_wt_row(const uint16_t *cells, int nf, int idx) {
    if (nf != CDS_WT_NF) return 0;
    int drv = 0;
    for (int f = CDS_WT_F0; f < nf; f++) {
        int c = (cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7;
        if (c > 3) return 0; // towards dark, or the 0 V code 7
        if (!c) continue;
        if (f >= CDS_WT_I0 && f <= CDS_WT_I1 && cds_wt_idle(f)) return 0;
        drv++;
    }
    return drv > 0;
}
// the tail as codes q[] (2 = -12 V; nf entries): the first n idle frames from CDS_WT_I0 on, below `end`. Returns the
// last of them (-1: none, q[] all 0).
static int cds_wt_q(int nf, int end, int n, int8_t *q) {
    int last = -1;
    for (int f = 0; f < nf; f++) q[f] = 0;
    if (nf != CDS_WT_NF) return -1;
    for (int f = CDS_WT_I0, k = 0; f < end && f <= CDS_WT_I1 && f < nf && k < n; f++) {
        if (!cds_wt_idle(f)) continue;
        q[f] = 2; k++; last = f;
    }
    return last;
}
// The rows that take the tail q[] (rows[] gets their idx, at most CDS_WT_ROWS; returns how many), and the tail put
// into them in `cells`, a copy of the table: the mean drive is then taken from what will really be driven.
// Only the table the tail was measured on gets it (0 and `cells` untouched otherwise): the rule must take exactly
// CDS_WT_NR rows, the three short ones (15, 19, 21 -> 30) and 0 -> 28 among them. The stock STD tables of the other
// temperature ranges with 86 frames give 2 (28-33 C), 3 (33-38 C), 41 (38-43 C, without 15 / 19 -> 30) and 0 rows
// (43-50 C): none of them was measured, none gets a tail.
static int cds_wt_mark(uint16_t *cells, int nf, const int8_t *q, uint16_t *rows) {
    int nr = 0, sig = 0;
    for (int from = 0; from < 27; from++)
        for (int ti = 0; ti < 2; ti++) {
            int idx = (from << 5) | (ti ? 30 : 28);
            if (!cds_wt_row(cells, nf, idx)) continue;
            sig += idx == ((15 << 5) | 30) || idx == ((19 << 5) | 30) || idx == ((21 << 5) | 30) || idx == 28;
            rows[nr++] = (uint16_t)idx;
        }
    if (nr != CDS_WT_NR || sig != 4) return 0;
    for (int i = 0; i < nr; i++)
        for (int g = CDS_WT_I0 / 5; g * 5 < nf; g++) cells[g * 1025 + rows[i]] = cd_cell(cells[g * 1025 + rows[i]], q, g, nf);
    return nr;
}

// ---- 1.8.25: the two whites (see the header). Pure. ----
#define CDF_A ((28 << 5) | 30) // the flip rows
#define CDF_B ((30 << 5) | 28)
static int cdf_code(const uint16_t *cells, int idx, int f) { return (cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7; }
// What the flip rows of a table of nf frames are. 1: stock (each is the white refresh row, 28 -> 28 / 30 -> 30);
// 2: both 0 V in every frame (code 0: free for the co-drive); 0: neither, or the table does not show that the two
// whites are one state: the two refresh rows equal and driven, the rows from 28 and from 30 equal for every target
// 0..27 and 29 (the hold columns among them), the hold cells of column 27 free.
static int cdf_kind(const uint16_t *cells, int nf) {
    int stock = 1, zero = 1, drv = 0;
    if (nf < 8 || nf >= CD_MAXF) return 0;
    for (int f = 0; f < nf; f++) {
        int a = cdf_code(cells, CDF_A, f), b = cdf_code(cells, CDF_B, f);
        int w28 = cdf_code(cells, (28 << 5) | 28, f), w30 = cdf_code(cells, (30 << 5) | 30, f);
        if (w28 != w30 || cdf_code(cells, (28 << 5) | 27, f)) return 0;
        if (cd_volt[w28]) drv = 1;
        if (a != w28 || b != w30) stock = 0;
        if (a || b) zero = 0;
        for (int to = 0; to < 30; to++)
            if (to != 28 && cdf_code(cells, (28 << 5) | to, f) != cdf_code(cells, (30 << 5) | to, f)) return 0;
    }
    if (!drv) return 0;
    return stock ? 1 : zero ? 2 : 0;
}
// 1: the table the flips were measured on (the STD of 22-28 C): the white tail's signature. `cells` is not changed
// (an all-zero tail).
// 1.8.30: or that table with the white tail standing in it (wt_static).
static int wts_sig(const uint16_t *cells, int nf, uint16_t *rows);
static int cdf_meas(uint16_t *cells, int nf) {
    uint16_t rows[CDS_WT_ROWS];
    return wts_sig(cells, nf, rows) >= 0;
}
// ---- 1.8.30: the white tail standing in the table (wt_static). Pure. ----
static int wts_tail(int f) { return f >= CDS_WT_I0 && f <= CDS_WT_I1 && cds_wt_idle(f); } // 68..71, 74..76
// the cell of frame group g with the tail's frames at -12 V (on) or idle (off); other frames as they are
static uint16_t wts_cell(uint16_t c, int g, int on) {
    for (int k = 0; k < 5; k++)
        if (wts_tail(g * 5 + k)) c = (uint16_t)((c & ~(7u << (3 * k))) | ((on ? 2u : 0u) << (3 * k)));
    return c;
}
// cds_wt_row, but the tail's frames may also all hold -12 V. 0: no such row; 1: the stock row; 2: the row with the tail.
static int wts_row(const uint16_t *cells, int nf, int idx) {
    if (nf != CDS_WT_NF) return 0;
    int drv = 0, tl = 0;
    for (int f = CDS_WT_F0; f < nf; f++) {
        int c = (cells[(f / 5) * 1025 + idx] >> (3 * (f % 5))) & 7;
        if (c > 3) return 0;
        if (wts_tail(f)) {
            if (c && c != 2) return 0;
            tl += c == 2;
        } else if (c) drv++;
    }
    if (!drv || (tl && tl != 7)) return 0;
    return tl ? 2 : 1;
}
// The measured table's signature (cds_wt_mark's: CDS_WT_NR rows, the short ones and 0 -> 28 among them), whether the
// tail stands in its rows or not. rows[]: their idx. -1: another table, or one whose rows are mixed; 0: the stock
// table; CDS_WT_NR: every row has the tail.
static int wts_sig(const uint16_t *cells, int nf, uint16_t *rows) {
    int nr = 0, sig = 0, nt = 0;
    for (int from = 0; from < 27; from++)
        for (int ti = 0; ti < 2; ti++) {
            int idx = (from << 5) | (ti ? 30 : 28), k = wts_row(cells, nf, idx);
            if (!k) continue;
            sig += idx == ((15 << 5) | 30) || idx == ((19 << 5) | 30) || idx == ((21 << 5) | 30) || idx == 28;
            nt += k == 2;
            rows[nr++] = (uint16_t)idx;
        }
    if (nr != CDS_WT_NR || sig != 4 || (nt && nt != nr)) return -1;
    return nt;
}
// the first frame from which the refresh row 28 -> 28 idles to the end (nf: it never does): a reader of the table that
// is there or further reads the same from a stock flip row and from one at 0 V
static int cdf_quiet(const uint16_t *cells, int nf) {
    int q = nf;
    while (q > 0 && !cd_volt[cdf_code(cells, (28 << 5) | 28, q - 1)]) q--;
    return q;
}
// the cell of frame group g of the flip row idx as it is to be: its frames below nf at 0 V (zero), or those of its
// refresh row (28 -> 28 for CDF_A, 30 -> 30 for CDF_B); the bits of no frame stay
static uint16_t cdf_cell(const uint16_t *cells, int nf, int g, int idx, int zero) {
    uint16_t old = cells[g * 1025 + idx], src = cells[g * 1025 + (idx == CDF_A ? (28 << 5) | 28 : (30 << 5) | 30)], val = old;
    for (int k = 0; k < 5 && g * 5 + k < nf; k++) {
        val &= (uint16_t)~(7 << (3 * k));
        if (!zero) val |= (uint16_t)(src & (7 << (3 * k)));
    }
    return val;
}

// The sample grid for the levels before and after an STD turn: one pixel of every 8x8 block, its place in the block
// changing from row to row (no column or line of a ruled or dotted page is sampled all the way).
#define CDS_STEP 8
#define CDS_NX (SW / CDS_STEP)
#define CDS_NY (SH / CDS_STEP)
#define CDS_N (CDS_NX * CDS_NY)
static int cds_x(int i, int j) { return i * CDS_STEP + ((j * 3) & 7); }
static int cds_y(int j) { return j * CDS_STEP + ((j * 5) & 7); }
// the samples of one state buffer row (the cds_y(j)-th: words with the level in bits 0-4)
static void cds_row(const uint32_t *row, int j, uint8_t *lv) {
    for (int i = 0; i < CDS_NX; i++) lv[i] = (uint8_t)(row[cds_x(i, j)] & 31);
}
// hist[from << 5 | to] of n samples; returns how many change
static int cds_histo(const uint8_t *from, const uint8_t *to, int n, uint32_t *hist) {
    int chg = 0;
    memset(hist, 0, 1024 * sizeof *hist);
    for (int i = 0; i < n; i++) {
        hist[((from[i] & 31) << 5) | (to[i] & 31)]++;
        chg += from[i] != to[i];
    }
    return chg;
}

#ifdef __linux__
static const uint8_t xo_buildid[20] = {0x7f, 0x2e, 0x34, 0xd3, 0x8b, 0xaf, 0x3a, 0xce, 0x11, 0x17,
                                       0xa5, 0xac, 0x3a, 0x9c, 0x18, 0x31, 0x3d, 0x66, 0x0a, 0x13};
static const uint32_t xo_impl_sig[8] = {0xd503233f, 0xa9a57bfd, 0x910003fd, 0xa90153f3,
                                        0xaa0003f4, 0xa90363f7, 0xaa0203f8, 0xa9046bf9};

typedef void (*impl_t)(void *self, const void *region, const void *map, int flags);
static uintptr_t xo_base;
static void *(*q_current_thread)(void);
static void *(*q_object_thread)(const void *);
static const uint8_t *(*q_image_const_bits)(const void *);
static int (*q_image_width)(const void *);
static int (*q_image_height)(const void *);
static int (*q_image_format)(const void *);
static long long (*q_image_bpl)(const void *);
static void (*q_region_ctor)(void *, int, int, int, int, int);
static void (*q_region_dtor)(void *);
static _Bool (*q_region_is_empty)(const void *);

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name && info->dlpi_name[0]) return 0; // the main program has an empty name
    *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
    return 1;
}

static void xo_fail(const char *why) {
    xo_state = -1;
    snprintf(xo_why, sizeof xo_why, "%s", why);
    glog("regional clean disabled: %s", why);
}

// One-time check of the running xochitl. 1 = verified, -1 = not the 3.29.1 binary we know: the sensor
// then stays passive (no pixels are read, no clean is ever issued).
static int xo_verify(void) {
    if (xo_state) return xo_state;
    uintptr_t base = 0;
    dl_iterate_phdr(phdr_cb, &base);
    if (!base) { xo_fail("no main program base"); return -1; }
    if (memcmp((const void *)(base + XO_BUILDID_OFF), xo_buildid, sizeof xo_buildid)) { xo_fail("xochitl build-id differs"); return -1; }
    if (memcmp((const void *)(base + XO_IMPL), xo_impl_sig, sizeof xo_impl_sig)) { xo_fail("swapBuffers_impl bytes differ"); return -1; }
    q_current_thread = (void *(*)(void))dlsym(RTLD_DEFAULT, "_ZN7QThread13currentThreadEv");
    q_object_thread = (void *(*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK7QObject6threadEv");
    q_image_const_bits = (const uint8_t *(*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK6QImage9constBitsEv");
    q_image_width = (int (*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK6QImage5widthEv");
    q_image_height = (int (*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK6QImage6heightEv");
    q_image_format = (int (*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK6QImage6formatEv");
    q_image_bpl = (long long (*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK6QImage12bytesPerLineEv");
    q_region_ctor = (void (*)(void *, int, int, int, int, int))dlsym(RTLD_DEFAULT, "_ZN7QRegionC1EiiiiNS_10RegionTypeE");
    q_region_dtor = (void (*)(void *))dlsym(RTLD_DEFAULT, "_ZN7QRegionD1Ev");
    q_region_is_empty = (_Bool (*)(const void *))dlsym(RTLD_DEFAULT, "_ZNK7QRegion7isEmptyEv");
    if (!q_current_thread || !q_object_thread || !q_image_const_bits || !q_image_width || !q_image_height ||
        !q_image_format || !q_image_bpl || !q_region_ctor || !q_region_dtor || !q_region_is_empty) {
        xo_fail("Qt symbols missing");
        return -1;
    }
    xo_base = base;
    xo_state = 1;
    snprintf(xo_why, sizeof xo_why, "verified");
    glog("xochitl 3.29.1 verified (base %p)", (void *)base);
    return 1;
}

// The EPFramebuffer object once it exists (NULL before that, or when its layout is not the expected one).
static void *xo_fbo(void) {
    void *fbo = __atomic_load_n((void **)(xo_base + XO_S_INSTANCE), __ATOMIC_ACQUIRE);
    if (!fbo) return NULL;
    uintptr_t vptr = *(const uintptr_t *)fbo;
    if (vptr != xo_base + XO_VT_ACEP2) { xo_fail("unexpected framebuffer class"); return NULL; }
    if (((const uintptr_t *)vptr)[XO_IMPL_SLOT] != xo_base + XO_IMPL) { xo_fail("unexpected vtable slot"); return NULL; }
    return fbo;
}

// Point fb at the EPFramebuffer's own frame image, never at any other QImage. Called for every
// update, so a moved image would be followed (it never moves after start-up).
static int fb_refresh(void) {
    // whenever fb is dropped, the model copy is dropped with it (re-read from the frame when fb is back)
    if (gg_off || xo_verify() < 0) { fb = NULL; prev_valid = 0; return 0; }
    void *fbo = xo_fbo();
    if (!fbo) { fb = NULL; prev_valid = 0; return 0; }
    const void *img = (const char *)fbo + FBO_IMAGE;
    if (q_image_width(img) != SW || q_image_height(img) != SH || q_image_format(img) != 4 /* RGB32 */) {
        xo_fail("unexpected frame image");
        fb = NULL;
        prev_valid = 0;
        return 0;
    }
    long long bpl = q_image_bpl(img);
    const uint8_t *bits = q_image_const_bits(img);
    if (!bits || bpl != XO_FRAME_BPL) {
        xo_fail("unexpected frame stride");
        fb = NULL;
        prev_valid = 0;
        return 0;
    }
    if (bits != fb || bpl != fb_bpl) {
        fb = bits;
        fb_bpl = bpl;
        prev_valid = 0;
        glog("%s: frame buffer RGB32 %dx%d bpl=%lld at %p (EPFramebuffer image)", GG_VERSION, SW, SH, bpl, (const void *)bits);
    }
    return 1;
}

// 1 = ready (fills *out), 0 = not now (retry later), -1 = unsupported on this firmware
static int xo_check(void **out) {
    if (xo_verify() < 0) return -1;
    void *fbo = xo_fbo();
    if (xo_state < 0) return -1;
    if (!fbo) { snprintf(xo_why, sizeof xo_why, "framebuffer object not created yet"); return 0; }
    if (!fb || q_image_const_bits((const char *)fbo + FBO_IMAGE) != fb) { snprintf(xo_why, sizeof xo_why, "frame not read yet"); return 0; }
    if (q_current_thread() != q_object_thread(fbo)) { snprintf(xo_why, sizeof xo_why, "not on the framebuffer thread"); return 0; }
    *out = fbo;
    return 1;
}

// 1.8.3: is xochitl's SWTCON still driving the panel? xochitl drives the panel itself: the frame generator thread of
// its SWTCON object G works through a list of updates. The stock EPFramebuffer::sync() (@0x8626e0, 28 instruction
// words checked below) waits for exactly this condition: with G's mutex (G+0xa0, pthread_mutex_lock) held, the update
// list (G+0x88, a list head: empty when its next pointer points at itself) is empty and no update is active (G+0x80,
// read with ldar); with no generator thread (G+0x50 == 0) it returns at once. G is the pointer in the GOT slot
// @0x1850fb0 (the adrp/ldr pair of sync()). Measured with the same two words (tools/swidle.c, 2026-11-09): busy
// 1.25 s after the New notebook dialog with the Overlay keyboard, 2.52 s with a UI keyboard, 1.0-1.4 s for an STD.
// Returns 1 = idle, 0 = driving (or its mutex is held right now: ask again later), -1 = cannot tell (not verified).
// Also the panel temperature xochitl's waveform choice uses: the float at @0x1840bc8 (temperature(), vtbl+0x78,
// @0x862480, returns it under the mutex @0x1861608; an aligned float is read whole), NAN when unknown.
#define XO_SYNC 0x8626e0
#define XO_SW_GOT 0x1850fb0
#define SW_THREAD 0x50
#define SW_ACTIVE 0x80
#define SW_LIST 0x88
#define SW_LOCK 0xa0
#define XO_TEMP 0x1840bc8
static const uint32_t xo_sync_sig[28] = {
    0xd503233f, 0xa9bd7bfd, 0x910003fd, 0xa9025bf5, 0xd0007f75, 0xf947dab5, 0xf9402aa0, 0xb4000360, 0x910202b6, 0xa90153f3,
    0x910282b4, 0x910222b3, 0x14000007, 0xaa1403e0, 0x97e4d09e, 0x52800c80, 0x97e4bdec, 0xf9402aa0, 0xb40001e0, 0xaa1403e0,
    0x97e4d26c, 0x35000220, 0xf9400260, 0xeb13001f, 0x54fffea1, 0xc8dffec0, 0xb5fffe60, 0xa9425bf5};
static int sw_ok; // 0 not checked yet, 1 the sync() bytes match, -1 they differ (regional cleans are then refused)

static int swtcon_idle(void) {
    if (xo_state != 1) return -1;
    if (!sw_ok) {
        sw_ok = memcmp((const void *)(xo_base + XO_SYNC), xo_sync_sig, sizeof xo_sync_sig) ? -1 : 1;
        if (sw_ok < 0) glog("SWTCON idle check disabled: EPFramebuffer::sync() bytes differ (regional cleans refused)");
    }
    if (sw_ok < 0) return -1;
    uintptr_t g = __atomic_load_n((const uintptr_t *)(xo_base + XO_SW_GOT), __ATOMIC_ACQUIRE);
    if (!g) return -1;
    if (!__atomic_load_n((void *const *)(g + SW_THREAD), __ATOMIC_ACQUIRE)) return 1; // no frame generator: nothing driven
    pthread_mutex_t *lk = (pthread_mutex_t *)(g + SW_LOCK);
    if (pthread_mutex_trylock(lk) != 0) return 0;
    uintptr_t next = __atomic_load_n((const uintptr_t *)(g + SW_LIST), __ATOMIC_ACQUIRE);
    uintptr_t act = __atomic_load_n((const uintptr_t *)(g + SW_ACTIVE), __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(lk);
    return next == g + SW_LIST && act == 0 ? 1 : 0;
}

// 1.8.15 (review): 1 = an update is still waiting to be accepted (its pixels are in the frame buffer but not in the
// state buffer yet), 0 = none (whatever is being driven has its states written), -1 = cannot tell. Only after
// swtcon_idle() (which checks the sync() bytes).
static int swtcon_queued(void) {
    if (xo_state != 1 || sw_ok != 1) return -1;
    uintptr_t g = __atomic_load_n((const uintptr_t *)(xo_base + XO_SW_GOT), __ATOMIC_ACQUIRE);
    if (!g) return -1;
    if (!__atomic_load_n((void *const *)(g + SW_THREAD), __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_t *lk = (pthread_mutex_t *)(g + SW_LOCK);
    if (pthread_mutex_trylock(lk) != 0) return 1;
    uintptr_t next = __atomic_load_n((const uintptr_t *)(g + SW_LIST), __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(lk);
    return next != g + SW_LIST;
}

static double xo_temp(void) {
    if (xo_state != 1) return NAN;
    uint32_t u = __atomic_load_n((const uint32_t *)(xo_base + XO_TEMP), __ATOMIC_RELAXED);
    float t;
    memcpy(&t, &u, sizeof t);
    return isfinite(t) && t > -40.0f && t < 100.0f ? (double)t : NAN;
}

// The stock "BlinkLater" request: a byte in the framebuffer object that turns the NEXT swapBuffers into
// a full-screen STD (read @0x85e36c, set by GhostBuster::viewChange @0x6dc684 and ScreenDriver).
#define FBO_PENDING 0xd0
#define FBO_PRESENT 0xe0 // 1 = the magazine presentation mode (everything is drawn as TEXT)
#define MAP_MONO 0x08
static int clean_text_ok(void *fbo, Rect r);                // 1.8.15, defined behind the co-drive's state reader
static void clean_text_send(void *fbo, const void *region);

// must be called WITHOUT holding mu: the implementation logs its update lines synchronously.
// *r is clipped in place. Refused while a stock flash is pending ("pending"): that request belongs to the
// next frame (the sleep screen's, ScreenDriver's "clear on next update", ...), which must keep it; ours
// would only come right before it.
static const char *do_rclean(Rect *r) {
    void *fbo = NULL;
    int st = xo_check(&fbo);
    if (st < 0) return "unsupported";
    if (st == 0) return "notready";
    if (!clip_rect(r) || r->w < 8 || r->h < 8) return "empty";
    pthread_mutex_t *lock = (pthread_mutex_t *)((char *)fbo + FBO_MUTEX);
    if (pthread_mutex_trylock(lock) != 0) return "busy";
    // With a Sleep part covering > 95% of the screen in the stored screen-mode map, the implementation
    // takes its full-screen sleep path instead of the regional STD path: only clean while it is empty.
    if (!q_region_is_empty((const char *)fbo + FBO_MAP + MAP_SLEEP)) {
        pthread_mutex_unlock(lock);
        return "sleepmap";
    }
    if (__atomic_load_n((uint8_t *)fbo + FBO_PENDING, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(lock);
        return "pending";
    }
    // 1.8.3 (rule K1, IMG_1753): less than the whole screen only while the panel is idle. The pixels an in-flight
    // update drives outside our rectangle were spoiled when ours started meanwhile; a full clean covers them all.
    int sw = swtcon_idle();
    __atomic_store_n(&last_sw, sw, __ATOMIC_RELAXED);
    if (sw != 1 && !rect_full(*r)) {
        pthread_mutex_unlock(lock);
        return sw == 0 ? "inflight" : "noidle";
    }
    // 1.8.29: while a page is owed this clean is the page's arrival (the whole screen, the true frame, the stock
    // clean: the states hold the white, so clean_text_ok would take the page for one without colour)
    int reveal = 0;
    const char *hold = wh_clean_gate(r, sw, &reveal);
    if (hold) {
        pthread_mutex_unlock(lock);
        return hold;
    }
    uint64_t region[2] = {0, 0}; // QRegion is one d-pointer
    q_region_ctor(region, r->x, r->y, r->w, r->h, 0 /* QRegion::Rectangle */);
    // 1.8.15: see clean_text_ok. Review: not while an update waits to be accepted. Our full-screen request crops it
    // out of the queue and draws its pixels itself, but the state buffer (which clean_text_ok reads) does not hold
    // them yet: a colour in it would be held by the TEXT row and never drawn. Behind an update that is being
    // driven (accepted, states written) the check is as good as on an idle panel.
    __atomic_store_n(&last_text_block, 0, __ATOMIC_RELAXED);
    int text = !reveal && __atomic_load_n(&cfg.clean_text, __ATOMIC_RELAXED) && (sw == 1 || swtcon_queued() == 0) && clean_text_ok(fbo, *r);
    __atomic_store_n(&last_clean_text, text, __ATOMIC_RELEASE);
    // 1.8.25: both kinds of clean are complete refreshes, and the one with TEXT targets takes a white 28 to 30 through
    // the flip row itself: the stock rows before it is queued (a turn's line cannot come between: it needs this lock)
    cdf_full("our clean");
    if (text) clean_text_send(fbo, region);
    else ((impl_t)(xo_base + XO_IMPL))(fbo, region, (const char *)fbo + FBO_MAP, 0x1);
    if (reveal) wh_clean_sent();
    q_region_dtor(region);
    pthread_mutex_unlock(lock);
    return "ok";
}

// 1 = the pending stock flash was withdrawn, 0 = none pending (yet), -1 = cannot tell (not verified)
static int withdraw_pending(void) {
    if (xo_state != 1) return -1;
    void *fbo = xo_fbo();
    if (!fbo) return -1;
    uint8_t one = 1;
    return __atomic_compare_exchange_n((uint8_t *)fbo + FBO_PENDING, &one, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// Give a withdrawn stock flash back to xochitl: the next frame then goes out as a full STD, as in stock.
// Under the framebuffer lock (the swapBuffers wrapper reads and clears the byte while holding it), trylock
// only. 1 = done, 0 = framebuffer busy, -1 = not verified.
static int restore_pending(void) {
    if (xo_state != 1) return -1;
    void *fbo = xo_fbo();
    if (!fbo) return -1;
    pthread_mutex_t *lock = (pthread_mutex_t *)((char *)fbo + FBO_MUTEX);
    if (pthread_mutex_trylock(lock) != 0) return 0;
    __atomic_store_n((uint8_t *)fbo + FBO_PENDING, (uint8_t)1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(lock);
    return 1;
}

// The same without the framebuffer lock, for when it cannot be taken (we are inside a swapBuffers, or it stayed
// busy): the wrapper reads the byte with an acquire load and clears it with a release store, so the request goes
// either to the frame being sent or to the next one. Never fails once verified.
static void force_pending(void) {
    if (xo_state != 1) return;
    void *fbo = xo_fbo();
    if (!fbo) return;
    __atomic_store_n((uint8_t *)fbo + FBO_PENDING, (uint8_t)1, __ATOMIC_RELEASE);
}

// 1.8.7 test commands (GGQ "send" and "bleach"; refused unless "set testcmd 1", which every start resets):
//  - sendUpdate (@0x86a910, 8 instruction words checked): (ignored, const QRegion *, int wf, int pixelMode, int flags)
//    puts one request per rectangle of the region into the stock queue and commits it; it uses neither the stored
//    screen-mode map nor the PMODE gate.
//  - ghostControl(BleachNow = 2) of the Acep2 class (@0x86a8d0, 16 words checked): the stock full-screen PMODE
//    (waveform 8: every white pixel gets its 4-frame negative top-off, nothing else is driven).
// Same guards as do_rclean, and always an idle panel. what: 0 = send, 1 = bleach.
#define XO_SEND 0x86a910
#define XO_GHOSTCTL 0x86a8d0
static const uint32_t xo_send_sig[8] = {0xd503233f, 0xa9b97bfd, 0xaa0103e0, 0x910003fd,
                                        0xa90153f3, 0x2a0203f4, 0xa9025bf5, 0x2a0303f5};
static const uint32_t xo_ghostctl_sig[16] = {0xd503245f, 0x7100083f, 0x540000a0, 0x71000c3f, 0x54000040, 0x17ffd39b,
                                             0x17ffff86, 0xd503233f, 0xa9bf7bfd, 0x910003fd, 0x97ffdff6, 0x97fff8a9,
                                             0xa8c17bfd, 0xd50323bf, 0x17ffeb9e, 0xd503201f};
typedef void (*send_t)(void *self, const void *region, int wf, int pm, int flags);
typedef void (*ghostctl_t)(void *self, int mode);
static const char *do_test_update(int what, int wf, int pm, int flags, Rect *r) {
    void *fbo = NULL;
    int st = xo_check(&fbo);
    if (st < 0) return "unsupported";
    if (st == 0) return "notready";
    if (what ? memcmp((const void *)(xo_base + XO_GHOSTCTL), xo_ghostctl_sig, sizeof xo_ghostctl_sig)
             : memcmp((const void *)(xo_base + XO_SEND), xo_send_sig, sizeof xo_send_sig))
        return "sigdiffer";
    if (!what && (!r || !clip_rect(r) || r->w < 8 || r->h < 8)) return "empty";
    pthread_mutex_t *lock = (pthread_mutex_t *)((char *)fbo + FBO_MUTEX);
    if (pthread_mutex_trylock(lock) != 0) return "busy";
    const char *rc = "ok";
    if (!q_region_is_empty((const char *)fbo + FBO_MAP + MAP_SLEEP)) rc = "sleepmap";
    else if (__atomic_load_n((uint8_t *)fbo + FBO_PENDING, __ATOMIC_ACQUIRE)) rc = "pending";
    else {
        int sw = swtcon_idle();
        if (sw != 1) rc = sw == 0 ? "inflight" : "noidle";
    }
    if (!strcmp(rc, "ok")) {
        cdf_full("a test update"); // 1.8.25: whatever it is, it reads the stock rows
        if (what) {
            ((ghostctl_t)(xo_base + XO_GHOSTCTL))(fbo, 2);
        } else {
            uint64_t region[2] = {0, 0};
            q_region_ctor(region, r->x, r->y, r->w, r->h, 0 /* QRegion::Rectangle */);
            ((send_t)(xo_base + XO_SEND))(fbo, region, wf, pm, flags);
            q_region_dtor(region);
        }
    }
    pthread_mutex_unlock(lock);
    return rc;
}

// ---------------------------------------------------------------- 1.8.8 co-drive (xochitl 3.29.1 only)
// Why whites turn lilac on a page turn (measured 9-10 Nov 2026, notes "common drive"): while an update drives its
// changed pixels, the common electrode follows the area-weighted mean drive voltage m(t) = fe*E(t) + fd*D(t)
// (fe / fd: the part of the screen being erased / drawn, E / D: the waveform's erase / draw rows), and every pixel
// that is only HELD sees -m(t). The stock tables hold with 0 V; here the hold cells of the page turn's own table
// (GLR, from-states 28..31, hold columns 27 and 29) get c(t) ~ gain*m(t) for the length of that one update
// (sigma-delta over the 0 / +-6 / +-12 V codes), so the held pixels see ~0 V. Nothing else is touched: no extra
// update over the page, no flash. The strip the turn's update does not cover is driven by a no-change update of
// the same waveform (the "companion"), which runs through the same hold cells.
// The helper thread finds the update in the SWTCON's ACTIVE list (std::list, sentinel G+0x68, nodes of 0xe8 bytes:
// rect x0,y0,x1,y1 at +0x48, start frame +0x60, progress +0x64, length +0x66, table object +0x68, waveform +0x98;
// table object: +0 frames, +4 == 32, +0xc == 3, +0x18 packed table: u16 cell[(frame/5)*1025 + (from<<5|to)], 3 bits
// per frame: 0 0 V, 1 -24, 2 -12, 3 -6, 4 +6, 5 +12, 6 +24). It reads without the SWTCON lock (the frame generator
// must never lose its trylock) through process_vm_readv, so a stale pointer is an error, not a crash, and it checks
// the table object before it writes. The cells are put back as soon as the update has left the list.
#define SW_ACT_LIST 0x68
#define ND_SIZE 0xe8
#define ND_RECT 0x48
#define ND_START 0x60
#define ND_PROG 0x64
#define ND_LEN 0x66
#define ND_TAB 0x68
#define ND_WF 0x98
#define CD_MAXW (8 * (CD_MAXF / 5 + 1))
#define CD_MAXN 32
typedef struct { uintptr_t node, tab; int32_t start; int x0, y0, x1, y1, prog, len, wf; } CdNode;
static pthread_mutex_t cd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cd_cv = PTHREAD_COND_INITIALIZER;
static unsigned cd_reqn;
static int cd_started;
static double cd_fe, cd_fd; // the latest request (under cd_mu)
static Rect cd_bb;          // its updates' bounding box
static struct { uintptr_t a; uint16_t old, val; } cd_sv[CD_MAXW];
static int cd_nsv;
static double cd_acc; // sigma-delta carry (V*frames) between turns
static Rect cd_crs[4]; // the companion updates of the turn in hand
static int cd_ncr;
// 1.8.9 pre-arm. The frame generator works ahead of the list: cells written once the node shows (at progress 5-6)
// are too late for the rows it has already done (measured 10 Nov: an uncompensated band of 70-360 rows at the top of
// the page, its height different in every run; a table patched before the turn gave an even page). So the cells are
// written on the GUI thread at the update's swapBuffers line, before xochitl queues the update, into the GLR table
// object the last GLR turn used (cd_tab4), while the helper is idle and no update on the list reads that table. The
// helper then looks for the update's node as before and puts the cells back at once when the update is not a GLR
// turn, when another update reads the table, or when the turn uses another table object (then it arms late, as 1.8.8,
// and the turn counts as missed: cd_mark). cd_sv / cd_acc belong to the helper while cd_busy, else to the GUI thread
// under cd_mu.
static uintptr_t cd_tab4;    // the GLR table object of the last GLR turn (0: not known yet)
static int cd_busy;          // under cd_mu: the helper works on a request
static int cd_pre;           // under cd_mu: 1 the waiting request's cells are written, 2 written for an older request
static int cd_pre_frames;    // their +-6 V frames
static uintptr_t cd_pre_tab; // the table object they were written to
static int cd_turn;          // under cd_mu: the waiting request is a page turn (1.8.19: 2 a later frame of the turn
                             // that has the id)
static int cd_cur_turn;      // helper: the request in hand is a page turn (1 / 2 as cd_turn)
static long cd_req_id;       // under cd_mu: the waiting turn's id (1.8.13), 0 for other updates
static long cd_cur_id;       // helper: the id of the turn in hand
static int cd_req_stdn;      // under cd_mu: a full refresh went over the screen since the turn before the waiting one
static int cd_req_textn;     // under cd_mu: ... and it was our clean with TEXT targets (1.8.15)
static int cd_req_col;       // under cd_mu: colour samples of the state buffer when the waiting turn was drawn (-1: unread)
static int cd_std_run;       // helper: STD page turns in a row (no GLR turn between them)
static int cd_std_odd;       // helper: those of them with no colour and no full refresh before them (unexplained)
// 1.8.10. (1) The companion updates of a pre-armed page turn are sent as soon as the helper wakes (cd_early), not once
// the turn's node shows: sent late they started ~7 frames after the turn's update and ran the co-drive with their own
// frame count, 7 frames off (measured 10 Nov, 1.8.9: the page stayed at 99-102 over three erase turns, the toolbar
// strip, which only the companion drives, fell 98 -> 95 -> 84). Not when the turn is expected as an STD (cd_std_next)
// and only for page turns (other large updates may use a waveform we do not look for: their companion would run the
// cells alone). When a companion runs alone for 3 walks (the turn's update waits behind another one) the cells go back
// and the turn counts as missed. (2) Review of 1.8.9: a turn request that is overwritten before the helper took it, or
// given up for a newer one, is marked (cd_taken); cells that could not be put back are kept and tried again; both
// loops stop at sleep / codrive off; another reader of the table must show in 3 walks before the pre-arm is dropped
// (the turn's own sibling updates show up before its node at times); a pre-armed turn's node is taken at any progress;
// 3 unreadable walks drop the pre-arm; a dropped pre-arm gives the sigma-delta carry back.
static unsigned cd_taken;    // under cd_mu: the last request the helper took
static double cd_pre_acc;    // under cd_mu: cd_acc before the pre-arm
static int cd_early_ok;      // under cd_mu: the waiting request may send its companions at once
static Rect cd_here_rs[4];   // under cd_mu: the companions the GUI thread queued with the waiting request (1.8.12)
static int cd_here_n, cd_here_why;
static int cd_here_wf = 4;   // under cd_mu: their waveform (1.8.18: 2 for a turn expected as an STD)

static int cd_rd(uintptr_t a, void *buf, size_t n) {
    struct iovec l = {buf, n}, r = {(void *)a, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

static int cd_wr(uintptr_t a, const void *buf, size_t n) {
    struct iovec l = {(void *)(uintptr_t)buf, n}, r = {(void *)a, n};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

// the SWTCON's global, 0 when this xochitl is not the one we know. No lock is taken (the helper must never hold the
// SWTCON's lock: the frame generator only tries it).
static uintptr_t cd_g(void) {
    if (xo_state != 1) return 0;
    int ok = __atomic_load_n(&sw_ok, __ATOMIC_RELAXED);
    if (!ok) {
        ok = memcmp((const void *)(xo_base + XO_SYNC), xo_sync_sig, sizeof xo_sync_sig) ? -1 : 1;
        __atomic_store_n(&sw_ok, ok, __ATOMIC_RELAXED);
    }
    if (ok != 1) return 0;
    return __atomic_load_n((const uintptr_t *)(xo_base + XO_SW_GOT), __ATOMIC_ACQUIRE);
}

// the ACTIVE list as it is right now; -1 when it cannot be told (a read failed because the list changed under us,
// or it is longer than max)
static int cd_walk(uintptr_t g, CdNode *out, int max) {
    uintptr_t p;
    int n = 0;
    if (!cd_rd(g + SW_ACT_LIST, &p, sizeof p)) return -1;
    for (int it = 0; p != g + SW_ACT_LIST; it++) {
        uint8_t nb[ND_SIZE];
        if (it >= 64 || n >= max || p < 0x10000 || (p & 7) || !cd_rd(p, nb, sizeof nb)) return -1;
        CdNode *c = &out[n++];
        int32_t r[4];
        int16_t s;
        uint16_t u;
        c->node = p;
        memcpy(r, nb + ND_RECT, sizeof r);
        c->x0 = r[0]; c->y0 = r[1]; c->x1 = r[2]; c->y1 = r[3];
        memcpy(&c->start, nb + ND_START, 4);
        memcpy(&s, nb + ND_PROG, 2); c->prog = s;
        memcpy(&u, nb + ND_LEN, 2); c->len = u;
        memcpy(&c->tab, nb + ND_TAB, 8);
        memcpy(&s, nb + ND_WF, 2); c->wf = s;
        memcpy(&p, nb, sizeof p);
    }
    return n;
}

static int cd_node_ok(const CdNode *c) {
    return c->x0 >= 0 && c->y0 >= 0 && c->x1 < SW && c->y1 < SH && c->x1 >= c->x0 && c->y1 >= c->y0 && c->tab > 0x10000 &&
           !(c->tab & 7) && c->len > 0 && c->len < CD_MAXF && c->prog >= 0;
}

static long cd_area_of(const CdNode *c) { return (long)(c->x1 - c->x0 + 1) * (c->y1 - c->y0 + 1); } // of an ok node

static int cd_hits(const CdNode *c, Rect r) { return c->x0 < r.x + r.w && c->x1 >= r.x && c->y0 < r.y + r.h && c->y1 >= r.y; }

// ---- 1.8.25: the flip rows held at 0 V for a turn (see "the two whites" in the header) ----
// The rows 28 -> 30 and 30 -> 28 of the STD table are written to 0 V on the GUI thread at the swapBuffers line of a
// turn expected as an STD, before xochitl queues the turn's update (cdf_hold), kept so while turns follow one
// another, and put back by the helper CDF_HOLD_S after the last of them (cdf_tick), when nobody reads a driven frame
// of them. A complete refresh must read the stock rows (measured 11 Nov: with the rows at 0 V a full STD left the
// pale areas deep blue, hp 67.9 against 1.6): every swapBuffers line with the complete-refresh flag and both of our
// cleans put them back at once, before the update is queued (cdf_line, cdf_full). Which way a change under a running
// update goes wrong: the white refresh row drives towards dark first (+24 V, ~24 frames) and to white after that, so
// rows going to 0 V under a reader would leave its flips dark (never done: nobody may be below the frame the row
// idles from, and nothing may wait to be accepted), rows going back to stock under a reader only whiten them more
// (taken when a complete refresh comes, and at CDF_HARD_S).
// Locks: cdf_mu is the innermost one (held for a few table reads / writes; the SWTCON's lock is only tried under it).
// Taken under cd_mu (GUI thread in cd_request, helper), under the framebuffer lock (our cleans, the swapBuffers
// lines) or alone.
#define CDF_HOLD_S 6.0  // the rows stay at 0 V this long after the last turn that asked for them (the page number
                        // hides 3.5-4.4 s after a turn with an update of this table: it runs on the same rows)
#define CDF_HARD_S 9.0  // ... and never longer, whatever is being driven
#define CDF_FULL_S 0.15 // no zeroing this soon after a complete refresh's line (its update may not be queued yet)
static pthread_mutex_t cdf_mu = PTHREAD_MUTEX_INITIALIZER;
static uint16_t cdf_cells[CD_NG * 1025];             // under cdf_mu: the table as cdf_hold read it
static uintptr_t cdf_obj;                            // atomic: the STD table object of the last STD node the helper found (0: none yet)
static int cdf_zon;                                  // written under cdf_mu, read atomically: the rows of cdf_ztab are at 0 V (ours)
static uintptr_t cdf_zobj, cdf_ztab;                 // under cdf_mu: the table object and its packed table
static int cdf_znf, cdf_zquiet;                      // its frames; the frame its white refresh row idles from
static unsigned cdf_zgen;                            // counts the zeroings (a helper that read the table tells its hold by it)
static double cdf_zt0, cdf_zt;                       // when they were written, and when the last turn asked for them
static int cdf_zturns;                               // the turns' frames that found them at 0 V since
static uint16_t cdf_zstock[2][CD_NG], cdf_zzero[2][CD_NG]; // the stock cells of CDF_A / CDF_B and their 0 V cells
static struct { uintptr_t a; uint16_t old, val; } cdf_fsv[2 * CD_NG]; // the co-drive's codes in the rows (old: the 0 V cell)
static int cdf_nfsv;                                 // written under cdf_mu, read atomically
static double cdf_full_t = -1e9;                     // under cdf_mu: the last complete refresh's line (or our clean)
static uintptr_t cdf_said;                           // under cdf_mu: the table the last "not the table" line was about
static unsigned long n_cdf_hold, n_cdf_no, n_cdf_rel, n_cdf_odd; // under cdf_mu: zeroings, refused, put back, cells in doubt

// a table object's packed table and frame count; 0: not a table object as we know them
static int cdf_hdr(uintptr_t obj, uintptr_t *tabp, int *nfp) {
    uint8_t hb[0x20];
    uint32_t nfr, n32, three;
    uintptr_t tab;
    if (obj < 0x10000 || (obj & 7) || !cd_rd(obj, hb, sizeof hb)) return 0;
    memcpy(&nfr, hb, 4); memcpy(&n32, hb + 4, 4); memcpy(&three, hb + 0xc, 4); memcpy(&tab, hb + 0x18, 8);
    if (n32 != 32 || three != 3 || nfr < 8 || nfr >= CD_MAXF || tab < 0x10000 || (tab & 1)) return 0;
    *tabp = tab; *nfp = (int)nfr;
    return 1;
}

// ---- 1.8.30: the white tail standing in the STD table (wt_static; see the header) ----
// Any thread, without locks held (takes cdf_mu): the table object of an STD update that was just seen. Writes the
// tail into the measured table (or takes it out when wt_static is 0); every cell must hold what was read, and what
// was written goes back when one does not. Says each change, and each table it leaves alone once.
static uint16_t wts_cells[CD_NG * 1025];                       // under cdf_mu
static struct { uintptr_t a; uint16_t old; } wts_sv[3 * CDS_WT_ROWS]; // under cdf_mu: this call's writes
static uintptr_t wts_said;                                     // under cdf_mu: the table the last "left alone" line was about
static unsigned long n_wts_put, n_wts_out, n_wts_bad;          // under cdf_mu
static void wts_apply(uintptr_t obj) {
    int want = __atomic_load_n(&cfg.wt_static, __ATOMIC_RELAXED) > 0;
    uintptr_t tab = 0;
    int nf = 0, st = -2, nw = 0, bad = 0, say = 0;
    uint16_t rows[CDS_WT_ROWS];
    if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&cdf_mu);
    if (cdf_hdr(obj, &tab, &nf) && nf == CDS_WT_NF && cd_rd(tab, wts_cells, (size_t)((nf + 4) / 5) * 1025 * sizeof wts_cells[0]))
        st = wts_sig(wts_cells, nf, rows);
    if (st == (want ? 0 : CDS_WT_NR)) {
        for (int g = CDS_WT_I0 / 5; g * 5 <= CDS_WT_I1 && !bad; g++)
            for (int i = 0; i < CDS_WT_NR && !bad; i++) {
                uintptr_t a = tab + ((uintptr_t)g * 1025 + (uintptr_t)rows[i]) * 2;
                uint16_t old = wts_cells[g * 1025 + rows[i]], val = wts_cell(old, g, want), cur;
                if (val == old) continue;
                if (nw >= (int)(sizeof wts_sv / sizeof wts_sv[0]) || !cd_rd(a, &cur, 2) || cur != old || !cd_wr(a, &val, 2)) { bad = 1; break; }
                wts_sv[nw].a = a; wts_sv[nw].old = old; nw++;
            }
        if (bad) {
            for (int i = nw - 1; i >= 0; i--) cd_wr(wts_sv[i].a, &wts_sv[i].old, 2);
            n_wts_bad++;
        } else if (want) n_wts_put++;
        else n_wts_out++;
        say = 1;
    } else if (st != (want ? CDS_WT_NR : 0)) { // not the measured table (another temperature's, or rows mixed): left alone
        say = wts_said != tab ? 2 : 0;
        wts_said = tab;
    }
    pthread_mutex_unlock(&cdf_mu);
    if (say == 1 && bad)
        glog("white tail: a cell of STD table %lx changed or could not be written: %d cells put back, tried again at the next STD update",
             (unsigned long)tab, nw);
    else if (say == 1)
        glog("white tail: %s the %d ink -> white rows of STD table %lx (%d cells; frames 68-71 and 74-76 %s)",
             want ? "written into" : "taken out of", CDS_WT_NR, (unsigned long)tab, nw, want ? "at -12 V, to stay (wt_static 1)" : "idle again (wt_static 0)");
    else if (say == 2)
        glog("white tail: STD table %lx (%d frames) is not the measured one of 22-28 C (%s): left as it is",
             (unsigned long)tab, nf, st == -2 ? "unreadable or another length" : "other rows");
}
// The turn that goes out as the full STD is not handed to the helper: a short-lived thread looks for its node (any STD
// node will do: it names the STD table in use at this temperature) and hands the table to wts_apply. One at a time.
static int wts_busy; // atomic
static void *wts_main(void *arg) {
    (void)arg;
    static CdNode nd[CD_MAXN]; // one thread at a time (wts_busy)
    double t0 = now_s();
    uintptr_t g = cd_g(), obj = 0;
    while (g && !obj && now_s() - t0 < 1.2 && !__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) {
        int n = cd_walk(g, nd, CD_MAXN);
        for (int i = 0; i < n && !obj; i++)
            if (nd[i].wf == 2 && cd_node_ok(&nd[i])) obj = nd[i].tab;
        if (!obj) usleep(4000);
    }
    if (obj) {
        __atomic_store_n(&cdf_obj, obj, __ATOMIC_RELAXED);
        wts_apply(obj);
    }
    __atomic_store_n(&wts_busy, 0, __ATOMIC_RELEASE);
    return NULL;
}
static void wts_kick(void) {
    if (__atomic_exchange_n(&wts_busy, 1, __ATOMIC_ACQ_REL)) return;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    int rc = pthread_create(&th, &at, wts_main, NULL);
    pthread_attr_destroy(&at);
    if (rc) __atomic_store_n(&wts_busy, 0, __ATOMIC_RELEASE);
}

// With cdf_mu held: the rows back to stock, cell by cell from the first frames on, and only what is still ours (the
// 0 V cell, or a code of the turn in hand in it); a cell that holds anything else is left alone. What cannot be read
// or written right now is tried again (the hold stays on: the helper's tick and every swapBuffers line come back
// here), CDF_TRIES times. When a cell stays in doubt cds_flip switches itself off (no table is written again).
#define CDF_TRIES 40
static int cdf_rel_tries; // under cdf_mu
static void cdf_release_l(const char *why) {
    if (!cdf_zon) return;
    uintptr_t tab = 0;
    int nf = 0, nw = 0, odd = 0, unread = 0;
    // the object must still name the table we wrote (a table that was loaded anew is not ours any more)
    int hdr = cdf_hdr(cdf_zobj, &tab, &nf), same = hdr && tab == cdf_ztab && nf == cdf_znf;
    for (int gi = 0; same && gi * 5 < cdf_znf; gi++)
        for (int r = 0; r < 2; r++) {
            uintptr_t a = cdf_ztab + ((uintptr_t)gi * 1025 + (uintptr_t)(r ? CDF_B : CDF_A)) * 2;
            uint16_t cur = 0, st = cdf_zstock[r][gi];
            int ok = 0;
            for (int tr = 0; tr < 2 && !ok; tr++) {
                if (!cd_rd(a, &cur, 2)) continue;
                int ours = cur == cdf_zzero[r][gi];
                for (int i = 0; i < cdf_nfsv && !ours; i++) ours = cdf_fsv[i].a == a && cdf_fsv[i].val == cur;
                if (cur == st) ok = 1;
                else if (!ours) ok = -1;
                else if (cd_wr(a, &st, 2)) { ok = 1; nw++; }
            }
            if (ok < 0) odd++; else if (!ok) unread++;
        }
    if ((!hdr || unread) && ++cdf_rel_tries < CDF_TRIES) { // not readable / writable right now: once more, soon
        if (cdf_rel_tries == 1)
            glog("codrive: flip rows of STD table %lx cannot be put back right now (%s; %s, %d cells written, %d not): trying again",
                 (unsigned long)cdf_ztab, why, hdr ? "cells" : "the table object", nw, unread);
        return;
    }
    if (!same) odd++;
    odd += unread;
    double now = now_s();
    __atomic_store_n(&cdf_nfsv, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&cdf_zon, 0, __ATOMIC_RELEASE);
    n_cdf_rel++;
    if (odd) {
        n_cdf_odd += (unsigned long)odd;
        __atomic_store_n(&cfg.cds_flip, 0, __ATOMIC_RELAXED);
        glog("codrive: flip rows of STD table %lx: %s could not be put back (%d tries): cds_flip is off now",
             (unsigned long)cdf_ztab, !hdr ? "the table object cannot be read, they" : !same ? "the object names another table, they" : "a cell",
             cdf_rel_tries + 1);
    }
    cdf_rel_tries = 0;
    glog("codrive: flip rows 28 <-> 30 back to the stock rows (%s; %d cells, at 0 V for %.0f ms, %d turn frames, %.0f ms after the last; %lu held, %lu refused)",
         why, nw, (now - cdf_zt0) * 1000.0, cdf_zturns, (now - cdf_zt) * 1000.0, n_cdf_hold, n_cdf_no);
}

// Before a complete refresh the rows must be stock when we return: a release that could not finish is tried again
// right here a few times (a few hundred microseconds each) instead of at the next tick.
static void cdf_release_now_l(const char *why) {
    for (int i = 0; i < 8 && cdf_zon; i++) {
        if (i) { struct timespec ts = { 0, 300000 }; nanosleep(&ts, NULL); }
        cdf_release_l(why);
    }
}

// A complete refresh is about to be queued (our cleans, a test update, the sleep screen): the stock rows, now.
static void cdf_full(const char *why) {
    pthread_mutex_lock(&cdf_mu);
    cdf_full_t = now_s();
    cdf_release_now_l(why);
    pthread_mutex_unlock(&cdf_mu);
}

// Every swapBuffers line (GUI thread, or the thread of our clean; before the update is queued). full: the line's
// complete-refresh flag. Any line also ends a hold that outlived CDF_HARD_S (the helper's tick is what normally does).
static void cdf_line(int full) {
    if (!full && !__atomic_load_n(&cdf_zon, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&cdf_mu);
    double now = now_s();
    if (full) cdf_full_t = now;
    if (cdf_zon && full) cdf_release_now_l("a complete refresh");
    else if (cdf_zon && (cdf_rel_tries || now - cdf_zt > CDF_HARD_S))
        cdf_release_l(cdf_rel_tries ? "tried again" : "time limit, at an update");
    pthread_mutex_unlock(&cdf_mu);
}

// GUI thread, under cd_mu, at the swapBuffers line of a turn's frame that is expected as an STD and is no complete
// refresh, before its update and its companions are queued: the flip rows to 0 V, or kept there.
static void cdf_hold(void) {
    if (__atomic_load_n(&cfg.cds_flip, __ATOMIC_RELAXED) <= 0 || __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) return;
    uintptr_t obj = __atomic_load_n(&cdf_obj, __ATOMIC_RELAXED), g = cd_g(), tab = 0;
    if (!obj || !g) return;
    static CdNode nd[CD_MAXN]; // under cdf_mu
    const char *why = NULL;
    int nf = 0, quiet = 0, nw = 0, say = 1;
    pthread_mutex_lock(&cdf_mu);
    double now = now_s();
    if (cdf_zon) { // a turn right behind another: the rows stay (not for another table object: that hold runs out)
        if (cdf_zobj == obj && !cdf_rel_tries) { cdf_zt = now; cdf_zturns++; }
        pthread_mutex_unlock(&cdf_mu);
        return;
    }
    if (now - cdf_full_t < CDF_FULL_S) why = "a complete refresh was just sent";
    else if (swtcon_queued() != 0) why = "an update waits to be accepted";
    else if (!cdf_hdr(obj, &tab, &nf) || !cd_rd(tab, cdf_cells, (size_t)((nf + 4) / 5) * 1025 * sizeof cdf_cells[0])) why = "the table cannot be read";
    else if (cdf_kind(cdf_cells, nf) != 1) { why = "not the stock flip rows we know"; say = cdf_said != tab; cdf_said = tab; }
    else if (!cdf_meas(cdf_cells, nf)) { why = "not the table of 22-28 C"; say = cdf_said != tab; cdf_said = tab; }
    else {
        quiet = cdf_quiet(cdf_cells, nf);
        int n = cd_walk(g, nd, CD_MAXN);
        if (n < 0) why = "the list cannot be read";
        for (int i = 0; i < n && !why; i++)
            if (nd[i].tab == obj && nd[i].prog < quiet) why = "an update reads a driven frame of them";
    }
    if (!why) {
        int ng = (nf + 4) / 5, bad = 0;
        for (int gi = 0; gi < ng; gi++)
            for (int r = 0; r < 2; r++) {
                int idx = r ? CDF_B : CDF_A;
                cdf_zstock[r][gi] = cdf_cells[gi * 1025 + idx];
                cdf_zzero[r][gi] = cdf_cell(cdf_cells, nf, gi, idx, 1);
            }
        for (int gi = 0; gi < ng && !bad; gi++)
            for (int r = 0; r < 2 && !bad; r++) {
                uintptr_t a = tab + ((uintptr_t)gi * 1025 + (uintptr_t)(r ? CDF_B : CDF_A)) * 2;
                uint16_t old = cdf_zstock[r][gi], val = cdf_zzero[r][gi], cur;
                if (val == old) continue;
                if (!cd_rd(a, &cur, 2) || cur != old || !cd_wr(a, &val, 2)) bad = 1; else nw++;
            }
        if (bad) { // a cell changed under us or could not be written: back to what was read (the rows must not stay mixed)
            int left = 0;
            for (int gi = 0; gi < ng; gi++)
                for (int r = 0; r < 2; r++) {
                    uintptr_t a = tab + ((uintptr_t)gi * 1025 + (uintptr_t)(r ? CDF_B : CDF_A)) * 2;
                    uint16_t old = cdf_zstock[r][gi], val = cdf_zzero[r][gi], cur;
                    int ok = val == old;
                    for (int tr = 0; tr < 2 && !ok; tr++)
                        if (cd_rd(a, &cur, 2)) ok = cur != val || cd_wr(a, &old, 2);
                    if (!ok) left++;
                }
            why = "a cell could not be written (put back)";
            if (left) { // not all back: the release owns them from here (it tries again, and says so)
                cdf_zobj = obj; cdf_ztab = tab; cdf_znf = nf; cdf_zquiet = quiet;
                cdf_zgen++;
                cdf_zt0 = cdf_zt = now;
                cdf_zturns = 0;
                __atomic_store_n(&cdf_nfsv, 0, __ATOMIC_RELAXED);
                __atomic_store_n(&cdf_zon, 1, __ATOMIC_RELEASE);
                cdf_release_l("a cell could not be written");
                why = "a cell could not be written (being put back)";
            }
        } else {
            cdf_zobj = obj; cdf_ztab = tab; cdf_znf = nf; cdf_zquiet = quiet;
            cdf_zgen++;
            cdf_zt0 = cdf_zt = now;
            cdf_zturns = 1;
            __atomic_store_n(&cdf_nfsv, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&cdf_zon, 1, __ATOMIC_RELEASE);
            n_cdf_hold++;
        }
    }
    if (why) n_cdf_no++;
    pthread_mutex_unlock(&cdf_mu);
    if (!why)
        glog("codrive: flip rows 28 <-> 30 of STD table %lx (%d frames) at 0 V for the turn (%d cells; the refresh row idles from frame %d)",
             (unsigned long)tab, nf, nw, quiet);
    else if (say)
        glog("codrive: flip rows not held for this turn: %s", why);
}

// With cdf_mu held: the turn's codes out of the flip rows (back to the 0 V cells). Returns the cells that could not
// be put back; the rows then go back to stock altogether. After a release there is nothing to take back.
static int cdf_unput_l(void) {
    int bad = 0;
    for (int i = 0; cdf_zon && !cdf_rel_tries && i < cdf_nfsv; i++) { // (a release that tries again takes them out itself)
        int ok = 0;
        for (int tr = 0; tr < 2 && !ok; tr++) {
            uint16_t cur;
            if (!cd_rd(cdf_fsv[i].a, &cur, 2)) continue;
            if (cur == cdf_fsv[i].val) ok = cd_wr(cdf_fsv[i].a, &cdf_fsv[i].old, 2);
            else ok = cur == cdf_fsv[i].old ? 1 : -1;
        }
        if (ok <= 0) bad++;
    }
    if (bad) cdf_release_l("a code could not be taken out of them"); // it still knows the codes as ours
    if (!cdf_zon || !cdf_rel_tries) __atomic_store_n(&cdf_nfsv, 0, __ATOMIC_RELAXED); // a release that tries again keeps them
    return bad;
}
static int cdf_unput(void) {
    if (!__atomic_load_n(&cdf_nfsv, __ATOMIC_RELAXED)) return 0;
    pthread_mutex_lock(&cdf_mu);
    int bad = cdf_unput_l();
    pthread_mutex_unlock(&cdf_mu);
    return bad;
}

// Helper: the STD turn's table as it is, read while no hold or release can write it. *genp: the hold that has its
// flip rows at 0 V right now (0: none, the rows are whatever the table says).
static int cdf_read(uintptr_t tab, int nf, uint16_t *cells, unsigned *genp) {
    pthread_mutex_lock(&cdf_mu);
    int ok = cd_rd(tab, cells, (size_t)((nf + 4) / 5) * 1025 * sizeof cells[0]);
    *genp = ok && cdf_zon && !cdf_rel_tries && cdf_ztab == tab && cdf_znf == nf ? cdf_zgen : 0; // (not one being given back)
    pthread_mutex_unlock(&cdf_mu);
    return ok;
}

// Helper: the co-drive's codes into the flip rows of the hold `gen` (from 28 as the hold cells from 28 / 29, from 30
// as those from 30 / 31; column 27's shapes). 1 written, 0 that hold is over (nothing written: the turn runs on
// without them), -1 a cell could not be written (all taken back).
static int cdf_put(uintptr_t tab, int nf, unsigned gen, const int8_t *q28, const int8_t *q30) {
    int rc = 1;
    pthread_mutex_lock(&cdf_mu);
    if (!gen || !cdf_zon || cdf_rel_tries || cdf_zgen != gen || cdf_ztab != tab || cdf_znf != nf || cdf_nfsv) rc = 0;
    for (int g = 0; rc == 1 && g * 5 < nf; g++)
        for (int r = 0; r < 2; r++) {
            uintptr_t a = tab + ((uintptr_t)g * 1025 + (uintptr_t)(r ? CDF_B : CDF_A)) * 2;
            uint16_t z = cdf_zzero[r][g], val = cd_cell(z, r ? q30 : q28, g, nf), cur;
            int n = cdf_nfsv;
            if (val == z) continue;
            if (n >= (int)(sizeof cdf_fsv / sizeof cdf_fsv[0]) || !cd_rd(a, &cur, 2) || cur != z || !cd_wr(a, &val, 2)) { rc = -1; break; }
            cdf_fsv[n].a = a; cdf_fsv[n].old = z; cdf_fsv[n].val = val;
            __atomic_store_n(&cdf_nfsv, n + 1, __ATOMIC_RELAXED);
        }
    if (rc < 0) cdf_unput_l();
    pthread_mutex_unlock(&cdf_mu);
    return rc;
}

// Helper, under cd_mu, every 50 ms while it waits and the rows are at 0 V: back to stock when the last turn that
// asked for them is CDF_HOLD_S ago and nobody reads a driven frame of them, at CDF_HARD_S whatever runs, and at sleep.
static void cdf_tick(void) {
    if (!__atomic_load_n(&cdf_zon, __ATOMIC_ACQUIRE)) return;
    static CdNode nd[CD_MAXN]; // under cdf_mu
    uintptr_t g = cd_g();
    const char *why = NULL;
    pthread_mutex_lock(&cdf_mu);
    if (cdf_zon) {
        double age = now_s() - cdf_zt;
        if (cdf_rel_tries) why = "tried again";
        else if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) why = "sleep";
        else if (__atomic_load_n(&cfg.cds_flip, __ATOMIC_RELAXED) <= 0) why = "cds_flip 0";
        else if (!g) why = "no SWTCON";
        else if (age > CDF_HARD_S) why = "time limit";
        else if (age > CDF_HOLD_S) {
            int n = cd_walk(g, nd, CD_MAXN), busy = n < 0;
            for (int i = 0; i < n; i++)
                if (nd[i].tab == cdf_zobj && nd[i].prog < cdf_zquiet) busy = 1;
            // (no look at the queue here: the helper never takes the SWTCON's lock, and stock rows under an
            // update that is accepted a moment later are the harmless direction)
            if (!busy) why = "no turn since";
        }
        if (why) cdf_release_l(why);
    }
    pthread_mutex_unlock(&cdf_mu);
}

static int cd_row(uintptr_t tab, int nfr, int idx, int8_t *v) {
    for (int g = 0; g * 5 < nfr; g++) {
        uint16_t w;
        if (!cd_rd(tab + ((uintptr_t)g * 1025 + (uintptr_t)idx) * 2, &w, 2)) return 0;
        for (int k = 0; k < 5 && g * 5 + k < nfr; k++) v[g * 5 + k] = cd_volt[(w >> (3 * k)) & 7];
    }
    return 1;
}

// puts the patched cells back (a failed read / write is tried once more); returns how many it could not (0 normally)
static int cd_restore(void) {
    int bad = cdf_unput(), keep = 0; // 1.8.25: the codes in the flip rows go first (those rows stay at 0 V)
    for (int i = 0; i < cd_nsv; i++) {
        int ok = 0;
        for (int tr = 0; tr < 2 && !ok; tr++) {
            uint16_t cur;
            if (!cd_rd(cd_sv[i].a, &cur, 2)) continue;
            // only what is still ours: a cell somebody else wrote meanwhile is left alone (and reported)
            if (cur == cd_sv[i].val) ok = cd_wr(cd_sv[i].a, &cd_sv[i].old, 2);
            else ok = cur == cd_sv[i].old ? 1 : -1;
        }
        if (ok == 0) cd_sv[keep++] = cd_sv[i]; // 1.8.10: neither read nor written: kept for another try
        if (ok <= 0) bad++;
    }
    cd_nsv = keep;
    return bad;
}

// cd_restore, and when a cell could not be put back the co-drive switches itself off (the page clean takes over)
static int cd_restore_chk(void) {
    int cells = cd_nsv, bad = cd_restore();
    if (bad) {
        __atomic_store_n(&cfg.codrive, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&n_cd[CD_BAD], 1, __ATOMIC_RELAXED);
        glog("codrive: %d of %d table cells could not be put back: the co-drive is off now", bad, cells);
    }
    return bad;
}

// 1.8.18 (out of cd_arm): the codes q[] into the hold cells (from 28..31, to 27 / 29) of the packed table `tab`, only
// in frames the table leaves at 0 V; what is written is kept in cd_sv. Frame group by frame group, the earliest
// first: what the generator reads next is complete first. -1: a cell could not be read or written (all put back).
// 1.8.21: qs[grp] are the codes of each group of cells (grp as for cds_shape)
static int cd_put4(uintptr_t tab, int nfr, const int8_t *const qs[4]) {
    for (int g = 0; g * 5 < nfr; g++) {
        int any = 0;
        for (int k = 0; k < 5 && g * 5 + k < nfr; k++)
            for (int i = 0; i < 4; i++) any |= qs[i][g * 5 + k];
        if (!any) continue;
        for (int from = 28; from < 32; from++) {
            for (int ti = 0; ti < 2; ti++) {
                const int8_t *q = qs[(from < 30 ? 2 : 0) | ti];
                int idx = (from << 5) | (ti ? 29 : 27);
                uintptr_t a = tab + ((uintptr_t)g * 1025 + (uintptr_t)idx) * 2;
                uint16_t old, val;
                if (!cd_rd(a, &old, 2)) { cd_restore_chk(); return -1; }
                val = cd_cell(old, q, g, nfr);
                if (val == old || cd_nsv >= CD_MAXW) continue;
                if (!cd_wr(a, &val, 2)) { cd_restore_chk(); return -1; }
                cd_sv[cd_nsv].a = a; cd_sv[cd_nsv].old = old; cd_sv[cd_nsv].val = val; cd_nsv++;
            }
        }
    }
    return 0;
}
static int cd_put(uintptr_t tab, int nfr, const int8_t *q) {
    const int8_t *const qs[4] = {q, q, q, q};
    return cd_put4(tab, nfr, qs);
}
// 1.8.23: the white tail q[] into the rows[] (idx) of the packed table `tab`, in frames the table leaves at 0 V; kept
// in cd_sv with the hold cells. -1: a cell could not be read, written or kept (all put back).
static int cds_wt_put(uintptr_t tab, int nfr, const uint16_t *rows, int nr, const int8_t *q) {
    for (int g = 0; g * 5 < nfr; g++) {
        int any = 0;
        for (int k = 0; k < 5 && g * 5 + k < nfr; k++) any |= q[g * 5 + k];
        if (!any) continue;
        for (int i = 0; i < nr; i++) {
            uintptr_t a = tab + ((uintptr_t)g * 1025 + (uintptr_t)rows[i]) * 2;
            uint16_t old, val;
            if (!cd_rd(a, &old, 2)) { cd_restore_chk(); return -1; }
            val = cd_cell(old, q, g, nfr);
            if (val == old) continue;
            if (cd_nsv >= CD_MAXW || !cd_wr(a, &val, 2)) { cd_restore_chk(); return -1; }
            cd_sv[cd_nsv].a = a; cd_sv[cd_nsv].old = old; cd_sv[cd_nsv].val = val; cd_nsv++;
        }
    }
    return 0;
}

// Writes the co-drive of an update with erased part fe and drawn part fd into the hold cells of the GLR table object
// tobj. 1 armed, 0 below the gate, -1 the table object is not what we know (len >= 0: its length must fit a node of
// that length). Frames before `first` are left alone (the generator is there already); their dose moves to the frames
// after them.
static int cd_arm(uintptr_t tobj, int len, int first, double fe, double fd, int *frames) {
    uint8_t hb[0x20];
    uint32_t nfr, n32, three;
    uintptr_t tab;
    if (cd_nsv && (cd_restore_chk() || cd_nsv)) return -1;
    if (tobj < 0x10000 || (tobj & 7) || !cd_rd(tobj, hb, sizeof hb)) return -1;
    memcpy(&nfr, hb, 4); memcpy(&n32, hb + 4, 4); memcpy(&three, hb + 0xc, 4); memcpy(&tab, hb + 0x18, 8);
    if (n32 != 32 || three != 3 || nfr < 8 || nfr >= CD_MAXF || (len >= 0 && (int)nfr != len + 1) || tab < 0x10000 || (tab & 1)) return -1;
    int8_t e[CD_MAXF], d[CD_MAXF], q[CD_MAXF];
    if (!cd_row(tab, (int)nfr, (0 << 5) | 30, e) || !cd_row(tab, (int)nfr, (30 << 5) | 0, d)) return -1;
    double gneg = cfg.cd_gain, gpos = cfg.cd_gpos, c[CD_MAXF], peak = 0.0, se = 0.0, sd = 0.0;
    int maxv = cfg.cd_max;
    for (int f = 0; f < (int)nfr; f++) {
        se += e[f]; sd += d[f];
        c[f] = fe * e[f] + fd * d[f];
        c[f] *= c[f] < 0.0 ? gneg : gpos; // towards white / towards dark (the eye forgives only the first)
        if (fabs(c[f]) > peak) peak = fabs(c[f]);
    }
    if (se >= -100.0 || sd <= 100.0) return -1; // not an erase row / a draw row: an unknown table
    if (peak < cfg.cd_minv) return 0;
    double acc = cd_acc;
    int nq = cd_quant(c, (int)nfr, first, maxv, &acc, q); // 1.8.18: the same arithmetic, now where the host tests reach it
    cd_acc = acc > 3.0 ? 3.0 : acc < -3.0 ? -3.0 : acc;
    if (cd_put(tab, (int)nfr, q) < 0) return -1;
    *frames = nq;
    return 1;
}

// GUI thread, under cd_mu, helper idle (see "1.8.9 pre-arm"): 1 the cells are written
static int cd_prearm(double fe, double fd) {
    uintptr_t tobj = __atomic_load_n(&cd_tab4, __ATOMIC_RELAXED), g;
    if (!tobj || !cfg.cd_pre || !(g = cd_g())) return 0;
    CdNode nd[CD_MAXN];
    int n = cd_walk(g, nd, CD_MAXN);
    if (n < 0) return 0;
    for (int i = 0; i < n; i++)
        if (nd[i].tab == tobj) return 0; // an update reads the table right now
    double acc0 = cd_acc;
    int frames = 0, r = cd_arm(tobj, -1, 0, fe, fd, &frames);
    if (r < 0) __atomic_store_n(&cd_tab4, 0, __ATOMIC_RELAXED); // not the table any more: the next GLR turn tells
    if (r <= 0) { cd_acc = acc0; return 0; }
    cd_pre_acc = acc0;
    cd_pre_frames = frames;
    cd_pre_tab = tobj;
    return 1;
}

// what the turn's updates (bounding box bb) leave out of the screen: up to 4 rectangles
static int cd_comp_rects(Rect bb, Rect *rs) {
    int n = 0;
    if (bb.x >= 8) rs[n++] = (Rect){0, 0, bb.x, SH};
    if (SW - (bb.x + bb.w) >= 8) rs[n++] = (Rect){bb.x + bb.w, 0, SW - (bb.x + bb.w), SH};
    if (bb.y >= 8) rs[n++] = (Rect){bb.x, 0, bb.w, bb.y};
    if (SH - (bb.y + bb.h) >= 8) rs[n++] = (Rect){bb.x, bb.y + bb.h, bb.w, SH - (bb.y + bb.h)};
    return n;
}

// 1.8.12: the companions of a pre-armed page turn, queued by the GUI thread itself. Sent from the helper they start
// 1-3 frames after the turn's update (1.8.11, measured: the GUI thread keeps the framebuffer lock until its own
// update is committed) and read the co-drive cells that much out of step: the toolbar strip lost ~2 % a turn.
// Stock updates are queued in three steps: "begin" (@0x8628d0: takes the SWTCON mutex G+0xa0), one "prepare" per
// rectangle (@0x8694b0(const int32_t req[10]): {top, left, bottom, right, flags, waveform, 0, pixel mode, 0, 0};
// it appends a node to the list at G+0x4128, which only prepare and commit use), and "commit" (@0x865780: moves
// that list into one batch for the frame generator and releases the mutex). sendUpdate and swapBuffers_impl are
// exactly these three. We are called from the log call at the top of the turn's swapBuffers (the framebuffer lock
// is this thread's, begin has not run): with the SWTCON mutex taken for the moment, one prepare per rectangle and
// no commit; the stock commit of this very swapBuffers takes our nodes along in its batch, so they start in the
// turn's first frame. Should that swapBuffers not commit, they leave with the next commit: a no-change update.
// Rectangles queued, or why not: -1 not verified, -2 SWTCON mutex busy, -3 asleep, -4 sleep map, -5 a stock flash
// is pending, -6 not inside swapBuffers (the framebuffer lock is not this thread's; glibc keeps the owner's tid).
#define XO_UPREP 0x8694b0
#define XO_UCOMMIT 0x865780
static const uint32_t xo_uprep_sig[12] = {0xd503233f, 0xd10843ff, 0xd2882508, 0xa9007bfd, 0x910003fd, 0xa90153f3,
                                          0xf0007f34, 0xf947da93, 0xa9025bf5, 0xaa0003f6, 0xa90573fb, 0x8b08027b};
static const uint32_t xo_ucommit_sig[12] = {0xd503233f, 0xa9b47bfd, 0x910003fd, 0xf0007f40, 0xf947d800, 0xa90153f3,
                                            0xa9025bf5, 0xa9046bf9, 0x9102201a, 0xf9404402, 0xf9003fe0, 0xf90047fa};
typedef void (*uprep_t)(const int32_t *req);
static int cd_companion_here(Rect bb, Rect *rs, int wf) {
    void *fbo = xo_fbo();
    uintptr_t g = cd_g();
    if (!fbo || !g || memcmp((const void *)(xo_base + XO_UPREP), xo_uprep_sig, sizeof xo_uprep_sig) ||
        memcmp((const void *)(xo_base + XO_UCOMMIT), xo_ucommit_sig, sizeof xo_ucommit_sig) ||
        memcmp((const void *)(xo_base + XO_SEND), xo_send_sig, sizeof xo_send_sig))
        return -1;
    if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) return -3;
    int tid = (int)syscall(SYS_gettid);
    pthread_mutex_t *gl = (pthread_mutex_t *)(g + SW_LOCK);
    if (__atomic_load_n(&((const int *)((const char *)fbo + FBO_MUTEX))[2], __ATOMIC_RELAXED) != tid) return -6;
    if (__atomic_load_n(&((const int *)gl)[2], __ATOMIC_RELAXED) == tid) return -6; // between begin and commit: not ours to touch
    if (!q_region_is_empty((const char *)fbo + FBO_MAP + MAP_SLEEP)) return -4;
    if (__atomic_load_n((uint8_t *)fbo + FBO_PENDING, __ATOMIC_ACQUIRE)) return -5;
    int n = cd_comp_rects(bb, rs);
    if (!n) return 0;
    int got = 0;
    for (int i = 0; i < 8 && !(got = pthread_mutex_trylock(gl) == 0); i++) usleep(250);
    if (!got) return -2;
    for (int i = 0; i < n; i++) {
        int32_t req[10] = {rs[i].y, rs[i].x, rs[i].y + rs[i].h - 1, rs[i].x + rs[i].w - 1, 0, wf, 0, 0x11, 0, 0};
        ((uprep_t)(xo_base + XO_UPREP))(req);
    }
    pthread_mutex_unlock(gl);
    return n;
}

// no-change updates of the same waveform over what the turn's updates (bounding box bb) leave out; they run through
// the same (patched) hold cells (every other hold cell of GLR is 0 V for every from-state: nothing else is driven).
// Sent from the helper thread under the framebuffer lock, like every swapBuffers. Rectangles sent, or why not:
// -1 not verified, -2 the framebuffer stayed busy (8 ms), -3 asleep, -4 sleep map, -5 a stock flash is pending.
static int cd_companion(Rect bb, int wf) {
    void *fbo = xo_fbo();
    if (!fbo || memcmp((const void *)(xo_base + XO_SEND), xo_send_sig, sizeof xo_send_sig)) return -1;
    if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) return -3;
    Rect *rs = cd_crs;
    cd_ncr = 0;
    int n = cd_comp_rects(bb, rs);
    if (!n) return 0;
    pthread_mutex_t *lock = (pthread_mutex_t *)((char *)fbo + FBO_MUTEX);
    int got = 0;
    for (int i = 0; i < 16 && !(got = pthread_mutex_trylock(lock) == 0); i++) usleep(500);
    if (!got) return -2;
    int sent = -3;
    if (!q_region_is_empty((const char *)fbo + FBO_MAP + MAP_SLEEP)) sent = -4;
    else if (__atomic_load_n((uint8_t *)fbo + FBO_PENDING, __ATOMIC_ACQUIRE)) sent = -5;
    else if (!__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < n; i++) {
            uint64_t region[2] = {0, 0};
            q_region_ctor(region, rs[i].x, rs[i].y, rs[i].w, rs[i].h, 0 /* QRegion::Rectangle */);
            ((send_t)(xo_base + XO_SEND))(fbo, region, wf, 0x11, 0);
            q_region_dtor(region);
        }
        sent = cd_ncr = n;
    }
    pthread_mutex_unlock(lock);
    return sent;
}

// inside one of the companion updates' rectangles?
static int cd_comp_rect(const CdNode *c) {
    for (int i = 0; i < cd_ncr; i++) {
        Rect r = cd_crs[i];
        if (c->x0 >= r.x - 8 && c->y0 >= r.y - 8 && c->x1 < r.x + r.w + 8 && c->y1 < r.y + r.h + 8) return 1;
    }
    return 0;
}

// one of our companion updates (sent early they start with the turn's own, sent late a few frames after it)?
static int cd_is_comp(const CdNode *c, int32_t start) {
    return c->start >= start - 3 && c->start <= start + 16 && cd_comp_rect(c);
}

// 1.8.19: a frame whose own change is below what is worth a co-drive (the bound on_update_line asks with)
static int cd_small(double fe, double fd) {
    return 24.0 * (fe * cfg.cd_gain > fd * cfg.cd_gpos ? fe * cfg.cd_gain : fd * cfg.cd_gpos) < cfg.cd_minv;
}

static void cd_mark_set(long id) {
    if (!id) return;
    // 1.8.14: only upwards (two writers: the helper, and the GUI thread for a turn the helper never took; an older
    // turn's mark written after a newer one looked like a new failure)
    long o = __atomic_load_n(&cd_fail_id, __ATOMIC_ACQUIRE);
    if (id <= o) return;
    // 1.8.15: the stamp first, then the id (a reader that saw the new id with the old stamp judged its age wrongly)
    long ms = (long)(now_s() * 1000.0);
    __atomic_store_n(&cd_fail_ms, ms ? ms : 1, __ATOMIC_RELEASE);
    while (id > o && !__atomic_compare_exchange_n(&cd_fail_id, &o, id, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) ;
}

// 1.8.22: that turn was left without co-drive because of a colour (cd_std 2): set before its cd_mark
static void cd_col_set(long id) {
    long o = __atomic_load_n(&cd_col_id, __ATOMIC_ACQUIRE);
    while (id > o && !__atomic_compare_exchange_n(&cd_col_id, &o, id, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) ;
}

// a page turn went without (all of) its co-drive: it gets the clean instead (cd_fail_tick)
static void cd_mark(void) {
    if (cd_cur_turn) cd_mark_set(cd_cur_id);
}

// 1.8.13: the page turn in hand needs no clean
static void cd_good(void) {
    if (cd_cur_turn && cd_cur_id) __atomic_store_n(&cd_ok_id, cd_cur_id, __ATOMIC_RELEASE);
}

// 1.8.13: how many samples of r (one per 16x16 px) hold a colour in the SWTCON's state buffer: levels 3..26 but for
// the grey 15 / 19 of smoothed edges. Besides the blacks a plain STD leaves at level 2, these are what makes xochitl
// draw a page turn as an STD (its scan at 0x869f78). -1: not readable.
#define XO_STATEP 0x1861f20 // the state buffer's pointer: rows of 1632 words, level in bits 0-4
// 1.8.17: the STD blacks (level 2) count from the page on and only above the page number's own. The document
// toolbar's strip (x < 112) keeps its icons at level 2 from the document's opening or a stock clean until a companion
// update or a TEXT-row turn takes them to 0 (measured 10 Nov: 16,880 px; 1.8.16 stopped at the first of them and so
// kept the stock clean on pages whose own blacks were at 0: the turn after it was an STD again). The page number
// (a UI update, STD row) is 372 px of level 2 on a white page.
#define CD_L2_X0 112
#define CD_L2_MAX 2000
// 1.8.15 (review): step 1 looks at every pixel as xochitl's scan does and stops at the first colour (returns 1).
static int cd_state_colour_step(Rect r, int step) {
    uintptr_t st = 0;
    uint32_t row[1632];
    if (!cd_rd(xo_base + XO_STATEP, &st, sizeof st) || !st) return -1;
    int n = 0, l2 = 0, x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > SW ? SW : r.x + r.w, y1 = r.y + r.h > SH ? SH : r.y + r.h;
    for (int y = y0 + step / 2; y < y1; y += step) {
        if (!cd_rd(st + (uintptr_t)y * sizeof row, row, sizeof row)) return -1;
        for (int x = x0 + step / 2; x < x1; x += step) {
            unsigned l = row[x] & 31;
            if (step == 1 && l == 2) { l2 += x >= CD_L2_X0; continue; } // 1.8.16/17: an STD black, see clean_text_ok
            if (l >= 3 && l <= 26 && l != 15 && l != 19) {
                if (step == 1) return 1;
                n++;
            }
        }
    }
    return step == 1 && l2 > CD_L2_MAX ? 2 : n; // step 1: 0 none, 1 a colour (above), 2 STD blacks on the page
}
static int cd_state_colour(Rect r) { return cd_state_colour_step(r, 16); }

// 1.8.26: how much of the page's colour is real colour. xochitl renders the greys of a scanned black-and-white page
// as BALANCED mixes of the six colour primaries (levels 4, 8, 12 and 16, 20, 24: two triples), so cd_state_colour
// counts thousands of "colour" samples on such a page (109 state dumps: 8,415 - 14,032 of 218,700). A coloured area
// is mostly ONE primary of a triple. Every 16th row is read whole; per cell of an 8 x 4 grid and per triple the
// part that is not balanced is max - min of the three counts; chroma is their sum over the screen. Measured:
// scanned black-and-white pages 6.3 - 6.9 % of their colour samples (0.24 - 0.41 % of the samples read), the colour
// pages of a text book 66.7 - 83.8 % (3.6 - 13.0 %). 1 = read (*px samples, *col of them colour, *chroma unbalanced).
#define CH_STEP 16
#define CH_BANDS 8
#define CH_COLS 4
static int cd_state_chroma(long *px, long *col, long *chroma) {
    uintptr_t st = 0;
    uint32_t row[1632];
    static const signed char idx[32] = {[4] = 1, [8] = 2, [12] = 3, [16] = 4, [20] = 5, [24] = 6};
    int cnt[CH_BANDS][CH_COLS][7];
    memset(cnt, 0, sizeof cnt);
    if (!cd_rd(xo_base + XO_STATEP, &st, sizeof st) || !st) return 0;
    int cw = (SW + CH_COLS - 1) / CH_COLS, bh = (SH + CH_BANDS - 1) / CH_BANDS;
    long n = 0;
    for (int y = CH_STEP / 2; y < SH; y += CH_STEP) {
        if (!cd_rd(st + (uintptr_t)y * sizeof row, row, sizeof row)) return 0;
        int (*b)[7] = cnt[y / bh];
        for (int x = 0; x < SW; x++) b[x / cw][idx[row[x] & 31]]++;
        n += SW;
    }
    long c = 0, ch = 0;
    for (int i = 0; i < CH_BANDS; i++)
        for (int j = 0; j < CH_COLS; j++) {
            const int *h = cnt[i][j];
            for (int k = 1; k <= 6; k++) c += h[k];
            for (int k = 1; k <= 4; k += 3) {
                int mx = h[k], mn = h[k];
                for (int m = 1; m < 3; m++) {
                    if (h[k + m] > mx) mx = h[k + m];
                    if (h[k + m] < mn) mn = h[k + m];
                }
                ch += mx - mn;
            }
        }
    *px = n; *col = c; *chroma = ch;
    return 1;
}

// 1.8.15: the clean with TEXT targets. The stock clean (swapBuffers_impl, flags 0x1) is {wf 2, pixel mode 0x11,
// flags 1}: every pixel is driven in place to the STD row's levels (white 28, black 2). xochitl draws a page turn
// as a GLR only while the page holds no level 2..26 but 15/19 (0x869f78), so the turn after every clean of a page
// with black on it was a flashing STD (measured: "page turn drawn as wf 2 (len 85)" after each clean; in the
// STD table every transition is a full drive, 28->28 included; 1.8.18: but a pixel that keeps its level does not
// take that row, it is held through the hold column at 0 V like the GLR's: see "1.8.18" below). The same update with flags bit 3 (0x9) takes the TEXT row for its targets (black 0, white 30, greys
// 15/19 as in the STD row, colour codes: hold) and the same STD waveform: measured on the panel 10 Nov (send 2 17 9,
// pages 2/3, 8/9, 10/11 and the library, 25.8 C) it cleans as evenly as the stock clean (white sd 3.3 vs 3.5 per
// mille) and the next turn is a normal co-driven GLR. xochitl sends this update itself ("wf2 + row 3", 0x86a370).
// Only where it is the same clean: nothing to draw as Mono, not the presentation mode, below the TMODE
// temperature (the stock path arms the TMODE there), sendUpdate as verified, and no colour in the rectangle's
// states (the TEXT row holds colour pixels: a colour page keeps the stock clean).
static int clean_text_ok(void *fbo, Rect r) {
    if (memcmp((const void *)(xo_base + XO_SEND), xo_send_sig, sizeof xo_send_sig)) return 0;
    if (!q_region_is_empty((const char *)fbo + FBO_MAP + MAP_MONO)) return 0;
    if (__atomic_load_n((uint8_t *)fbo + FBO_PRESENT, __ATOMIC_RELAXED) == 1) return 0;
    double tc = xo_temp();
    if (isnan(tc) || tc >= 28.0) return 0;
    // review: every pixel, not the samples. A thin coloured line the samples miss would be held (not cleaned) and
    // would make the next turn an STD although the model expects a GLR. Unreadable (-1): the stock clean.
    // 1.8.16: nor on a black the STD row left at level 2 (a stock clean or a turn away from a colour page, no redraw
    // since). The TEXT row would send it to level 0 and the STD table's 2->0 is the full black pulse (+564 V x
    // frames, the same as 30->0; 2->2 is +6): an already black pixel driven black once more. Measured 10 Nov: the
    // next page then shows that page's ink 2.3 % deep (v2a) against 0.6..1.1 % for every other order of cleans. Such
    // a page keeps the stock clean; xochitl's next turn ("wf2 + row 3") brings the page to the TEXT levels itself.
    // 1.8.17: counted on the page only (CD_L2_X0, CD_L2_MAX): the toolbar's icons and the page number do not decide.
    // While a turn is running the states already hold its targets (sampled 10 Nov), so the clean right after a
    // TEXT-row turn sees the page at level 0.
    int blk = cd_state_colour_step(r, 1);
    __atomic_store_n(&last_text_block, blk, __ATOMIC_RELAXED);
    return blk == 0;
}
static void clean_text_send(void *fbo, const void *region) {
    ((send_t)(xo_base + XO_SEND))(fbo, region, 2, 0x11, 0x9);
}

static void cd_failed(int what) {
    __atomic_fetch_add(&n_cd[what], 1, __ATOMIC_RELAXED);
    cd_mark();
}

// 1.8.18: the co-drive of an STD page turn (2026-11-10). A page with colour on it is turned as an STD (wf 2, 86
// frames at 25 C), and that update is a partial one like the GLR's: a pixel that keeps its level (about half of a
// colour page turn; measured 30->30 43 %, 28->28 7 %) goes through the hold column (to 27 / 29) at 0 V while the
// rest is flashed, so it takes the common electrode's swing in full. Measured 10 Nov at 25.6 C on colour PDF A: the
// held white ends 10.5 % darker after one turn (the old page's negative: "only a hard flash clears it"), 0.1 % with
// the hold cells of from 28..31 written as gain x the mean drive (1.0 towards white, 0.4 towards dark of the page
// rectangle's mean; here 1.07 / 0.43 of the whole screen's). The note of 1.8.13 that an STD turn holds no pixel was
// wrong. Unlike the GLR's, the STD's mean drive depends on every pixel's level before and after (each transition has
// its own row), so:
//  - cd_request (GUI thread, before the update is queued: the state buffer still holds the old page) samples the
//    levels (cds_req) when the old page holds a colour (xochitl then takes the STD) and queues the strip's companion
//    as a wf 2 no-change update in the turn's batch (flags 0: its levels stay as they are);
//  - the helper finds the turn's node (progress ~5), samples the levels again (now the new page's targets), counts
//    the transitions, takes the mean drive per frame from the table's own rows and writes the co-drive from the frame
//    the generator has not reached yet (cds_lead) into the same eight hold cells per frame group as for a GLR. The
//    missed frames' dose (the first ~10 of the +7..+12 V phase; the white phase that makes the trace is frames
//    38-59) moves to the frames after them;
//  - the cells go back as soon as the last written frame has passed (the page number's update reads the same table
//    later), and the verdict is given at once: co-driven (no clean), or the turn keeps the rule of 1.8.17.
// Left to that rule (the page clean): a turn whose old page showed no colour sample (xochitl is expected to take the
// GLR; a b/w -> colour turn is an STD nobody sampled for), no companion in the turn's batch, a table that is not
// the STD we know, levels that cannot be read twice the same, a change too small to drive, another update reading
// the table, a turn that waits behind another update (its companion ran alone). Held pixels that are not white (from
// below 28) are not driven. cd_std 0 switches all of this off.
static uint8_t cds_req[CDS_N]; // under cd_mu: the levels before the waiting turn (GUI thread)
static int cds_req_ok;         // under cd_mu: cds_req belongs to the waiting request
static uint8_t cds_from[CDS_N], cds_to[CDS_N], cds_to2[CDS_N]; // helper
static uint16_t cds_cells[CD_NG * 1025];                       // helper: the turn's table
static uint32_t cds_hist[1024];                                // helper
#define CDS_TAIL 8                                             // frames at the end of an STD turn that get no code
static int cds_run;                                            // helper: co-driven STD turns since the last one that failed (cds_every)
static int cds_wt_n;                                           // helper: rows that got the white tail in the turn in hand (1.8.23)
static int cds_flip_on;                                        // helper: the turn in hand has its codes in the flip rows too (1.8.25)
static int cds_flip_seen;                                      // helper: the turn in hand found the flip rows at 0 V (cdf_hold)

// the sampled levels of the state buffer as it is right now; 0: not readable
static int cds_read(uint8_t *lv) {
    uintptr_t st = 0;
    uint32_t row[1632];
    if (!cd_rd(xo_base + XO_STATEP, &st, sizeof st) || st < 0x10000 || (st & 3)) return 0;
    for (int j = 0; j < CDS_NY; j++) {
        if (!cd_rd(st + (uintptr_t)cds_y(j) * sizeof row, row, sizeof row)) return 0;
        cds_row(row, j, lv + (size_t)j * CDS_NX);
    }
    return 1;
}

// Writes the co-drive of the STD turn mn (seen at tseen; levels before it in cds_from) into its table's hold cells.
// 1 armed. Otherwise nothing is written: 0 too small to drive, -1 older cells could not be put back, -2 not a table
// of the node's length, -3 table unreadable, -4 not the STD rows we know, -5 levels unreadable, -6 levels not
// steady, -7 the node left the list, -8 a cell could not be written, -9 found too late (1.8.19: no frame left to
// write; 0 is only "needs none").
static int cds_arm(uintptr_t g, const CdNode *mn, double tseen, int *frames, int *qlast, int *wlastp, int *firstp, double *chg, double *peakp) {
    uint8_t hb[0x20];
    uint32_t nfr, n32, three;
    uintptr_t tab;
    unsigned gen = 0; // 1.8.25: the hold that has the table's flip rows at 0 V (0: none)
    cds_flip_on = cds_flip_seen = 0;
    if ((cd_nsv || __atomic_load_n(&cdf_nfsv, __ATOMIC_RELAXED)) && (cd_restore_chk() || cd_nsv)) return -1;
    if (!cd_rd(mn->tab, hb, sizeof hb)) return -2;
    memcpy(&nfr, hb, 4); memcpy(&n32, hb + 4, 4); memcpy(&three, hb + 0xc, 4); memcpy(&tab, hb + 0x18, 8);
    if (n32 != 32 || three != 3 || nfr < 8 || nfr >= CD_MAXF || (int)nfr != mn->len + 1 || tab < 0x10000 || (tab & 1)) return -2;
    int nf = (int)nfr;
    if (!cdf_read(tab, nf, cds_cells, &gen)) return -3;
    // 1.8.25: flip rows at 0 V (cdf_hold wrote them for this turn) drive nothing: the mean below is the turn's without
    // them, and they are hold cells like the others (they get the codes too, cdf_put)
    cds_flip_seen = gen != 0;
    // an erase row, a draw row, and hold cells that are free in every frame
    if (cds_rowsum(cds_cells, nf, (0 << 5) | 30) >= -100 || cds_rowsum(cds_cells, nf, (30 << 5) | 0) <= 100) return -4;
    for (int from = 28; from < 32; from++)
        if (cds_rowabs(cds_cells, nf, (from << 5) | 27)) return -4;
    // the new page's levels: the state buffer holds the update's targets once its node shows; read twice the same
    if (!cds_read(cds_to) || !cds_read(cds_to2)) return -5;
    if (memcmp(cds_to, cds_to2, sizeof cds_to)) {
        usleep(2000);
        if (!cds_read(cds_to)) return -5;
        if (memcmp(cds_to, cds_to2, sizeof cds_to)) return -6;
    }
    double m[CD_MAXF], c[CD_MAXF], cg[CD_MAXF];
    static int8_t cds_q[4][CD_MAXF]; // helper thread only
    static int8_t cds_wq[CD_MAXF];   // 1.8.23, helper thread only: the white tail's codes and rows
    static uint16_t cds_wrows[CDS_WT_ROWS];
    // no code in the turn's last CDS_TAIL frames (the table drives next to nothing there): the page number's update
    // follows on this table as soon as the turn has ended, and the cells must be back before it reads them
    int end = mn->len - CDS_TAIL;
    if (end > nf) end = nf;
    cds_histo(cds_from, cds_to, CDS_N, cds_hist);
    // 1.8.23: the white tail goes into the copy of the table first (the co-drive below follows the rows as they will
    // be driven), and only for a turn found long before the tail's frames
    int wt = __atomic_load_n(&cfg.cds_wt, __ATOMIC_RELAXED), wlast = -1, wrows = 0;
    if (__atomic_load_n(&cfg.wt_static, __ATOMIC_RELAXED) > 0) wt = 0; // 1.8.30: the tail stands in the table (wts_apply), none per turn
    int lead = __atomic_load_n(&cfg.cds_lead, __ATOMIC_RELAXED);
    cds_wt_n = 0;
    if (wt > 0 && mn->prog + (int)ceil((now_s() - tseen) / 0.01176) + lead + 26 < CDS_WT_I0) {
        wlast = cds_wt_q(nf, end, wt, cds_wq);
        if (wlast >= 0) wrows = cds_wt_mark(cds_cells, nf, cds_wq, cds_wrows);
        if (!wrows) wlast = -1;
    }
    cds_mean(cds_hist, (double)CDS_N, cds_cells, nf, m, chg);
    *peakp = cds_codrive(m, nf, cfg.cds_gain, cfg.cds_gpos, c);
    if (*peakp < cfg.cd_minv) return 0;
    // where the generator is by now: the node's progress, or what the clock says when the list cannot be read
    CdNode nd[CD_MAXN];
    int p = mn->prog + (int)ceil((now_s() - tseen) / 0.01176), n = cd_walk(g, nd, CD_MAXN), here = n < 0;
    for (int i = 0; i < n; i++)
        if (nd[i].node == mn->node && nd[i].start == mn->start) { here = 1; if (nd[i].prog > p) p = nd[i].prog; }
    if (!here) return -7;
    int first = p + lead, last = -1;
    if (end <= first) return -9;
    if (wlast >= 0 && first + 4 > CDS_WT_I0) return -9; // the tail the mean counts on could not be written in time
    // 1.8.21: each group of hold cells gets its own shape of the drive (all the same with the default keys); the
    // frames counted and the carry are those of the margins' cells (from 30, column 27)
    double acc = cd_acc;
    const int8_t *qs[4];
    int nq = 0, lvm = cds_lvm(), carry = cfg.cds_carry;
    for (int i = 0; i < 4; i++) {
        double a = cd_acc;
        cds_shape(c, nf, i, cg);
        int n = cd_quant_m(cg, end, first, lvm, carry, &a, cds_q[i]);
        for (int f = end; f < nf; f++) cds_q[i][f] = 0;
        for (int f = 0; f < end; f++)
            if (cds_q[i][f] && f > last) last = f;
        if (!i) { nq = n; acc = a; }
        qs[i] = cds_q[i];
    }
    if (last < 0) return 0;
    if (cd_put4(tab, nf, qs) < 0) return -8;
    if (wlast >= 0) { // 1.8.23: the tail's cells, 60 frames ahead of the generator; they go back with the hold cells
        if (cds_wt_put(tab, nf, cds_wrows, wrows, cds_wq) < 0) return -8;
        cds_wt_n = wrows;
    }
    if (gen) { // 1.8.25: from 28 as the hold group of 28 / 29 (column 27), from 30 as that of 30 / 31
        int fr = cdf_put(tab, nf, gen, qs[2], qs[0]);
        if (fr < 0) { cd_restore_chk(); return -8; } // the hold cells and the tail go back too
        cds_flip_on = fr > 0; // 0: that hold is over (a complete refresh came): the rows are stock again
    }
    cd_acc = acc > 3.0 ? 3.0 : acc < -3.0 ? -3.0 : acc; // only what was really written carries over
    *frames = nq; *qlast = last; *wlastp = wlast; *firstp = first;
    return 1;
}

// 1.8.19: an update in its last frames on the turn's table (the one before a turn's later frame: that frame's update
// starts as it ends). It reads the frames from len - CDS_TAIL on only, and those never get a code (the generator is
// ahead of the list's progress, never behind it).
static int cds_ending(const CdNode *c, const CdNode *mn) {
    return c->len == mn->len && c->prog >= c->len - CDS_TAIL;
}

// The STD page turn mn (nd0[n0]: the list it was found in). 1: dealt with here (co-driven and confirmed, needs none,
// or marked for the clean); -1: nothing was written, the caller's rule decides. small: the frame's own change is
// below what is worth a co-drive (the request's fe / fd).
static int cds_turn(uintptr_t g, const CdNode *mnp, const CdNode *nd0, int n0, int early, double tseen, int hwhy, int small) {
    CdNode mn = *mnp, nd[CD_MAXN];
    // the table is shared: another update reading it with its own frame counter must not see our cells
    for (int i = 0; i < n0; i++)
        if (nd0[i].node != mn.node && nd0[i].tab == mn.tab && (nd0[i].start < mn.start - 1 || nd0[i].start > mn.start + 1) &&
            !cds_ending(&nd0[i], &mn)) {
            glog("codrive: STD turn node %d,%d-%d,%d len %d not co-driven: node %d,%d-%d,%d (start %+d, at %d of %d) reads the table",
                 mn.x0, mn.y0, mn.x1, mn.y1, mn.len, nd0[i].x0, nd0[i].y0, nd0[i].x1, nd0[i].y1,
                 (int)(nd0[i].start - mn.start), nd0[i].prog, nd0[i].len);
            return -1;
        }
    int frames = 0, qlast = 0, wlast = -1, first = 0;
    double chg = 0.0, peak = 0.0;
    int r = cds_arm(g, &mn, tseen, &frames, &qlast, &wlast, &first, &chg, &peak);
    if (r == 0 && (small || cds_flip_seen)) {
        // 1.8.19: the levels before and after and the frame's own change agree: next to nothing to drive against
        // (a turn's later frame that draws the same tiles again). Needs none, like a small GLR (CD_SMALL).
        // 1.8.25: the same with the flip rows at 0 V: what is left of the turn's mean drive is below cd_minv (the
        // flips were three quarters of it), and that must not be taken for a turn that failed (the page clean).
        __atomic_fetch_add(&n_cd[CD_SMALL], 1, __ATOMIC_RELAXED);
        cd_good();
        glog("codrive: STD turn node %d,%d-%d,%d len %d needs no co-drive (changed %.3f, peak %.1f V%s)",
             mn.x0, mn.y0, mn.x1, mn.y1, mn.len, chg, peak, cds_flip_seen ? "; flips held" : "");
        return 1;
    }
    if (r <= 0) {
        glog("codrive: STD turn node %d,%d-%d,%d len %d not co-driven (%d: changed %.3f, peak %.1f V)",
             mn.x0, mn.y0, mn.x1, mn.y1, mn.len, r, chg, peak);
        return -1;
    }
    // hold until the last written frame has passed; put the cells back at once when another update starts to read
    // the table
    double t1 = now_s(), lim = mn.len * 0.0125 + 0.5;
    // 1.8.23: the white tail may end after the last hold code (wlast, -1: no tail): the cells stay until both have
    // passed. Whether another update took something from the co-drive is still judged by the hold cells' last frame
    // (qlast): one that starts between the two only cuts the tail short, and that needs no clean.
    int until = (wlast > qlast ? wlast : qlast) + 1; // at most len - CDS_TAIL
    int aborted = 0, cut = 0, next = 0, main_here = 1, unknown = 0, timed = 0, done = 0, lastprog = mn.prog, coff = 99;
    for (;;) {
        usleep(2000);
        int n = cd_walk(g, nd, CD_MAXN);
        if (n < 0) {
            if (++unknown >= 3) { aborted = 1; break; } // the list cannot be read: nobody can tell who reads the table
            continue;
        }
        unknown = 0;
        main_here = 0;
        for (int i = 0; i < n; i++) {
            if (nd[i].tab != mn.tab) continue;
            if (nd[i].node == mn.node && nd[i].start == mn.start) { main_here = 1; lastprog = nd[i].prog; continue; }
            if (nd[i].start >= mn.start - 1 && nd[i].start <= mn.start + 1) { // the turn's own other updates
                if (early > 0 && coff == 99 && cd_comp_rect(&nd[i])) coff = nd[i].start - mn.start;
                continue;
            }
            if (nd[i].start < mn.start && cds_ending(&nd[i], &mn)) continue; // 1.8.19: it reads no frame with a code
            // any other update reads the cells with its own frame counter: put them back at once. One that is not yet
            // at its first coded frame and either started after our last hold code (the list lags the generator by
            // a few frames) or shows up when all of them were read took nothing from the turn and read nothing of ours.
            if (nd[i].prog < first && ((nd[i].start > mn.start + qlast + 1 && lastprog >= qlast - 6) || lastprog > qlast + 1)) next = 1; else cut = 1;
        }
        if (cut) break;
        if (lastprog > until || next) { done = 1; break; }
        if (!main_here) break;
        if (now_s() - t1 > lim) { timed = 1; break; }
        if (!__atomic_load_n(&cfg.codrive, __ATOMIC_RELAXED) || __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) { aborted = 1; break; }
    }
    int cells = cd_nsv, bad = cd_restore_chk();
    // the strip's companion must have run frame for frame with the turn (same batch)
    int compok = early <= 0 || (coff >= -1 && coff <= 1);
    // 1.8.23: the turn got its co-drive when every hold code was read (1.8.22's rule: the list's progress past the
    // last of them), however the wait for the tail ended (the node gone, the list unreadable, the time limit, co-drive
    // switched off): only the tail was cut short then. An update that read our cells (cut) still gets the page clean.
    int holdok = lastprog > qlast + 1;
    int whole = (done || (holdok && !cut)) && !bad && compok, valve = 0;
    if (whole) {
        int every = __atomic_load_n(&cfg.cds_every, __ATOMIC_RELAXED);
        if (cd_cur_turn != 2) cd_std_run = cd_std_odd = 0; // 1.8.19: a later frame takes no part in the rule's counts
        __atomic_fetch_add(&n_cd[CD_STDCD], 1, __ATOMIC_RELAXED);
        if (every > 0 && ++cds_run >= every) { cds_run = 0; valve = 1; cd_mark(); }
        else cd_good();
    } else {
        cds_run = 0;
        cd_failed(CD_ABORT);
    }
    __atomic_store_n(&cd_last, frames, __ATOMIC_RELAXED);
    glog("codrive: STD turn node %d,%d-%d,%d len %d seen at %d, first frame %d, last %d: changed %.3f, peak %.1f V, %d x 6 V frames, %d cells (white tail: %d rows%s), companion %d (h%d, %+d), held %.0f ms to frame %d%s%s%s",
         mn.x0, mn.y0, mn.x1, mn.y1, mn.len, mn.prog, first, until - 1, chg, peak, frames, cells, cds_wt_n,
         cds_flip_on ? "; flips co-driven" : cds_flip_seen ? "; flips held" : "", early, hwhy, coff,
         (now_s() - t1) * 1000.0, lastprog,
         cut ? ", cut short by another update" : timed ? ", time limit" : aborted ? ", stopped (list unreadable, co-drive off or sleep)" : !done ? ", the turn ended early" : !compok ? ", companion out of step" : next ? ", ended by the next update" : "",
         !done && holdok && !cut ? " after the last hold code (white tail cut short)" : "",
         whole ? (valve ? ": co-driven, the page clean is due (cds_every)" : ": co-driven, no clean") : ": gets the page clean");
    return 1;
}

static void *cd_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    uintptr_t last_node = 0;
    int32_t last_start = 0;
    for (;;) {
        if (cd_nsv || __atomic_load_n(&cdf_nfsv, __ATOMIC_RELAXED)) cd_restore_chk(); // nothing stays written while the helper waits
        for (int tr = 0; cd_nsv && tr < 50; tr++) { usleep(20000); cd_restore(); } // 1.8.10 (the co-drive is off by now)
        pthread_mutex_lock(&cd_mu);
        cd_busy = 0;
        while (cd_reqn == seen) {
            // 1.8.25: while the flip rows are at 0 V the helper looks every 50 ms whether they are due back (cdf_tick)
            if (!__atomic_load_n(&cdf_zon, __ATOMIC_ACQUIRE)) { pthread_cond_wait(&cd_cv, &cd_mu); continue; }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&cd_cv, &cd_mu, &ts);
            if (cd_reqn == seen) cdf_tick();
        }
        seen = cd_taken = cd_reqn;
        double fe = cd_fe, fd = cd_fd, acc0 = cd_pre_acc;
        Rect rb = cd_bb;
        int pre = cd_pre, pframes = cd_pre_frames, early_ok = cd_early_ok, here_n = cd_here_n, hwhy = cd_here_why;
        Rect hrs[4];
        memcpy(hrs, cd_here_rs, sizeof hrs);
        cd_here_n = 0;
        int cwf = here_n > 0 ? cd_here_wf : 4, snap = cds_req_ok; // 1.8.18: the companions' waveform, the old page's levels
        if (snap) memcpy(cds_from, cds_req, sizeof cds_from);
        cds_req_ok = 0;
        uintptr_t ptab = cd_pre_tab;
        cd_pre = 0;
        cd_busy = 1;
        cd_cur_turn = cd_turn;
        cd_cur_id = cd_req_id;
        int stdn = cd_req_stdn, fcol = cd_req_col, textn = cd_req_textn;
        pthread_mutex_unlock(&cd_mu);
        cd_ncr = 0;
        // 1.8.19: a later frame of the turn that has the id; and a frame whose own change is below what is worth a
        // co-drive
        int fup = cd_cur_turn == 2, small = cd_small(fe, fd);
// the pre-armed cells go back unused: the carry is what it was before them
#define CD_DROP_PRE() do { cd_restore_chk(); cd_acc = acc0; pre = 0; } while (0)
        if (pre == 2) CD_DROP_PRE(); // written for an update that was replaced: not this one's shape
        if (!__atomic_load_n(&cfg.codrive, __ATOMIC_RELAXED) || __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) {
            if (pre) CD_DROP_PRE();
            continue;
        }
        uintptr_t g = cd_g();
        if (!g) { if (pre) CD_DROP_PRE(); cd_failed(CD_BAD); continue; }
        // 1.8.10: the companions of a pre-armed page turn go out now, right behind the turn's own update (the GUI
        // thread is still inside its swapBuffers: the framebuffer lock orders us behind it)
        // 1.8.11: that swapBuffers holds the lock for longer than the 17 ms 1.8.10 waited (it never sent one early):
        // keep trying for 120 ms; the lock comes free the moment the turn's update is queued.
        int early = 0, ewhy = pre != 1 ? 8 : !early_ok ? 9 : 7, coff = 99;
        double ewait = 0;
        if (here_n > 0) { // 1.8.12: queued by the GUI thread with the turn's own update
            memcpy(cd_crs, hrs, sizeof hrs);
            cd_ncr = early = here_n;
            ewhy = 1;
            __atomic_fetch_add(&n_cd[CD_EARLY], 1, __ATOMIC_RELAXED);
        } else if (pre == 1 && early_ok && __atomic_load_n(&cfg.cd_comp, __ATOMIC_RELAXED) && __atomic_load_n(&cfg.cd_early, __ATOMIC_RELAXED)) {
            double te = now_s();
            for (;;) {
                early = cd_companion(rb, 4);
                ewait = now_s() - te;
                if (early != -2 || ewait > 0.3 || __atomic_load_n(&cd_reqn, __ATOMIC_RELAXED) != seen) break;
            }
            ewhy = early < 0 ? -early : 0;
            if (early < 0) early = 0; // not possible right now: they go out once the node shows, as before
            else if (early) __atomic_fetch_add(&n_cd[CD_EARLY], 1, __ATOMIC_RELAXED);
        }
        // the request's update: the largest new GLR / STD node over the request's box, caught in its first frames.
        // It may wait behind the update before it (overlapping updates run one after the other).
        CdNode nd[CD_MAXN], mn, bn;
        int found = 0, n = 0, newer = 0, off = 0, nfor = 0, lone = 0, unk = 0, nbusy = 0;
        long minarea = cfg.cd_area;
        double t0 = now_s(), tb = t0, tfor = 0, tlone = 0, tseen = t0;
        for (;;) {
            if (__atomic_load_n(&cd_reqn, __ATOMIC_RELAXED) != seen) { newer = 1; break; }
            if (!__atomic_load_n(&cfg.codrive, __ATOMIC_RELAXED) || __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) { off = 1; break; }
            n = cd_walk(g, nd, CD_MAXN);
            tseen = now_s();
            long best = 0;
            int big = n < 0, foreign = 0, alone = 0, busy = 0;
            for (int i = 0; i < n; i++) {
                if (!cd_node_ok(&nd[i])) continue;
                long area = cd_area_of(&nd[i]);
                int mine = early > 0 && nd[i].wf == cwf && cd_comp_rect(&nd[i]); // a companion we sent at the wake
                int large = !mine && (nd[i].wf == 4 || nd[i].wf == 2) && area >= minarea;
                // caught in its first frames; a pre-armed turn's node at any progress (its cells were there from frame 0)
                int cand = large && (nd[i].prog <= 6 || (pre && nd[i].tab == ptab)) && cd_hits(&nd[i], rb) &&
                           !(nd[i].node == last_node && nd[i].start == last_start);
                if (large) big = 1;
                if (mine) alone = 1;
                else if (pre && !cand && nd[i].tab == ptab) foreign = 1;
                // 1.8.24: any other update that still runs (see below)
                if (!mine && !cand && nd[i].prog < nd[i].len) { busy = 1; bn = nd[i]; }
                if (!cand || area <= best) continue;
                best = area; mn = nd[i]; found = 1;
            }
            if (found) break;
            // another update reads the table and the turn's own does not show: it must not see our cells
            // (1.8.11: by time, 20 ms: the turn's own update may show a frame after a small one that started with it)
            double t = now_s();
            if (!foreign) tfor = 0; else if (tfor == 0) tfor = t;
            nfor = foreign && t - tfor >= 0.02 ? 3 : 0;
            if (pre && nfor >= 3) CD_DROP_PRE();
            unk = n < 0 ? unk + 1 : 0;
            if (pre && unk >= 3) CD_DROP_PRE(); // the list cannot be read: nobody can tell who reads the table
            // our companions run and the turn's update does not (it waits behind another update): they must not run
            // the cells alone
            // (1.8.11: by time, 45 ms: the two may enter the list a frame or two apart)
            if (!alone) tlone = 0; else if (tlone == 0) tlone = t;
            lone = alone && t - tlone >= 0.045 ? 3 : 0;
            if (lone >= 3) break;
            // 1.8.24: a page turn's update also waits behind a small update that still runs: xochitl hides the page
            // number 3.5 - 4.4 s after a turn (258x72, an STD of 85 frames), and a turn made while that runs enters
            // the list up to 1 s after its frame (measured 11 Nov: 500 ms at one turn per 4.45 s). 1.8.23 gave up
            // after 250 ms, the turn ran without co-drive: a clean on two of three such turns, a blue page on the
            // third. Only for a page turn; the 1.5 s limit holds.
            if (cd_cur_turn && busy) nbusy++;
            if (big || alone || (cd_cur_turn && busy)) tb = t;
            if (t - tb > 0.25 || t - t0 > 1.5) break;
            usleep(1000);
        }
        if (newer || off) { // a newer request: its update replaced ours or follows it
            if (pre) CD_DROP_PRE();
            if (newer) {
                // 1.8.10: a turn that is given up went without its co-drive
                // 1.8.19: but for a later frame that changes next to nothing, whatever the newer request is: nothing
                // was written for it and nothing was needed
                int quiet = fup && small;
                if (!quiet) cd_mark();
                if (cd_cur_turn)
                    glog("codrive: page turn %ld%s (fe %.3f fd %.3f) given up for a newer request after %.0f ms%s", cd_cur_id,
                         fup ? " (later frame)" : "", fe, fd, (now_s() - t0) * 1000.0,
                         quiet ? ": next to no change, nothing missed" : ": marked");
            }
            continue;
        }
        if (!found) {
            if (pre) CD_DROP_PRE();
            // 1.8.19: a turn's later frame that changes next to nothing draws nothing large (the same tiles again):
            // nothing was written for it (no pre-arm) and nothing was needed
            int quiet = fup && small && lone < 3;
            if (quiet) __atomic_fetch_add(&n_cd[CD_MISS], 1, __ATOMIC_RELAXED);
            else cd_failed(lone >= 3 ? CD_ABORT : CD_MISS);
            if (cd_cur_turn)
                glog("codrive: page turn %ld%s (fe %.3f fd %.3f): no update of its own found in %.0f ms (%s; companion %d, h%d)%s",
                     cd_cur_id, fup ? " (later frame)" : "", fe, fd, (now_s() - t0) * 1000.0,
                     lone >= 3 ? "its companion ran alone" : n < 0 ? "the list cannot be read" : "none in its first frames",
                     early, hwhy, quiet ? ": next to no change, nothing missed" : ": marked");
            continue;
        }
        last_node = mn.node; last_start = mn.start;
        if (cd_cur_turn && nbusy && tseen - t0 > 0.25)
            glog("codrive: page turn %ld waited %.0f ms for its update behind another one (last: %d,%d-%d,%d wf %d, at %d of %d)",
                 cd_cur_id, (tseen - t0) * 1000.0, bn.x0, bn.y0, bn.x1, bn.y1, bn.wf, bn.prog, bn.len);
        // 1.8.14: cd_std_next is not cleared here any more: only a page turn takes it (cd_request). Cleared for any
        // large update (a menu closing after a clean), the expected STD turn after the clean counted as unexplained
        // and got a second clean.
        if (mn.wf != 4) { // not a GLR: nothing of ours in its table
            if (pre) CD_DROP_PRE();
            __atomic_fetch_add(&n_cd[CD_STD], 1, __ATOMIC_RELAXED);
            // 1.8.18: the STD turn away from a page with colour is co-driven from the levels sampled before and after it
            // (cds_turn). It needs the old page's samples, and the strip's companion in the turn's batch (or no strip:
            // h0) unless companions are off; whatever it does not take keeps the rule below.
            // 1.8.19: whatever the old page held (the levels are sampled for every turn's frame); and "no strip" is
            // seen from the request's box itself where nobody tried to queue a companion (h7-h9: no pre-arm, a full
            // refresh before the turn, companions not in the batch)
            Rect nrs[4];
            int nostrip = cd_comp_rects(rb, nrs) == 0 && hwhy >= 7;
            int compin = early > 0 ? cwf == 2 : (hwhy == 0 || nostrip || !__atomic_load_n(&cfg.cd_comp, __ATOMIC_RELAXED));
            // 1.8.22: cd_std 2 co-drives it only between two pages without colour (the co-driven turn left the old
            // page's letters on a colour page): colour before (fcol) or after it keeps the rule below, so the clean
            int cstd = __atomic_load_n(&cfg.cd_std, __ATOMIC_RELAXED);
            int ccol = cstd == 2 && cd_cur_turn && mn.wf == 2 &&
                       (fcol > 0 || cd_state_colour((Rect){mn.x0, mn.y0, mn.x1 - mn.x0 + 1, mn.y1 - mn.y0 + 1}) > 0);
            if (mn.wf == 2) wts_apply(mn.tab); // 1.8.30: the white tail stands in the measured table (or is taken out)
            if (mn.wf == 2) __atomic_store_n(&cdf_obj, mn.tab, __ATOMIC_RELAXED); // 1.8.25: the STD table object in use (cdf_hold, at the next turn)
            if (cd_cur_turn && mn.wf == 2 && snap && compin && cstd && !ccol &&
                cds_turn(g, &mn, nd, n, early, tseen, hwhy, small) > 0)
                continue;
            if (ccol && cd_cur_id) cd_col_set(cd_cur_id); // 1.8.22: its clean decides by turn_px_col
            if (fup) {
                // 1.8.19: a turn's later frame takes no part in the rule's counts: its own change decides
                if (small) cd_good(); else cd_mark();
                glog("codrive: page turn's later frame (fe %.3f fd %.3f) drawn as wf %d (len %d), not co-driven: %s (colour samples %d before; companion %d, h%d)",
                     fe, fd, mn.wf, mn.len, small ? "next to no change, passes" : "gets the page clean", fcol, early, hwhy);
            } else if (cd_cur_turn) {
                // 1.8.13: an STD page turn holds its unchanged pixels at 0 V like the GLR and gets no co-drive: it keeps
                // the page clean. The one exception stops the clean from following every turn: a clean (a plain STD)
                // leaves its blacks at level 2, which makes the next turn an STD too (it puts them back to level 0, the
                // turn after is a GLR again). That one black and white STD right after a full refresh passes (as it
                // did in the chains measured with 1.8.12). Not while black and white STDs keep coming with no full
                // refresh before them (at 33 C and more xochitl draws every turn as an STD): from the second such
                // turn on every STD is cleaned, but for one in eight that passes to see whether the GLR is back.
                int tcol = fcol ? fcol : cd_state_colour((Rect){mn.x0, mn.y0, mn.x1 - mn.x0 + 1, mn.y1 - mn.y0 + 1});
                // 1.8.14: a state buffer that cannot be read (-1) is no colour: such a turn falls under the black and
                // white rule and its limiter (it counted as colour: every turn got the clean, for ever)
                int bw = fcol <= 0 && tcol <= 0;
                // 1.8.15 (review): the first turn after our clean with TEXT targets is expected as a GLR. Where it is an
                // STD all the same (the new page holds a colour the samples miss, 33 C reached meanwhile, ...) it passes
                // like the first turn after any full refresh, as it did up to 1.8.14: never a clean after every turn.
                int after = stdn || textn;
                if (bw && !after) cd_std_odd++;
                int pass = bw && after && (cd_std_odd < 2 || cd_std_run % 8 == 0);
                cd_std_run++;
                if (pass) cd_good(); else cd_mark();
                glog("codrive: page turn drawn as wf %d (len %d): %s (colour samples %d before, %d after; %s; STD turns in a row %d, unexplained %d)",
                     mn.wf, mn.len, pass ? "passes" : "gets the page clean", fcol, tcol,
                     stdn ? "first turn after a full refresh" : textn ? "first turn after a clean with TEXT targets" : "no full refresh before it",
                     cd_std_run, cd_std_odd);
            }
            continue;
        }
        if (cd_cur_turn == 1) cd_std_run = cd_std_odd = 0; // 1.8.19: not a later frame's GLR
        if (fup && small) { // 1.8.19: a later frame that changes next to nothing needs none (cd_arm would find the same)
            __atomic_fetch_add(&n_cd[CD_SMALL], 1, __ATOMIC_RELAXED);
            cd_good();
            glog("codrive: page turn's later frame (GLR node %d,%d-%d,%d len %d): next to no change, needs none",
                 mn.x0, mn.y0, mn.x1, mn.y1, mn.len);
            continue;
        }
        int other = 0; // the table is shared: another update reading it with its own frame counter must not see our cells
        for (int i = 0; i < n; i++)
            if (nd[i].node != mn.node && nd[i].tab == mn.tab && (nd[i].start < mn.start - 1 || nd[i].start > mn.start + 1) &&
                !(early > 0 && cd_is_comp(&nd[i], mn.start)))
                other = 1;
        if (other) {
            if (pre) CD_DROP_PRE();
            cd_failed(CD_ABORT);
            if (cd_cur_turn) glog("codrive: page turn %ld: another update reads the GLR table (node seen at %d): marked", cd_cur_id, mn.prog);
            continue;
        }
        int frames = 0, r, late = 1;
        uint32_t nfr = 0;
        if (pre && mn.tab == ptab && cd_nsv > 0 && cd_rd(mn.tab, &nfr, 4) && (int)nfr == mn.len + 1) {
            frames = pframes; r = 1; late = 0; // the cells were there before the turn's first frame
        } else {
            if (pre) CD_DROP_PRE();
            r = cd_arm(mn.tab, mn.len, mn.prog + 2, fe, fd, &frames);
            if (r >= 0) __atomic_store_n(&cd_tab4, mn.tab, __ATOMIC_RELAXED); // the next turn is armed ahead
        }
        if (r < 0) {
            cd_failed(CD_BAD);
            if (cd_cur_turn) glog("codrive: page turn %ld: the GLR table could not be armed (%d): marked", cd_cur_id, r);
            continue;
        }
        if (r == 0) { // 1.8.13: needs none
            __atomic_fetch_add(&n_cd[CD_SMALL], 1, __ATOMIC_RELAXED);
            cd_good();
            if (cd_cur_turn) glog("codrive: page turn %ld (GLR, fe %.3f fd %.3f): too small to drive, needs none", cd_cur_id, fe, fd);
            continue;
        }
        __atomic_fetch_add(&n_cd[late ? CD_ARM : CD_PRE], 1, __ATOMIC_RELAXED);
        if (late) cd_mark(); // the rows the generator had done keep their trace
        int whole = !late; // 1.8.13: the turn got all of its co-drive
        Rect bb = {mn.x0, mn.y0, mn.x1 - mn.x0 + 1, mn.y1 - mn.y0 + 1};
        int comp = early;
        if (early <= 0 && __atomic_load_n(&cfg.cd_comp, __ATOMIC_RELAXED)) {
            int x0 = mn.x0, y0 = mn.y0, x1 = mn.x1, y1 = mn.y1;
            n = cd_walk(g, nd, CD_MAXN); // afresh: the turn's other updates (same start frame), e.g. the page's bottom rows
            for (int i = 0; i < n; i++) {
                if (!cd_node_ok(&nd[i]) || nd[i].start < mn.start - 1 || nd[i].start > mn.start + 1) continue;
                if (nd[i].x0 < x0) x0 = nd[i].x0;
                if (nd[i].y0 < y0) y0 = nd[i].y0;
                if (nd[i].x1 > x1) x1 = nd[i].x1;
                if (nd[i].y1 > y1) y1 = nd[i].y1;
            }
            bb = (Rect){x0, y0, x1 - x0 + 1, y1 - y0 + 1};
            comp = cd_companion(bb, mn.wf);
            if (comp < 0) { cd_failed(CD_COMPFAIL); whole = 0; }
            else if (comp) __atomic_fetch_add(&n_cd[CD_COMP], 1, __ATOMIC_RELAXED);
        }
        // hold until the turn's updates and the companions have left the list; put the cells back at once when
        // another update starts to read the table
        double t1 = now_s(), lim = mn.len * 0.0125 + 0.5;
        int aborted = 0, main_here = 1, unknown = 0, timed = 0;
        for (;;) {
            usleep(2000);
            n = cd_walk(g, nd, CD_MAXN);
            int here = 0;
            if (n < 0) {
                if (++unknown >= 3) { aborted = 1; break; } // the list cannot be read: nobody can tell who reads the table
                continue;
            }
            unknown = 0;
            main_here = 0;
            for (int i = 0; i < n; i++) {
                if (nd[i].tab != mn.tab) continue;
                if (nd[i].node == mn.node && nd[i].start == mn.start) { here = main_here = 1; continue; }
                if (comp > 0 && coff == 99 && cd_comp_rect(&nd[i])) coff = nd[i].start - mn.start; // for the log
                if (nd[i].start >= mn.start - 1 && nd[i].start <= mn.start + 1) { here = 1; continue; }
                if (comp > 0 && cd_is_comp(&nd[i], mn.start)) { here = 1; continue; }
                aborted = 1;
            }
            if (aborted || !here) break;
            if (now_s() - t1 > lim) { timed = 1; break; }
            if (!__atomic_load_n(&cfg.codrive, __ATOMIC_RELAXED) || __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) { aborted = 1; break; }
        }
        int cells = cd_nsv;
        cd_restore_chk();
        if (aborted || timed) {
            if (main_here) { cd_failed(CD_ABORT); whole = 0; } else __atomic_fetch_add(&n_cd[CD_ABORT], 1, __ATOMIC_RELAXED);
        }
        if (whole) cd_good();
        __atomic_store_n(&cd_last, frames, __ATOMIC_RELAXED);
        glog("codrive: fe %.3f fd %.3f node %d,%d-%d,%d len %d %s %d: %d x 6 V frames, %d cells, companion %d%s (e%d h%d %.0f ms, %+d), held %.0f ms%s",
             fe, fd, mn.x0, mn.y0, mn.x1, mn.y1, mn.len, late ? "late, seen at" : "ahead, seen at", mn.prog, frames, cells, comp,
             early > 0 ? " early" : "", ewhy, hwhy, ewait * 1000.0, coff, (now_s() - t1) * 1000.0,
             timed ? ", time limit" : aborted ? (main_here ? ", cut short by another update" : ", ended by the next update") : "");
    }
    return NULL;
}

static void cd_request(double fe, double fd, Rect bb, int turn, int full) {
    pthread_mutex_lock(&cd_mu);
    if (!cd_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (pthread_create(&th, &at, cd_main, NULL) == 0) cd_started = 1;
        pthread_attr_destroy(&at);
        if (!cd_started) { // no helper: the page clean does the job
            __atomic_store_n(&cfg.codrive, 0, __ATOMIC_RELAXED);
            pthread_mutex_unlock(&cd_mu);
            glog("codrive: the helper thread could not be started: the co-drive is off now");
            return;
        }
    }
    // 1.8.10: a turn the helper never took went without its co-drive
    // 1.8.19: but for a later frame that changes next to nothing, whatever replaces it (the same turn's next frame,
    // the next turn while leafing, a menu): nothing was written for it and nothing was needed
    if (cd_reqn != cd_taken && cd_turn && !(cd_turn == 2 && cd_small(cd_fe, cd_fd))) cd_mark_set(cd_req_id);
    // 1.8.18: a turn away from a page with colour in its states is drawn as an STD: no GLR cells for it, but the old
    // page's levels for the helper and the strip's companion as an STD
    int col = turn ? cd_state_colour(bb) : 0;
    int cstd = __atomic_load_n(&cfg.cd_std, __ATOMIC_RELAXED);
    int pstd = turn && col > 0 && cstd == 1;
    // 1.8.22 (cd_std 2): that turn is left alone (it gets the page clean): no GLR cells ahead of an STD, no
    // companions, no levels
    int cskip = turn && col > 0 && cstd == 2;
    if (cd_pre) cd_pre = 2; // an older request's cells the helper has not seen yet: it puts them back
    // 1.8.19: no GLR cells ahead of a turn's later frame (it mostly draws nothing large: its companions would run
    // the cells alone)
    else if (!pstd && !cskip && turn != 2 && !cd_busy && cd_nsv == 0 && cd_prearm(fe, fd)) cd_pre = 1;
    cd_fe = fe; cd_fd = fd; cd_bb = bb;
    cd_turn = turn;
    cd_req_id = 0; cd_req_stdn = 0; cd_req_col = 0; cd_req_textn = 0;
    if (turn == 2) { // 1.8.19: a later frame of the turn that has the newest id: its verdict goes to that turn
        cd_req_col = col;
        cd_req_id = __atomic_load_n(&cd_turn_id, __ATOMIC_ACQUIRE);
    } else if (turn) { // 1.8.13: the turn gets its id; what the helper needs to judge an STD is taken before the update starts
        cd_req_stdn = __atomic_load_n(&cd_std_next, __ATOMIC_ACQUIRE);
        cd_req_textn = __atomic_load_n(&cd_text_next, __ATOMIC_ACQUIRE);
        cd_req_col = col;
        __atomic_store_n(&cd_turn_ms, (long)(now_s() * 1000.0), __ATOMIC_RELAXED);
        cd_req_id = __atomic_add_fetch(&cd_turn_id, 1, __ATOMIC_RELEASE);
    }
    if (cskip && cd_req_id) cd_col_set(cd_req_id); // 1.8.22: whatever becomes of the request, turn_px_col decides its clean
    cd_early_ok = turn && !__atomic_load_n(&cd_std_next, __ATOMIC_ACQUIRE);
    cd_here_n = 0;
    cd_here_wf = 4;
    cd_here_why = cd_pre != 1 ? 8 : !cd_early_ok ? 9 : 7;
    cds_req_ok = 0;
    int comps = __atomic_load_n(&cfg.cd_comp, __ATOMIC_RELAXED) && __atomic_load_n(&cfg.cd_early, __ATOMIC_RELAXED) >= 2;
    // 1.8.19: the levels for every turn's frame (a turn to a page with colour, or away from blacks an STD left at
    // level 2, is an STD nobody could tell from the old page's colours)
    if (turn && cstd && !cskip) cds_req_ok = cds_read(cds_req);
    if (pstd) {
        cd_here_why = cds_req_ok ? 7 : 10;
        // 1.8.25: the flip rows to 0 V for this turn (or kept there), before its update and its companions are
        // queued; never for a complete refresh (cdf_line has put the stock rows back for it)
        if (cds_req_ok && !full) cdf_hold();
        // 1.8.19: not for a later frame: it mostly draws nothing large, and its companions (the complement of its
        // box) would start at once on the table the first frame's co-drive is running on
        if (cds_req_ok && comps && turn != 2) {
            int r = cd_companion_here(bb, cd_here_rs, 2); // with the turn's own update, in its batch
            cd_here_why = r < 0 ? -r : 0;
            cd_here_n = r > 0 ? r : 0;
            cd_here_wf = 2;
        }
    } else if (cd_pre == 1 && cd_early_ok && comps) {
        int r = cd_companion_here(bb, cd_here_rs, 4); // 1.8.12: with the turn's own update, in its batch
        cd_here_why = r < 0 ? -r : 0;
        cd_here_n = r > 0 ? r : 0;
    }
    if (turn == 1) { // 1.8.11: one turn takes it, found by the helper or not
        __atomic_store_n(&cd_std_next, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&cd_text_next, 0, __ATOMIC_RELEASE);
    }
    cd_reqn++;
    pthread_cond_signal(&cd_cv);
    pthread_mutex_unlock(&cd_mu);
}
#else
static unsigned long host_cd_n;             // co-drive requests the host "sent", and the last one's parts
static double host_cd_fe, host_cd_fd;
static int host_cd_verdict;                 // 1.8.13: the host helper's answer to a page turn: 0 none, 1 co-driven, 2 not
static int host_cd_turn = -1;               // 1.8.19: the last request's kind (0 no turn, 1 a turn, 2 a turn's later frame)
static int host_colour;                     // 1.8.22: what the host's state buffer "holds" (colour samples; -1 unreadable)
static int cd_state_colour(Rect r) { (void)r; return host_colour; }
static int host_chroma = 100;               // 1.8.26: the part of host_colour that is not balanced, in %
static long host_px;                        // and how many samples the host's scan "read" (0: the smallest part always passes)
static int cd_state_chroma(long *px, long *col, long *chroma) {
    if (host_colour < 0) return 0;
    *px = host_px; *col = host_colour; *chroma = (long)host_colour * host_chroma / 100;
    return 1;
}
static void cdf_line(int full) { (void)full; }
static unsigned long host_wts_kick;         // 1.8.30: promoted turns that asked for the white tail's table check
static void wts_kick(void) { host_wts_kick++; }
static void cdf_full(const char *why) { (void)why; }
static void cd_request(double fe, double fd, Rect bb, int turn, int full) {
    (void)bb; (void)full; host_cd_n++; host_cd_fe = fe; host_cd_fd = fd; host_cd_turn = turn;
    if (turn != 1) return; // 1.8.19: a later frame of the turn keeps the turn's id
    long id = ++cd_turn_id;
    cd_turn_ms = (long)(now_s() * 1000.0);
    if (host_cd_verdict == 1) cd_ok_id = id;
    if (host_colour > 0 && cfg.cd_std == 2) cd_col_id = id; // 1.8.22: as the device marks a turn away from colour
    if (host_cd_verdict == 2) { cd_fail_id = id; cd_fail_ms = cd_turn_ms ? cd_turn_ms : 1; }
}
static int xo_check(void **out) { (void)out; snprintf(xo_why, sizeof xo_why, "not linux"); xo_state = -1; return -1; }
static const char *host_rc = "unsupported"; // host tests choose what the clean returns (checks before the SWTCON gate)
static int host_sw = 1;                     // the SWTCON state the gate sees: 1 idle, 0 driving, -1 cannot tell
static double host_sw_until = -1;           // replay: the panel is driven until this time
static double host_temp = 21.5;
static int swtcon_idle(void) { return host_sw < 0 ? -1 : host_sw == 0 || (host_sw_until > 0 && now_s() < host_sw_until) ? 0 : 1; }
static double xo_temp(void) { return host_temp; }
static Rect host_last;                      // the last rectangle a clean was sent for
static int host_text_ok;                    // 1.8.15: the host's clean_text_ok (tests set it)
static int host_queued;                     // 1.8.15: an update waits to be accepted (see the target's swtcon_queued)
static int swtcon_queued(void) { return host_queued; }
static const char *do_rclean(Rect *r) {
    if (strcmp(host_rc, "ok")) return host_rc;
    int sw = swtcon_idle();
    __atomic_store_n(&last_sw, sw, __ATOMIC_RELAXED);
    if (sw != 1 && !rect_full(*r)) return sw == 0 ? "inflight" : "noidle";
    int reveal = 0; // 1.8.29, as on the tablet
    const char *hold = wh_clean_gate(r, sw, &reveal);
    if (hold) return hold;
    __atomic_store_n(&last_clean_text, !reveal && cfg.clean_text && (sw == 1 || swtcon_queued() == 0) && host_text_ok && xo_temp() < 28.0, __ATOMIC_RELEASE);
    host_last = *r;
    if (reveal) wh_clean_sent();
    return "ok";
}
static int host_test_n;                      // test updates the host "sent"
static int host_test_last[4];               // what, wf, pm, flags of the last one
static const char *do_test_update(int what, int wf, int pm, int flags, Rect *r) {
    if (strcmp(host_rc, "ok")) return host_rc;
    if (!what && (!r || !clip_rect(r) || r->w < 8 || r->h < 8)) return "empty";
    if (swtcon_idle() != 1) return "inflight";
    host_test_n++;
    host_test_last[0] = what; host_test_last[1] = wf; host_test_last[2] = pm; host_test_last[3] = flags;
    if (r) host_last = *r;
    return "ok";
}
static int fb_refresh(void) { return !gg_off && fb != NULL; } // host tests point fb at a synthetic frame
static unsigned long n_restore, n_force;
static int restore_pending(void) { n_restore++; return 1; }
static void force_pending(void) { n_force++; }
static unsigned long n_withdraw_host;
static int host_wd_ok; // host tests: taking the byte back clears the way ("pending" -> "ok")
static int withdraw_pending(void) {
    n_withdraw_host++;
    if (host_wd_ok && !strcmp(host_rc, "pending")) host_rc = "ok";
    return 1;
}
#endif

// Give a withdrawn stock flash back for the next frame (the sleep screen): under the framebuffer lock when it can
// be had within 20 ms, else without it. Called WITHOUT mu.
static void restore_pending_now(void) {
    for (int i = 0; i < 40; i++) {
        int r = restore_pending();
        if (r != 0) return; // done, or not verified (then nothing was ever withdrawn)
        usleep(500);
    }
    force_pending();
}

// ---------------------------------------------------------------- our in-place clean (all paths go through here)
// clean_wf 2 = the stock STD path (swapBuffers_impl, flags 0x1: Mono part wf3, the rest wf2), proven on the panel.
static int clean_wf_ok(int wf) { return wf == 2; }
// 1.8.7: what the test command "send" may put into the queue. Waveforms 1..9 (never INIT), the pixel modes and
// flag bits the stock callers use; PMODE and TMODE (8, 9) only with flags 0 (with bit 0 or 1 nothing useful is driven).
static int send_args_ok(int wf, int pm, int flags) {
    if (wf < 1 || wf > 9) return 0;
    if (pm != 3 && pm != 7 && pm != 0xe && pm != 0x11 && pm != 0x13) return 0;
    if (flags < 0 || flags > 15) return 0;
    if (wf >= 8 && flags != 0) return 0;
    return 1;
}
static const char *do_clean(Rect *r) {
    // 1.8.4 (review of 1.8.3, CF-3): reset before the sleep check too, so an "asleep" line never shows the panel
    // state of an earlier clean
    __atomic_store_n(&last_sw, -2, __ATOMIC_RELAXED); // set by do_rclean once it reads the SWTCON state
    // never draw over the sleep screen, whoever asks (an explicit rclean included)
    if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) return "asleep";
    int wf = cfg.clean_wf;
    __atomic_store_n(&last_clean_text, 0, __ATOMIC_RELEASE); // 1.8.15: set by do_rclean when it takes the TEXT targets
    const char *rc = do_rclean(r);
    if (!strcmp(rc, "ok") && __atomic_load_n(&last_clean_text, __ATOMIC_ACQUIRE)) wf = 3;
    if (!strcmp(rc, "ok") && wf >= 0 && wf < 16) n_clean_wf[wf]++;
    if (!strcmp(rc, "inflight")) __atomic_add_fetch(&n_inflight, 1, __ATOMIC_RELAXED);
    else if (!strcmp(rc, "ok") && __atomic_load_n(&last_sw, __ATOMIC_RELAXED) == 0)
        __atomic_add_fetch(&n_full_busy, 1, __ATOMIC_RELAXED); // a full clean over an in-flight update (allowed, logged)
    return rc;
}

// 1.8.3: a clean that should be tried again at a later tick (same deadline): the framebuffer or the panel is busy,
// the frame is not read yet, or another stock flash goes out first
static int rc_retry(const char *rc) {
    return !strcmp(rc, "busy") || !strcmp(rc, "notready") || !strcmp(rc, "pending") || !strcmp(rc, "inflight");
}

// the SWTCON state and the panel temperature at the last clean, for the log lines
static const char *sw_note(char *b, size_t n) {
    int sw = __atomic_load_n(&last_sw, __ATOMIC_RELAXED);
    const char *s = sw == 1 ? "idle" : sw == 0 ? "busy" : sw == -1 ? "unknown" : "not read";
    double tc = xo_temp();
    const char *tx = __atomic_load_n(&last_clean_text, __ATOMIC_ACQUIRE) ? ", text"
                     : __atomic_load_n(&last_text_block, __ATOMIC_RELAXED) == 2 ? ", std black" : "";
    if (isnan(tc)) snprintf(b, n, "panel %s, temp ?%s", s, tx);
    else snprintf(b, n, "panel %s, %.1f C%s", s, tc, tx);
    return b;
}

// ---------------------------------------------------------------- one flash per view change, in place
// xochitl answers a view change (Settings opened/closed, document dialogs, ...) with BlinkLater, so the
// FIRST frame of the new view is a full-screen STD. That frame changes the content, and an STD that changes
// the content leaves the old image behind on this panel (the old screen stays visible; an STD over content
// that is already shown erases it). With "defer" on, the stock flash is withdrawn as soon as xochitl sets
// it, the new view is drawn by its normal update, and once nothing has changed for tr_quiet seconds
// (QML sends "GGQ <id> settle") the changed area gets ONE STD in place. Without a QML clock heartbeat in
// the last 90 s nothing is withdrawn, so a stopped QML side can never leave a view change without a flash.
// GhostBuster::viewChange logs its line BEFORE it sets the byte (log @0x6dc708, set @0x6dc684, reached
// after the log), so a helper thread waits for the byte (at most 150 ms; the stock frame follows 16-247 ms
// after the line, measured) and clears it with a compare-and-swap. If the frame wins the race, the stock
// flash goes out as before and the in-place clean still follows.
// Rules (1.5.1, from the review):
//  - only while rm.framebuffer.updates lines come in: without them the redrawn area is unknown;
//  - a view change during a waiting cycle joins it, and the clean also waits for that view's first frame
//    (up to TR_FRAME_MAX), so the newest view is never left with neither the flash nor the clean;
//  - when ink is drawn on the new view, the clean needs TR_INK_QUIET without updates and the pen away from
//    the screen (QML adds "pen" while PenClosenessMonitor.penWasClose): never in the middle of writing;
//  - a later stock full refresh that changes almost nothing in the redrawn area is itself the in-place
//    clean, so no second flash follows (tr_on_complete);
//  - another stock flash pending at clean time goes out first with its own frame (do_rclean "pending");
//  - a view change that draws nothing within TR_FRAME_MAX changed nothing on screen and needs no clean;
//    a cycle older than TR_MAX_AGE is dropped.
// Added in 1.8 (review round 2, liveness-3):
//  - minor ui updates (sync icons: minor_ui_update) neither delay the clean nor count as the new view's first
//    frame: during a sync they came every 0.3-0.6 s and pushed the clean of a document close onto the menu the user
//    opened next;
//  - never while the on-screen keyboard is up (typing); a cycle that waited for it keeps TR_INK_MAX_AGE, so it
//    cleans once the keyboard has closed (its own keyboard request is then covered by this full clean).
static int defer_live(void) { return cfg.defer && hb_t > 0 && now_s() - hb_t < 90.0 && xo_state >= 0; }

#ifdef __linux__
static pthread_mutex_t wd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wd_cv = PTHREAD_COND_INITIALIZER;
static unsigned wd_req;
static int wd_started;

static void *wd_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&wd_mu);
        while (wd_req == seen) pthread_cond_wait(&wd_cv, &wd_mu);
        seen = wd_req;
        pthread_mutex_unlock(&wd_mu);
        int got = 0;
        double t0 = now_s();
        do {
            if (!__atomic_load_n(&cfg.defer, __ATOMIC_RELAXED)) break; // turned off meanwhile: leave it to xochitl
            if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) break;   // the sleep frame owns the flash
            int w = withdraw_pending();
            if (w > 0 && __atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) { // lost the race with the sleep guard
                force_pending();
                break;
            }
            if (w != 0) { got = w > 0; break; }
            usleep(250);
        } while (now_s() - t0 < 0.15);
        pthread_mutex_lock(&mu);
        if (got) { tr_withdrawn = 1; n_wd++; } else { n_wd_miss++; }
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void wd_kick(void) {
    pthread_mutex_lock(&wd_mu);
    if (!wd_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 64 * 1024);
        if (pthread_create(&th, &at, wd_main, NULL) == 0) wd_started = 1;
        pthread_attr_destroy(&at);
    }
    wd_req++;
    pthread_cond_signal(&wd_cv);
    pthread_mutex_unlock(&wd_mu);
}
#else
static unsigned long n_kick; // host tests: count the requests instead of starting the helper
static void wd_kick(void) { n_kick++; }
#endif

// 1.8.26: does the page on the screen hold real colour (see cd_state_chroma)? Only such a page gets the full STD
// turn: on a scanned black-and-white book the user will not have a flash at every turn (1.8.23). tf_chroma 0: any
// colour sample will do (1.8.22). The numbers go to the log either way.
static int tf_chroma_ok(long *px, long *col, long *ch) {
    *px = *col = *ch = 0;
    if (cfg.tf_chroma <= 0) return 1;
    if (!cd_state_chroma(px, col, ch)) return 0;
    return *col > 0 && *ch * 100 >= (long)cfg.tf_chroma * *col && *ch * 10000 >= (long)cfg.tf_chroma_min * *px;
}

// 1.8.22 (turn_full): a turn whose frame did not come (the same page again) must not leave its byte to flash a
// later frame that has nothing to do with it (a menu, a dialog). Called at every tick of the QML clock (GUI thread)
// and, as a quiet screen has no tick for up to 20 s, by a timer thread started at the promotion.
static void turn_full_age(const char *by) {
    pthread_mutex_lock(&mu);
    int tf_old = promote_live && turn_full_np && now_s() - turn_full_t > TURN_FULL_MAX_AGE;
    if (tf_old) {
        promote_live = 0;
        turn_full_np = 0;
        n_unpromote++;
    }
    pthread_mutex_unlock(&mu);
    if (!tf_old) return;
    int w = withdraw_pending();
    glog("page turn: its frame did not come within %.1f s, the promoted flash is taken back (turn_full, %s), "
         "withdrawn=%d", TURN_FULL_MAX_AGE, by, w);
}

#ifdef __linux__
static pthread_mutex_t tfa_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tfa_cv = PTHREAD_COND_INITIALIZER;
static unsigned tfa_req;
static int tfa_started;

static void *tfa_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&tfa_mu);
        while (tfa_req == seen) pthread_cond_wait(&tfa_cv, &tfa_mu);
        seen = tfa_req;
        pthread_mutex_unlock(&tfa_mu);
        // second review: sleep to the live promotion's own deadline and look again (a newer promotion, a signal that
        // cut the sleep short); back to the wait only when no turn's byte is left
        for (;;) {
            pthread_mutex_lock(&mu);
            int live = promote_live && turn_full_np;
            double left = turn_full_t + TURN_FULL_MAX_AGE + 0.05 - now_s();
            pthread_mutex_unlock(&mu);
            if (!live) break;
            if (left > 0) {
                if (left > 2.0) left = 2.0;
                struct timespec ts = {(time_t)left, (long)((left - (double)(time_t)left) * 1e9)};
                nanosleep(&ts, NULL);
                continue;
            }
            turn_full_age("the timer");
        }
    }
    return NULL;
}

static void tfa_kick(void) {
    pthread_mutex_lock(&tfa_mu);
    if (!tfa_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&th, &at, tfa_main, NULL) == 0) tfa_started = 1;
        pthread_attr_destroy(&at);
    }
    tfa_req++;
    pthread_cond_signal(&tfa_cv);
    pthread_mutex_unlock(&tfa_mu);
}
#else
static unsigned long n_tfa_kick; // host tests: count the requests instead of starting the timer
static void tfa_kick(void) { n_tfa_kick++; }
#endif

// ---- 1.8.29: the white intermediate, the panel's side (the frame's side stands in front of luma_at) --------------
#define XO_SW_RGB 0x4120 // in the SWTCON's object: the pointer to the RGB frame its conversion reads
#ifdef __linux__
// frame lock held (inside swapBuffers): the stored screen-mode map has a Sleep part, the frame is the sleep screen
static int wh_map_sleep(void) {
    if (xo_state != 1) return 0;
    void *fbo = xo_fbo();
    return fbo && !q_region_is_empty((const char *)fbo + FBO_MAP + MAP_SLEEP);
}

// the SWTCON converts the very image the page is hidden in (not a copy, which would still hold the page)
static int wh_frame_ok(void) {
    uintptr_t g = cd_g(), p = 0;
    return g && fb && cd_rd(g + XO_SW_RGB, &p, sizeof p) && p == (uintptr_t)fb;
}

// The panel's states once the white has gone out, every 16th pixel of every 16th row: 1 white, 0 not (more than
// 0.3 % of the samples below the dithered white, level 28), -1 not readable.
static int wh_state_white(long *n, long *bad) {
    uintptr_t st = 0;
    uint32_t row[1632];
    *n = *bad = 0;
    if (xo_state != 1 || !cd_rd(xo_base + XO_STATEP, &st, sizeof st) || !st) return -1;
    for (int y = 8; y < SH; y += 16) {
        if (!cd_rd(st + (uintptr_t)y * sizeof row, row, sizeof row)) return -1;
        for (int x = 8; x < SW; x += 16) {
            ++*n;
            if ((row[x] & 31) < 28) ++*bad;
        }
    }
    return *bad * 1000 <= *n * 3;
}

// The watchdog: WH_WD after the turn the page is still owed (no tick of the clock brought it). The next frame
// xochitl draws is then made a complete refresh, and the swapBuffers hook gives it the page. Nothing is drawn or
// sent from this thread. One thread for good, started at the first white turn.
static pthread_mutex_t whw_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t whw_cv = PTHREAD_COND_INITIALIZER;
static unsigned whw_req;
static int whw_started;

static void *whw_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&whw_mu);
        while (whw_req == seen) pthread_cond_wait(&whw_cv, &whw_mu);
        seen = whw_req;
        pthread_mutex_unlock(&whw_mu);
        for (;;) {
            pthread_mutex_lock(&mu);
            double left = wh_t0 + WH_WD - now_s();
            int owed = __atomic_load_n(&wh_owed, __ATOMIC_ACQUIRE);
            int fire = owed && left <= 0 && !__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE);
            if (fire) n_wh_wd++;
            pthread_mutex_unlock(&mu);
            if (!owed) break;
            if (left > 0) {
                if (left > 2.0) left = 2.0;
                struct timespec ts = {(time_t)left, (long)((left - (double)(time_t)left) * 1e9)};
                nanosleep(&ts, NULL);
                continue;
            }
            if (fire) {
                force_pending();
                glog("white intermediate: the page did not follow within %.1f s: the next frame goes out as a complete refresh", WH_WD);
            }
            break;
        }
    }
    return NULL;
}

static void wh_wd_kick(void) {
    pthread_mutex_lock(&whw_mu);
    if (!whw_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&th, &at, whw_main, NULL) == 0) whw_started = 1;
        pthread_attr_destroy(&at);
    }
    whw_req++;
    pthread_cond_signal(&whw_cv);
    pthread_mutex_unlock(&whw_mu);
}
#else
static int host_wh_sleepmap;  // host tests: the map has a Sleep part
static int wh_map_sleep(void) { return host_wh_sleepmap; }
static int host_wh_frame = 1; // the SWTCON reads the frame image itself
static int wh_frame_ok(void) { return host_wh_frame; }
static int host_wh_state = 1; // what the states say after the white: 1 white, 0 not, -1 not readable
static int wh_state_white(long *n, long *bad) {
    *n = host_wh_state < 0 ? 0 : 13635;
    *bad = host_wh_state == 0 ? 500 : 0;
    return host_wh_state;
}
static unsigned long n_whw_kick; // count the requests instead of starting the watchdog
static void wh_wd_kick(void) { n_whw_kick++; }
#endif

// The turn's frame is about to go out as the full STD (on_update_line: under mu, inside swapBuffers): the page is
// hidden behind white. 0: not this time (the clock is not alive, a check failed, no memory): the caller arms the
// second pass of 1.8.26 instead.
static int wh_hide(double t0) {
    if (wh_off || !fb || fb_bpl < SW * 4 || !defer_live() || !wh_frame_ok()) { n_wh_no++; return 0; }
    if (!wh_shadow) {
        wh_shadow = malloc((size_t)fb_bpl * SH);
        if (!wh_shadow) { wh_off = 1; n_wh_no++; return 0; }
        memset(wh_shadow, 0xff, (size_t)fb_bpl * SH);
    }
    int first = !__atomic_load_n(&wh_active, __ATOMIC_ACQUIRE);
    long strays = wh_cover(first);
    wh_t0 = t0;
    wh_pend = 0;
    wh_checked = 0;
    __atomic_store_n(&wh_owed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&wh_active, 1, __ATOMIC_RELEASE);
    n_wh_hide++;
    if (first && strays) { // xochitl wrote the marker's value itself: the marker cannot be told from a pixel
        wh_off = 1;
        n_wh_bad++;
        glog("white intermediate: %ld pixels of the frame already held the marker's value: after this turn none until the next start (the double pass instead)", strays);
    }
    wh_wd_kick();
    return 1;
}

// start (or join) a cycle: the stock flash of the view change is withdrawn by the helper thread
static void tr_begin(const char *what) {
    snprintf(tr_what, sizeof tr_what, "%s", what);
    n_tr++;
    double t = now_s();
    if (!tr_active) { // a second change before the clean joins the first one
        tr_active = 1;
        tr_t0 = t;
        tr_box = (Rect){0, 0, 0, 0};
        tr_withdrawn = 0;
        tr_late = 0;
        tr_inked = 0;
        tr_sd = 0;
    }
    tr_vc_last = t; // the clean waits for this view's first frame too
    tr_inked = 0;   // ink drawn before belonged to the previous view
    tr_kb_held = 0; // the same for the keyboard hold (set again at the next tick if it is still up)
    tr_wait_t = 0;  // 1.8.3: the wait for an idle panel starts again with the new view's own update
}

// "viewChange xofm::libs::ghostbuster::IGhostBuster::ViewChangeMode::<Mode>" (called with mu held)
static int on_view_change(const char *m) {
    if (!defer_live() || !fb || n_updates == 0 || sleeping) return 0;
    const char *mode = strstr(m, "ViewChangeMode::");
    tr_begin(mode ? mode + 16 : "?");
    if (promote_live && turn_full_np) { // second review: a turn's byte (turn_full) no frame took yet is taken back
        promote_live = 0;               // first, so the helper's one take is the view change's own byte (as the
        turn_full_np = 0;               // document close does)
        tr_promoted = 0;
        n_unpromote++;
        unpromote_why = "a view change";
        return 4;
    }
    return 1;
}

// 1.8: ScreenDriver lines (category xofm.modules.devicesceneview.screendriver), with mu held. 1 = a document close
// started a cycle: kick the helper that withdraws its stock flash. 2 = promote the skipped gesture clean: set the
// BlinkLater byte now (the caller does it without mu); 5 = 2 for a page turn (turn_full: the caller also starts
// the timer that takes an unused byte back). 3 = take our promoted byte back. 4 = 3, then 1 (the close
// sets its own byte only after " - reset", @0x2b1ebc, so the helper finds that one). A promote that a waiting view
// change cycle would end in ends it only at the frame that takes the byte (tr_promoted); a take-back keeps it.
static int on_sd_line(const char *m) {
    if (!strncmp(m, "visibleChanged: ", 16)) {
        sd_visible = !strncmp(m + 16, "true", 4);
        if (!sd_visible) sd_gesture = sd_mono = sd_tiles = sd_tiles_open = sd_mono_gesture = 0;
        else if (!sleeping) {
            // an open: its tiles are pending although no "pendingTilesChanged: true" is logged (every open measured;
            // "pendingTilesChanged: false" followed 0.45-0.65 s later). A wake logs "visibleChanged: true" while the
            // display is still asleep, with no tiles lines at all: not an open.
            sd_tiles = 1;
            sd_tiles_open = 1;
            sd_open_t = now_s();
        }
        return 0;
    }
    if (!strncmp(m, "penCloseChanged: ", 17)) {
        sd_pen = !strncmp(m + 17, "true", 4);
        if (sd_pen && promote_live && !sleeping) { // the user is about to write: no flash under the first stroke
            promote_live = 0;
            tr_promoted = 0; // a cycle it would have ended goes on waiting for its own clean
            n_unpromote++;
            unpromote_why = "the pen came near";
            return 3;
        }
        return 0;
    }
    if (!strncmp(m, "gestureModeChanged: ", 20)) {
        sd_gesture = !strncmp(m + 20, "true", 4);
        if (sd_gesture) sd_mono_gesture = 1;
        if (sd_gesture && auto_req && auto_kind != AK_KB) {
            page_given_up();
            auto_req = 0; // a new gesture: the screen changes again, the ScreenDriver decides after it
            n_auto_covered++;
        }
        if (sd_gesture && promote_live && !sleeping) { // a new gesture before the redraw: it decides again at its end
            promote_live = 0;
            tr_promoted = 0;
            n_unpromote++;
            unpromote_why = "a new gesture started";
            return 3;
        }
        return 0;
    }
    if (!strncmp(m, "pendingTilesChanged: ", 21)) {
        sd_tiles = !strncmp(m + 21, "true", 4);
        sd_tiles_open = 0;
        return 0;
    }
    if (strstr(m, "updating mono state, full -> mono")) { sd_mono = 1; if (sd_gesture) sd_mono_gesture = 1; return 0; }
    if (strstr(m, "updating mono state, mono -> full")) { sd_mono = 0; sd_full_t = now_s(); return 0; }
    if (!strncmp(m, "finalizeNewPage", 15)) {
        sd_newpage_prev_t = sd_newpage_t; // 1.8.26
        sd_newpage_t = now_s();
        sd_newpage_n++;
        // 1.8.29: while a page is held back behind the white (or still owed) no further turn is promoted: its frame
        // joins the page behind the white, and the one complete refresh that follows shows the newest page
        if (__atomic_load_n(&wh_owed, __ATOMIC_ACQUIRE)) return 0;
        // 1.8.22 (turn_full; see the header): the page on the screen holds colour, so the turn is an STD and would
        // get the clean after it: let its frame (~50 ms from here) go out as the full STD instead. Never with the
        // pen near, in a gesture or mono state, with the keyboard up, asleep, or while a view change or a request of
        // ours waits for its own clean (those go on as they are).
        // 1.8.26: but the second pass of the turn before this one may wait (the user leafs through the pages): this
        // turn is promoted all the same, and its frame takes that pass over (arm_second_pass).
        if (!cfg.turn_full || cfg.codrive != 1 || !cfg.page_clean || !sd_visible || sleeping || sd_pen || sd_gesture ||
            sd_mono || kb_on || promote_live || tr_active || (auto_req && !(auto_kind == AK_PAGE && auto_page_idle)))
            return 0;
        // review: not at a document's open (this line comes 16-48 ms after "visibleChanged: true", the library is
        // still on the screen): its first page is drawn and then gets xochitl's own in-place STD (as page_track)
        double ot = sd_newpage_t - sd_open_t;
        if (sd_open_t > 0 && ot < 10.0 && (ot < 1.0 || t_stock_full < sd_open_t)) return 0;
        if (cd_state_colour((Rect){0, 0, SW, SH}) <= 0) return 0;
        long px, col, ch; // 1.8.26: real colour only (not the balanced primaries of a scanned page's greys)
        if (!tf_chroma_ok(&px, &col, &ch)) {
            glog("page turn: the page's colour is balanced or small (chroma %ld of %ld colour samples, %ld read): no full STD", ch, col, px);
            return 0;
        }
        promote_live = 1;
        turn_full_np = sd_newpage_n;
        turn_full_t = sd_newpage_t;
        n_turn_full++;
        glog("page turn: the page holds colour (chroma %ld of %ld colour samples, %ld read) -> the turn's frame goes out as a full STD (turn_full %d)",
             ch, col, px, cfg.turn_full);
        return 5;
    }
    if (!strncmp(m, " - clean the screen, dirty=", 27)) { // the ScreenDriver's measure, logged right before its decision
        char *end = NULL;
        double d = strtod(m + 27, &end);
        sd_dirty = (end != m + 27 && d >= 0.0 && d <= 1.0) ? d : -1.0;
        return 0;
    }
    if (!strncmp(m, " - doing nothing", 16)) { // the ScreenDriver skipped its clean after a gesture
        double d = sd_dirty;
        sd_dirty = -1.0;
        // only after a gesture (a lasso or text selection also runs a mono state: never promoted), never with the
        // pen near (the user is about to write), never while the display sleeps
        if (!cfg.gesture_clean || !sd_visible || sleeping || !sd_mono_gesture || sd_pen || d < cfg.gesture_min) {
            if (cfg.gesture_clean && d >= 0.0) n_promote_skip++;
            return 0;
        }
        // 1.8.5 (see the header): the redraw is one page change; promoted under the page rule (count, model, cap)
        int leafing = 0;
        int due = page_due(now_s(), 0, &leafing);
        if (!due) {
            n_promote_skip++;
            glog("gesture end: ScreenDriver dirty=%.3f skipped its clean; page change %d since the last full refresh "
                 "(the first clean may come at %d), left alone", d, n_pchg, cfg.page_gap);
            return 0;
        }
        if (due == 1) {
            finalize_pending();
            Score sc = compute_score_q(cfg.auto_quiet);
            if (!page_model_ok(&sc, cfg.page_px)) {
                n_promote_skip++;
                n_page_none++;
                glog("gesture end: ScreenDriver dirty=%.3f skipped its clean; page change %d, ghost=%ld (page_px %d) "
                     "hot=%d, left alone", d, n_pchg, sc.ghost, cfg.page_px, sc.hot);
                return 0;
            }
        }
        // its mono -> full redraw comes next (~50 ms, measured): let it go out as the full STD its own
        // " - full update.." branch would have asked for. A request of ours waiting meanwhile is covered by it,
        // and so is a view change waiting for its clean (one flash).
        if (auto_req && auto_kind != AK_KB) {
            auto_req = 0;
            n_auto_stock++;
        }
        if (tr_active) tr_promoted = 1; // ended by the frame that takes the byte; a take-back keeps it waiting
        promote_live = 1;
        turn_full_np = 0; // this byte is the gesture end's, not a page turn's
        n_promote++;
        n_arm[AK_GESTURE]++;
        glog("gesture end: ScreenDriver dirty=%.3f skipped its clean -> its redraw goes out as a full STD (stock path)", d);
        return 2;
    }
    if (!strncmp(m, " - full update..", 16)) { // the ScreenDriver cleans the screen itself (BlinkLater)
        sd_dirty = -1.0;
        if (auto_req && auto_kind != AK_KB) {
            auto_req = 0;
            n_auto_stock++;
        }
        if (tr_active && sd_visible) {
            tr_active = 0;
            n_tr_absorbed++;
            glog("view change %s: the ScreenDriver's own full STD stands in for the clean", tr_what);
        }
        return 0;
    }
    if (!strncmp(m, " - reset", 8)) { sd_mono_gesture = 0; return 0; }
    if (!strcmp(m, " - clearing on next update")) { // the document view was hidden (closed, or the sleep screen)
        if (!cfg.sd_close || !defer_live() || !fb || n_updates == 0 || sleeping ||
            (sleep_soon_t > 0 && now_s() - sleep_soon_t < 10.0)) {
            n_sd_skip++;
            return 0;
        }
        tr_begin("DocumentClosed");
        tr_sd = 1;
        n_sd_close++;
        if (promote_live) { // take our promoted byte back first, so the helper's one take is the close's own byte
            promote_live = 0;
            tr_promoted = 0;
            n_unpromote++;
            unpromote_why = "the document closes";
            return 4;
        }
        return 1;
    }
    return 0;
}

// 1.8: rm.batterymanager lines, with mu held. 1 = give a withdrawn stock flash back now (the sleep frame is next).
static int on_power_line(const char *m) {
    if (strstr(m, "Light sleep timer timed out")) { sleep_soon_t = now_s(); return 0; }
    if (!strncmp(m, "Waking up display", 17)) { n_wake++; t_wake = now_s(); return 0; }
    const char *p = strstr(m, "Changing display state from ");
    if (!p) return 0;
    const char *to = strstr(p, " to ");
    if (!to) return 0;
    if (!strncmp(to + 4, "Normal", 6)) {
        __atomic_store_n(&sleeping, 0, __ATOMIC_RELEASE);
        t_wake = now_s();
        return 0;
    }
    // DeepSleep (or any other state that is not Normal): the next frame is the sleep screen, which must keep the
    // stock full refresh. Nothing of ours may run until the display is Normal again.
    __atomic_store_n(&sleeping, 1, __ATOMIC_RELEASE);
    cdf_full("sleep"); // 1.8.25: the sleep screen's refresh reads the stock rows
    promote_live = 0;
    tr_promoted = 0;
    sd_pen = 0; // the pen state is not known again until its next line
    if (auto_req) {
        auto_req = 0;
        n_auto_aged++;
    }
    if (kbo_req) kbo_drop("the display goes to sleep", now_s());
    int back = 0;
    if (tr_active) {
        back = 1; // give it back even if the helper is still looking for it (it stops once sleeping is set)
        tr_active = 0;
        n_sleep_back++;
        glog("view change %s: the display goes to sleep %.0f ms later, stock flash given back to the sleep frame",
             tr_what, (now_s() - tr_t0) * 1000.0);
    }
    return back;
}

// Need-based clean (1.6; 1.8 triggers it from xochitl's own events, see the header). The request (arm_auto, or
// "GGQ <id> auto <why>" from QML) is tried at every settle/heartbeat tick. It is dropped when a stock full refresh
// or a full clean of ours went over the screen after it, when ink was drawn after it (the user writes), while the
// display sleeps, or after AUTO_MAX_AGE (AUTO_PEN_MAX_AGE with the pen near). It waits while the pen is near, while
// the on-screen keyboard is up, while a view change waits for its own clean, while the ScreenDriver still works
// (gesture, mono state, tiles), until stock_gap has passed since a stock full refresh, and for auto_quiet without
// updates. A QML request made within stock_gap after a stock full refresh is dropped (it would be a second flash).
// Then page/QML requests ask the residue model ("soon"/"now"), gesture and keyboard requests do not, and the
// whole screen gets ONE in-place STD (a box would leave its edges visible). Called WITHOUT mu, on the GUI thread.
// The document view still renders tiles. The state an open set without a tiles line lapses after 2 s, so a missing
// "pendingTilesChanged: false" can never hold the cleans back.
static int tiles_busy(double t) {
    if (sd_tiles && sd_tiles_open && t - sd_open_t > 2.0) sd_tiles = sd_tiles_open = 0;
    return sd_tiles;
}

static const char *auto_drop_reason(double t, int pen) {
    // a request the pen held back keeps AUTO_PEN_MAX_AGE also on the first tick after the pen left (review round 2:
    // otherwise any hover longer than AUTO_MAX_AGE lost the clean; median hover in the journals 6.6 s)
    int pen_age = pen || auto_pen_held || (sd_visible && sd_pen);
    // 1.8.26: the second pass of a full STD turn (turn_full 2). It waits for the panel to finish the turn's frame
    // (1.0 - 1.4 s) and, while the user leafs, for page_flip without updates after the last turn: it has the long
    // deadline. It is never held back for the pen: the pen near means the user is about to write, and no flash may
    // come then (it is dropped, the faint trace of one full STD stays until the next turn).
    int second = auto_kind == AK_PAGE && auto_page_idle;
    if (t - auto_t > (pen_age || second ? AUTO_PEN_MAX_AGE : AUTO_MAX_AGE)) { n_auto_aged++; return "the screen did not settle in time"; }
    if (sleeping) { n_auto_aged++; return "the display sleeps"; }
    if (auto_inked) { n_auto_ink++; return "ink was drawn (the user writes)"; }
    if (second && (pen || (sd_visible && sd_pen))) { n_auto_ink++; return "the pen came near (the user is about to write)"; }
    double sf = auto_stock_t(); // 1.8.26: the turn's own full STD is not one that covers its second pass
    if (sf > 0 && sf >= auto_t) { n_auto_stock++; return "a stock full refresh went over the screen"; }
    // a QML guess right after a stock full refresh (wake, document close with clock 10b) would be a second flash
    if (auto_kind == AK_QML && t_stock_full > 0 && auto_t - t_stock_full < cfg.stock_gap) {
        n_auto_stock++;
        return "it followed a stock full refresh";
    }
    if (t_our_full > 0 && t_our_full >= auto_t) { n_auto_covered++; return "our full clean went over the screen"; }
    if (second && !sd_visible) { n_auto_covered++; return "the document view was closed"; } // 1.8.26: never a flash on another view
    return NULL;
}

static const char *auto_try(int pen) {
    pthread_mutex_lock(&mu);
    if (!auto_req) { pthread_mutex_unlock(&mu); return "idle"; }
    finalize_pending();
    if (!auto_req) { pthread_mutex_unlock(&mu); return "idle"; } // 1.8.26: the frame finalized just now dropped it
    double t = now_s();
    char why[16];
    snprintf(why, sizeof why, "%s", auto_why);
    int kind = auto_kind;
    int page_idle = kind == AK_PAGE && auto_page_idle; // 1.8.26: the second pass of a full STD turn
    double own_full = auto_own_full;
    const char *drop = auto_drop_reason(t, pen);
    if (drop) {
        page_given_up();
        auto_req = 0;
        pthread_mutex_unlock(&mu);
        glog("auto %s: %s, no clean", why, drop);
        return "dropped";
    }
    // never with the pen near (1.6 rule: the user writes or is about to; the request ages out after
    // AUTO_PEN_MAX_AGE if the pen stays) and never while the on-screen keyboard is up (typing). The ScreenDriver's
    // states count only while the document view is visible: at xochitl's start it logs "pendingTilesChanged: true"
    // with the view hidden and no "false" until a document opens (seen on the tablet 2026-11-09 02:57:03 UTC).
    if (pen || (sd_visible && sd_pen)) auto_pen_held = 1;
    // 1.8.5: a page clean that fell due while the user leafed waits for page_settle without updates
    double quiet_need = auto_quiet_need > 0.0 ? auto_quiet_need : cfg.auto_quiet;
    double sf = auto_stock_t(); // 1.8.26: a second pass does not keep stock_gap away from its own turn's frame
    if (pen || kb_on || tr_active || promote_live || (sd_visible && (sd_pen || sd_gesture || sd_mono || tiles_busy(t))) ||
        (sf > 0 && t - sf < cfg.stock_gap) ||
        (t_last_update > 0 && t - t_last_update < quiet_need)) {
        pthread_mutex_unlock(&mu);
        return "wait";
    }
    // 1.8.26 (tf_idle 1): the second pass goes out once the SWTCON has finished the turn's frame. A full clean that
    // is queued over an update still running starts its pixels at different times (what cd_std 2 looked like to the
    // user: the page darkened and ghosted before it came clean). Without a reading of the SWTCON: 1.4 s after the
    // last update, the longest an STD was seen busy.
    if (page_idle && cfg.tf_idle) {
        int sw = swtcon_idle();
        if (sw == 0 || (sw < 0 && t_last_update > 0 && t - t_last_update < 1.4)) {
            pthread_mutex_unlock(&mu);
            return "wait";
        }
    }
    if (!fb || !prev_valid) {
        auto_req = 0;
        n_auto_fail++;
        pthread_mutex_unlock(&mu);
        return "nomodel";
    }
    // 1.8.6: a page turn's request is tried page_quiet after its frame: the model counts the page's own cells
    Score sc = compute_score_q(kind == AK_PAGE ? cfg.page_quiet : cfg.auto_quiet);
    const char *rec = rec_of(&sc);
    int gate = kind == AK_PAGE || kind == AK_QML;
    int page_cap = kind == AK_PAGE && auto_page_cap;
    // 1.8.22: a turn left without co-drive for a colour has its own, lower threshold (never a higher one)
    int page_col = kind == AK_PAGE && auto_page_col;
    int low = page_col && cfg.turn_px_col < cfg.turn_px;
    int tpx = low ? cfg.turn_px_col : cfg.turn_px;
    auto_req = 0;
    // 1.8.5 (see the header): a page request has its own threshold (1.8.6: turn_px), none when the cap armed it
    int pass = kind == AK_PAGE ? page_cap || page_model_ok(&sc, tpx)
               : kind == AK_QML ? !strcmp(rec, "now") || !strcmp(rec, "soon")
                                : 1;
    if (!pass) {
        n_auto_none++;
        if (kind == AK_PAGE) n_page_none++;
        pthread_mutex_unlock(&mu);
        if (kind == AK_PAGE)
            glog("auto %s: ghost=%ld (%s %d) hot=%d after page change %d, no clean", why, sc.ghost,
                 low ? "turn_px_col" : "turn_px", tpx, sc.hot, n_pchg);
        else
            glog("auto %s: rec=%s ghost=%ld hot=%d, no clean", why, rec, sc.ghost, sc.hot);
        return "none";
    }
    if (kind == AK_PAGE) rec = page_idle ? "second pass" : page_cap ? "cap" : low ? "turn_px_col" : "turn_px";
    Rect r = {0, 0, SW, SH};
    double age = t - auto_t;
    int inked = auto_inked;
    pthread_mutex_unlock(&mu);

    const char *rc = do_clean(&r);
    char swb[48];
    sw_note(swb, sizeof swb);
    pthread_mutex_lock(&mu);
    if (!strcmp(rc, "ok")) {
        n_auto_clean++;
        n_rclean_ok++;
        if (page_idle) n_second_pass++; // 1.8.26
        model_clean_rect(r);
    } else if (rc_retry(rc)) {
        if (!auto_req) { // try again at the next tick, same deadline
            auto_req = 1;
            auto_kind = kind;
            auto_inked = inked;
            auto_page_cap = page_cap; // 1.8.5
            auto_page_col = page_col; // 1.8.22
            auto_page_idle = page_idle; // 1.8.26
            auto_own_full = own_full;
            auto_t = t - age;
            snprintf(auto_why, sizeof auto_why, "%s", why);
        }
    } else {
        n_auto_fail++;
    }
    pthread_mutex_unlock(&mu);
    glog("auto %s: %s%s ghost=%ld hot=%d -> in-place clean %d,%d %dx%d %.0f ms later (wf %d, %s) -> %s", why,
         gate ? "rec=" : "", gate ? rec : "no model gate,", sc.ghost, sc.hot, r.x, r.y, r.w, r.h, age * 1000.0,
         cfg.clean_wf, swb, rc);
    return rc;
}

// 1.8.1 keyboard-open clean (see the header): ONE STD in place of the keyboard band once nothing but minor ui
// updates (the text cursor) came for kbo_quiet s after the keyboard frame (kbo_track drops it at anything else).
// Never while the display sleeps, a document gesture or its mono hold runs, or a promoted redraw is due; dropped
// once the keyboard closed, after a full clean of ours, or KBO_MAX_AGE s after the keyboard frame. 1.8.2: a tick
// with the pen near drops it (the clean would run into keys tapped with the pen; the clock holds its first ticks
// back while the pen is near, so it would come late anyway), and so does a stock full refresh less than stock_gap
// s before. Called WITHOUT mu at every QML tick, on the GUI thread; pen = this tick's pen flag. 1 = the clean ran
// or will be tried again (nothing else runs at this tick).
static int kbo_try(int pen) {
    pthread_mutex_lock(&mu);
    // first the frame still being assembled: it may be the keyboard's (arms the request, so its age is checked
    // below at this same tick) or a key press (kbo_track drops the request)
    finalize_pending();
    if (!kbo_req) { pthread_mutex_unlock(&mu); return 0; }
    double t = now_s();
    const char *drop = !kb_on                                ? "the keyboard closed"
                       : sleeping                              ? "the display sleeps"
                       : t - kbo_t > KBO_MAX_AGE               ? "the screen did not settle in time"
                       : t_our_full > 0 && t_our_full >= kbo_t ? "our full clean went over the screen"
                       : pen                                   ? "the pen is near (the keys may be tapped with it)"
                       : t_stock_full > 0 && t - t_stock_full < cfg.stock_gap ? "a stock full refresh came just before"
                                                               : NULL;
    if (drop) {
        kbo_drop(drop, t);
        pthread_mutex_unlock(&mu);
        return 0;
    }
    if (t - t_last_major < cfg.kbo_quiet || promote_live || (sd_visible && (sd_gesture || sd_mono))) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    Rect r = kbo_box;
    double age = t - kbo_t;
    kbo_req = 0;
    pthread_mutex_unlock(&mu);

    const char *rc = do_clean(&r);
    char swb[48];
    sw_note(swb, sizeof swb);
    pthread_mutex_lock(&mu);
    int quiet_retry = 0;
    if (!strcmp(rc, "ok")) {
        n_kbo_clean++;
        n_rclean_ok++;
        model_clean_rect(r);
    } else if (rc_retry(rc)) {
        if (kb_on && !sleeping) kbo_req = 1; // the next tick, same frame time and deadline
        quiet_retry = !strcmp(rc, "inflight"); // 1.8.3: every tick while the panel is busy: not logged each time
    } else {
        n_kbo_fail++;
    }
    pthread_mutex_unlock(&mu);
    if (!quiet_retry)
        glog("keyboard open: in-place clean of the keyboard band %d,%d %dx%d %.0f ms after it was drawn (%s) -> %s", r.x,
             r.y, r.w, r.h, age * 1000.0, swb, rc);
    return 1;
}

// 1.8.29: the page behind the white goes out. Every tick of the clock while a page is owed (GUI thread, without
// mu): once the panel has finished the white, or WH_HOLD after the turn whatever is going on, one complete
// full-screen refresh of the true frame (do_clean: its gate un-hides the frame and takes the stock clean). The
// pen near the screen does not hold it back: the page is not there yet. 1: it went out at this tick.
static int wh_tick(void) {
    if (!__atomic_load_n(&wh_owed, __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&mu);
    double t = now_s();
    hb_t = t;
    if (sleeping) { pthread_mutex_unlock(&mu); return 0; } // the wake's own refresh, or the first tick after it
    int late = t - wh_t0 >= WH_HOLD;
    int sw = swtcon_idle();
    if (!late && (sw == 0 || (sw < 0 && t_last_update > 0 && t - t_last_update < 1.4) || tr_active || promote_live ||
                  (sd_visible && tiles_busy(t)))) {
        pthread_mutex_unlock(&mu);
        return 0;
    }
    // once per turn, on an idle panel: did the white really go out? (the states are what the panel shows)
    long cn = 0, cb = 0;
    int chk = 1;
    if (!wh_checked && sw == 1 && __atomic_load_n(&wh_active, __ATOMIC_ACQUIRE)) {
        wh_checked = 1;
        chk = wh_state_white(&cn, &cb);
        wh_chk_n = chk < 0 ? -1 : cn;
        wh_chk_bad = chk < 0 ? -1 : cb;
        if (chk == 0) { wh_off = 1; n_wh_bad++; }
    }
    double ms = (t - wh_t0) * 1000.0;
    pthread_mutex_unlock(&mu);
    if (chk == 0)
        glog("white intermediate: %ld of %ld samples of the panel were not white after the turn: none until the next start (the double pass instead)", cb, cn);

    Rect r = {0, 0, SW, SH};
    const char *rc = do_clean(&r);
    if (!strcmp(rc, "pending")) { // xochitl's next frame is to be a complete refresh, but none has come for two ticks
        if (++wh_pend >= 2 || late) {
            withdraw_pending();
            rc = do_clean(&r);
        }
    } else
        wh_pend = 0;
    int ok = !strcmp(rc, "ok");
    if (!ok && rc_retry(rc)) return 0; // the next tick tries again
    char swb[48];
    sw_note(swb, sizeof swb);
    pthread_mutex_lock(&mu);
    if (ok) {
        n_rclean_ok++;
        model_clean_rect(r);
        hb_t = now_s();
    }
    pthread_mutex_unlock(&mu);
    glog("white intermediate: the page follows %.0f ms after the turn as one complete refresh (%s%s) -> %s", ms, swb,
         late ? ", not held back any longer" : "", rc);
    return ok;
}

// From the QML clock, on the GUI thread (the framebuffer object's thread): "GGQ <id> settle [pen]" every
// 250 ms for 2 s after each screen update (from_hb 0) and "GGQ <id> hb [pen]" every 20 s (from_hb 1).
// pen: the pen is near the screen (PenClosenessMonitor.penWasClose).
static const char *deferred_clean(int from_hb, int pen) {
    turn_full_age("the clock's tick");
    if (wh_tick()) return "page"; // 1.8.29: the page behind the white went out at this tick
    if (kbo_try(pen)) { // the keyboard band got its clean at this tick: one clean per tick
        pthread_mutex_lock(&mu);
        hb_t = now_s();
        if (auto_req && hb_t - auto_t > AUTO_PEN_MAX_AGE) { // as below: a stale request never outlives a tick
            auto_req = 0;
            n_auto_aged++;
            glog("auto %s: the screen did not settle within %.0f s, no clean", auto_why, AUTO_PEN_MAX_AGE);
        }
        pthread_mutex_unlock(&mu);
        return "keyboard";
    }
    pthread_mutex_lock(&mu);
    finalize_pending(); // may end the cycle: a stock in-place flash stood in for the clean (tr_on_complete)
    cd_fail_tick();
    double t = now_s();
    hb_t = t;
    if (!tr_active) { pthread_mutex_unlock(&mu); return auto_try(pen); }
    if (auto_req && t - auto_t > AUTO_PEN_MAX_AGE) {
        auto_req = 0;
        n_auto_aged++;
        glog("auto %s: the screen did not settle within %.0f s, no clean", auto_why, AUTO_PEN_MAX_AGE);
    }
    int drawn = tr_box.w > 0 && tr_box.h > 0;
    if (kb_on) tr_kb_held = 1; // a cycle that waited for the keyboard must not age out at the tick after it closed
    int long_age = tr_inked || tr_kb_held;
    const char *drop = !drawn && t - tr_vc_last > TR_FRAME_MAX                         ? "drew nothing"
                       : t - tr_vc_last > (long_age ? TR_INK_MAX_AGE : TR_MAX_AGE) ? "is too old"
                                                                                     : NULL;
    if (drop) {
        tr_active = 0;
        n_tr_aged++;
        glog("view change %s %s: no clean", tr_what, drop);
        pthread_mutex_unlock(&mu);
        return "aged";
    }
    double quiet = from_hb ? TR_HB_QUIET : tr_inked && cfg.tr_quiet < TR_INK_QUIET ? TR_INK_QUIET : cfg.tr_quiet;
    // minor ui updates (sync icons) are not part of the view's drawing: t_last_major skips them
    if (!drawn ||                                                         // nothing redrawn yet
        t - t_last_major < quiet ||                                       // the screen is still changing
        (t_last_major <= tr_vc_last && t - tr_vc_last < TR_FRAME_MAX) ||  // the newest view has not drawn yet
        (pen && (tr_inked || t - t_last_major < TR_INK_QUIET)) ||         // the user writes (or is about to)
        kb_on ||                                                          // the on-screen keyboard is up (typing)
        (sd_visible && (sd_gesture || sd_mono)) ||                        // a document gesture or its mono hold
        promote_live) {                                                   // the promoted redraw is due (~50 ms)
        pthread_mutex_unlock(&mu);
        return "wait";
    }
    Rect r = tr_box, r0 = tr_box; // r0: the changed area itself (1.8.3: a retry pads it once again, not twice)
    double age = t - tr_t0, wait0 = tr_wait_t;
    int wd = tr_withdrawn, late = tr_late, inked = tr_inked, kbh = tr_kb_held;
    char what[48];
    snprintf(what, sizeof what, "%s", tr_what);
    tr_active = 0;
    pthread_mutex_unlock(&mu);

    // 1.8.4: tr_full is 0, so a view change always gets the whole screen here (see the header); the regional branch
    // below runs only when a test sets tr_full directly.
    // 1.8.3: a regional clean waits for an idle panel (do_rclean "inflight"); once it has waited TR_IDLE_MAX, or when
    // the panel's state cannot be read ("noidle"), the whole screen gets the clean instead (it covers every pixel an
    // in-flight update drives, so it may go out meanwhile): still one flash for the view change
    int up = 0;
    if ((double)r.w * r.h >= cfg.tr_full * SW * SH) {
        r = (Rect){0, 0, SW, SH};
    } else if (wait0 > 0 && t - wait0 >= TR_IDLE_MAX) {
        r = (Rect){0, 0, SW, SH};
        up = 1;
    } else {
        r.x -= cfg.tr_pad; r.y -= cfg.tr_pad;
        r.w += 2 * cfg.tr_pad; r.h += 2 * cfg.tr_pad;
    }
    const char *rc = do_clean(&r);
    if (!strcmp(rc, "noidle")) {
        r = (Rect){0, 0, SW, SH};
        up = 1;
        rc = do_clean(&r);
    }
    char swb[48];
    sw_note(swb, sizeof swb);
    pthread_mutex_lock(&mu);
    int first_wait = 0;
    if (!strcmp(rc, "ok")) {
        n_tr_clean++;
        n_rclean_ok++;
        if (up) n_upgrade++;
        model_clean_rect(r);
    } else if (rc_retry(rc)) {
        // try again at the next settle ("pending": after the frame that takes the other stock flash; if that
        // frame goes over the area in place, it ends the cycle; "inflight": once the panel is idle); a view change
        // that started meanwhile keeps its own fields
        if (!tr_active) {
            tr_active = 1;
            tr_t0 = t - age;
            tr_box = (Rect){0, 0, 0, 0};
            tr_withdrawn = wd;
            tr_late = late;
            tr_inked = inked;
            tr_kb_held = kbh;
            tr_wait_t = wait0;
        }
        rect_union(&tr_box, r0);
        if (!strcmp(rc, "inflight") && tr_wait_t <= 0) {
            tr_wait_t = t;
            first_wait = 1;
        }
    } else {
        n_tr_fail++;
    }
    pthread_mutex_unlock(&mu);
    // an "inflight" retry comes at every tick while the panel is busy: only the first one of a cycle is logged
    if (strcmp(rc, "inflight") || first_wait) {
        char wb[64] = "";
        if (up) snprintf(wb, sizeof wb, " (whole screen: the panel stayed busy)");
        else if (wait0 > 0) snprintf(wb, sizeof wb, " (after waiting %.0f ms for the panel)", (t - wait0) * 1000.0);
        glog("view change %s: in-place clean %d,%d %dx%d %.0f ms later%s%s, stock flash %s (%s) -> %s", what, r.x, r.y, r.w,
             r.h, age * 1000.0, inked ? " (after writing)" : "", wb, late ? "went out anyway" : wd ? "withdrawn" : "not seen",
             swb, rc);
    }
    return rc;
}

// QML command channel: console.log("GGQ <id> <cmd> [args]")
static void on_command(const char *id, const char *cmd, const char *a1, const char *a2, const char *a3,
                       const char *a4, const char *rc) {
    n_cmds++;
    if (!strcmp(cmd, "status") || !strcmp(cmd, "rclean") || !strcmp(cmd, "caps") || !strcmp(cmd, "auto") ||
        !strcmp(cmd, "send") || !strcmp(cmd, "bleach")) write_status(id, rc);
    else if (!strcmp(cmd, "map")) { write_map(); write_status(id, rc); }
    else if (!strcmp(cmd, "reset")) { finalize_pending(); if (res) memset(res, 0, NS * sizeof *res); write_status(id, rc); }
    else if (!strcmp(cmd, "sim")) {
        // shadow mode: forget the residue of a rectangle as if it had been cleaned (model only, panel untouched)
        finalize_pending();
        Rect r = {atoi(a1), atoi(a2), atoi(a3), atoi(a4)};
        if (res && clip_rect(&r)) clear_residue_rect(r);
        n_sim++;
        write_status(id, rc);
    }
    else if (!strcmp(cmd, "set")) { set_param(a1, a2); write_status(id, rc); }
    else if (!strcmp(cmd, "hello")) { glog("hello from QML, fb=%p bpl=%lld", (void *)fb, fb_bpl); write_status(id, rc); }
}

// ---------------------------------------------------------------- message handler
static void msg_to_ascii(const QString *msg, char *out, size_t cap) {
    size_t n = 0;
    if (msg && msg->ptr) {
        long long len = msg->size;
        for (long long i = 0; i < len && n + 1 < cap; i++) {
            uint16_t ch = msg->ptr[i];
            out[n++] = ch < 128 ? (char)ch : '?';
        }
    }
    out[n] = 0;
}

static void our_handler(int type, const QMessageLogContext *ctx, const QString *msg) {
    const char *cat = ctx ? ctx->category : NULL;
    int swallow = 0;
    if (cat && !strcmp(cat, "rm.framebuffer.updates")) {
        char m[2048];
        msg_to_ascii(msg, m, sizeof m);
        pthread_mutex_lock(&mu);
        on_update_line(m);
        pthread_mutex_unlock(&mu);
        swallow = !cfg.debug;
    } else if (cat && !strcmp(cat, "xofm.libs.ghostbuster") && msg && msg->size >= 10 && msg->ptr[0] == 'v') {
        char m[160];
        msg_to_ascii(msg, m, sizeof m);
        if (!strncmp(m, "viewChange", 10)) {
            pthread_mutex_lock(&mu);
            int go = on_view_change(m);
            pthread_mutex_unlock(&mu);
            if (go == 4) {
                int w = withdraw_pending();
                glog("page turn: promoted flash taken back before its frame (%s), withdrawn=%d", unpromote_why, w);
            }
            if (go) wd_kick();
        }
    } else if (cat && !strcmp(cat, "xofm.modules.devicesceneview.screendriver") && msg && msg->size >= 8) {
        char m[160];
        msg_to_ascii(msg, m, sizeof m);
        pthread_mutex_lock(&mu);
        int go = on_sd_line(m);
        pthread_mutex_unlock(&mu);
        if (go == 1) wd_kick();
        else if (go == 2 || go == 5) {
            force_pending();
            if (go == 5) tfa_kick(); // 1.8.22: the turn's byte is taken back if no frame takes it within 1 s
        } else if (go == 3 || go == 4) {
            int w = withdraw_pending();
            glog("gesture end: promoted flash taken back before its redraw (%s), withdrawn=%d", unpromote_why, w);
            if (go == 4) wd_kick();
        }
    } else if (cat && !strcmp(cat, "rm.batterymanager") && msg && msg->size >= 10) {
        char m[200];
        msg_to_ascii(msg, m, sizeof m);
        pthread_mutex_lock(&mu);
        int back = on_power_line(m);
        pthread_mutex_unlock(&mu);
        if (back) restore_pending_now();
    } else if (cat && (!strcmp(cat, "qml") || !strcmp(cat, "js") || !strcmp(cat, "default")) && msg && msg->size >= 4 &&
               msg->ptr[0] == 'G' && msg->ptr[1] == 'G' && msg->ptr[2] == 'Q' && msg->ptr[3] == ' ') {
        char m[512];
        msg_to_ascii(msg, m, sizeof m);
        char id[32] = "0", cmd[32] = "", a1[64] = "", a2[64] = "", a3[64] = "", a4[64] = "";
        sscanf(m + 4, "%31s %31s %63s %63s %63s %63s", id, cmd, a1, a2, a3, a4);
        const char *rc = "";
        if (!strcmp(cmd, "settle") || !strcmp(cmd, "hb")) {
            // the QML clock: never forwarded (it would fill the journal), no status file
            deferred_clean(!strcmp(cmd, "hb"), !strcmp(a1, "pen"));
            return;
        }
        if (!strcmp(cmd, "auto")) {
            // need-based clean (see auto_try); outside mu on purpose, like rclean
            pthread_mutex_lock(&mu);
            arm_auto(AK_QML, a1[0] ? a1 : "doc");
            pthread_mutex_unlock(&mu);
            rc = auto_try(!strcmp(a2, "pen"));
        } else if (!strcmp(cmd, "rclean")) {
            // outside mu on purpose: the implementation logs while it runs (see model_clean_rect)
            Rect r = {atoi(a1), atoi(a2), atoi(a3), atoi(a4)};
            rc = do_clean(&r);
            char swb[48];
            sw_note(swb, sizeof swb);
            pthread_mutex_lock(&mu);
            if (!strcmp(rc, "ok")) {
                n_rclean_ok++;
                model_clean_rect(r);
            } else {
                n_rclean_fail++;
            }
            pthread_mutex_unlock(&mu);
            glog("rclean %s,%s %sx%s (%s) -> %s", a1, a2, a3, a4, swb, rc);
        } else if (!strcmp(cmd, "send") || !strcmp(cmd, "bleach")) {
            // 1.8.7 test commands; outside mu like rclean. The model is left alone: nothing is "cleaned" for it.
            int v[7] = {0, 0, 0, 0, 0, 0, 0};
            if (!cfg.testcmd) rc = "off";
            else if (__atomic_load_n(&sleeping, __ATOMIC_ACQUIRE)) rc = "asleep";
            else if (cmd[0] == 'b') rc = do_test_update(1, 8, 0, 0, NULL);
            else if (sscanf(m + 4, "%*s %*s %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7 ||
                     !send_args_ok(v[0], v[1], v[2])) rc = "badargs";
            else {
                Rect r = {v[3], v[4], v[5], v[6]};
                rc = do_test_update(0, v[0], v[1], v[2], &r);
            }
            glog("%s wf %d pm %d flags %d rect %d,%d %dx%d -> %s", cmd, v[0], v[1], v[2], v[3], v[4], v[5], v[6], rc);
        } else if (!strcmp(cmd, "caps")) {
            void *fbo;
            int st = xo_check(&fbo);
            rc = st > 0 ? "ready" : st == 0 ? "notready" : "unsupported";
        }
        pthread_mutex_lock(&mu);
        on_command(id, cmd, a1, a2, a3, a4, rc);
        pthread_mutex_unlock(&mu);
        swallow = !cfg.debug;
    }
    if (!swallow && next_handler && next_handler != our_handler) next_handler(type, ctx, msg);
}

// Chain our handler in front of Qt's default right at load time, so the order never depends on
// when (or whether) xochitl installs its own handler.
static void install_ours(void) {
    if (our_installed) return;
    if (!real_install)
        real_install = (QtMessageHandler(*)(QtMessageHandler))dlsym(
            RTLD_NEXT, "_Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE");
    if (!real_install) return;
    QtMessageHandler old = real_install(our_handler);
    if (!next_handler) next_handler = old;
    if (!qt_default_h && old != our_handler) qt_default_h = old; // at load time this is Qt's own handler
    our_installed = 1;
}

// interposed: xochitl calls this to install its own handler
QtMessageHandler _Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE(QtMessageHandler h) {
    if (!real_install)
        real_install = (QtMessageHandler(*)(QtMessageHandler))dlsym(
            RTLD_NEXT, "_Z22qInstallMessageHandlerPFv9QtMsgTypeRK18QMessageLogContextRK7QStringE");
    if (!real_install) return NULL;
    if (gg_off) return real_install(h); // other processes: plain pass-through
    if (h == our_handler) return real_install(h);
    QtMessageHandler prev_app = next_handler;
    if (h) {
        next_handler = h;
    } else {
        // restore default: ask Qt for it by installing default and capturing what it returns
        QtMessageHandler cur = real_install(NULL);
        (void)cur;
        QtMessageHandler def = real_install(our_handler); // returns the default handler
        if (!def || def == our_handler) def = qt_default_h;
        next_handler = def;
        our_installed = 1;
        return prev_app;
    }
    QtMessageHandler old = real_install(our_handler);
    our_installed = 1;
    if (!prev_app) prev_app = old; // first install: old is Qt's default handler
    return prev_app;
}

// Child processes of xochitl (e.g. /usr/bin/xochitl_pdf_renderer) inherit LD_PRELOAD; only the
// application itself is ever observed.
static int exe_is_xochitl(const char *exe) { return exe && !strcmp(exe, "/usr/bin/xochitl"); }

__attribute__((constructor)) static void gg_init(void) {
#ifdef __linux__
    char exe[256];
    ssize_t xl = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[xl > 0 ? xl : 0] = 0;
    if (!exe_is_xochitl(exe)) {
        gg_off = 1;
        return;
    }
#endif
    const char *dbg = getenv("GGSENSE_DEBUG");
    if (dbg && *dbg == '1') cfg.debug = 1;
    mkdir(RUNDIR, 0755);
    char line[64];
    int n = snprintf(line, sizeof line, "{\"loaded\":\"%s\",\"pid\":%d}", GG_VERSION, (int)getpid());
    write_file(RUNDIR "/loaded.json", line, (size_t)n);
    install_ours();
}
