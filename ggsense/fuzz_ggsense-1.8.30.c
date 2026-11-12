// Random event sequences through the deferred view-change logic, under ASan/UBSan.
// Invariants: no crash; a non-empty tr_box stays on screen; counters only grow; tr_active implies defer on
// or a cycle started before defer went off (set_defer ends cycles, so: tr_active implies cfg.defer).
#define RUNDIR TESTRUN
#include "ggsense-1.8.30.c"

static uint8_t *frame;
static unsigned long long rng = 88172645463325252ULL;
static unsigned rnd(unsigned n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng % n); }

// invariants checked around every single line (review fixes): a promote only after a gesture mono state, never with
// the pen near, never asleep or with the document view hidden; a promoted byte never outlives a frame or a sleep
static unsigned long bad_inv;
static unsigned long kbo_cleans_this_open;
static long cur_step;
static void inv(const char *what) { if (bad_inv++ < 5) printf("%s at step %ld\n", what, cur_step); }
static void line(const char *s) {
    pthread_mutex_lock(&mu);
    on_update_line(s);
    pthread_mutex_unlock(&mu);
    if (!strncmp(s, "swapBuffers:", 12) && promote_live) inv("promoted byte outlived a frame");
}
static void ggq_raw(const char *cat, const char *t) {
    uint16_t u[600];
    size_t n = strlen(t);
    for (size_t i = 0; i < n; i++) u[i] = (uint16_t)(unsigned char)t[i];
    QString qs = {NULL, u, (long long)n};
    QMessageLogContext ctx = {2, 0, NULL, NULL, cat};
    our_handler(0, &ctx, &qs);
}
static void ggq(const char *cat, const char *t) {
    int mg = sd_mono_gesture, pen = sd_pen, vis = sd_visible, slp = sleeping;
    unsigned long p = n_promote;
    ggq_raw(cat, t);
    if (n_promote != p && (!mg || pen || !vis || slp)) inv("promote without a gesture mono state, pen near, hidden or asleep");
    if (sleeping && promote_live) inv("promoted byte outlived a sleep");
    if (auto_req && auto_page_cap && auto_kind != AK_PAGE) inv("page cap flag on a request that is not a page's");
    // 1.8.26: the second pass of a full STD turn is a page request without the model's gate
    if (auto_req && auto_page_idle && (auto_kind != AK_PAGE || !auto_page_cap)) inv("second-pass flag on a request that is not a capped page request");
    if (n_pchg < 0 || cfg.page_gap < 1 || cfg.page_px < 0 || cfg.page_every < 0 || cfg.turn_px < 0 ||
        cfg.page_quiet < 0.2 || cfg.page_quiet > 2.0)
        inv("page rule out of range");
    // 1.8.6: a waiting page request never needs less quiet than page_quiet, nor a waiting request more than page_settle
    if (auto_req && auto_kind == AK_PAGE && auto_quiet_need < cfg.page_quiet - 1e-9 && auto_quiet_need < cfg.page_settle - 1e-9 &&
        auto_quiet_need < cfg.auto_quiet - 1e-9)
        inv("page request waits less than its quiet");
}
static uint8_t *want; // 1.8.29: the picture as it was painted (what the frame holds whenever nothing is hidden)
static void paint(int x, int y, int w, int h, int v) {
    for (int yy = y; yy < y + h && yy < SH; yy++)
        for (int xx = x; xx < x + w && xx < SW; xx++) {
            uint8_t *p = frame + (size_t)yy * SW * 4 + (size_t)xx * 4;
            p[0] = p[1] = p[2] = (uint8_t)v;
            p[3] = 255; // 1.8.29: as the painter writes an RGB32 pixel (a sentinel's alpha byte is 0)
            memcpy(want + (p - frame), p, 4);
        }
}

// pretend s seconds have passed (every stored time moves back by s)
static void shift_time(double s) {
    t_last_update -= s; t_last_major -= s; tr_vc_last -= s; tr_t0 -= s; hb_t -= s / 50.0; auto_t -= s; kbo_t -= s;
    if (tr_wait_t > 0) tr_wait_t -= s;
    if (t_stock_full > 0) t_stock_full -= s;
    if (t_our_full > 0) t_our_full -= s;
    if (sleep_soon_t > 0) sleep_soon_t -= s;
    if (sd_full_t > 0) sd_full_t -= s;
    if (sd_newpage_t > 0) sd_newpage_t -= s;
    if (sd_newpage_prev_t > 0) sd_newpage_prev_t -= s; // 1.8.26
    if (auto_own_full > 0) auto_own_full -= s;
    if (turn_full_t > 0) turn_full_t -= s;
    if (wh_t0 > 0) wh_t0 -= s; // 1.8.29
    if (sd_open_t > 0) sd_open_t -= s;
    if (t_pchg_prev > 0) t_pchg_prev -= s; // 1.8.5
    // 1.8.13/1.8.14: the turn verdict's stamps (ms on the same clock)
    long ms = (long)(s * 1000.0);
    cd_pend_ms -= ms; cd_turn_ms -= ms; cd_fail_ms -= ms;
    if (cd_turn_prev_t > 0) cd_turn_prev_t -= s;
    if (pend_active) pend_t -= s;
    for (int c = 0; c < NCELL; c++) if (cell_touch[c] > 0) cell_touch[c] -= s;
}

// 1.8.26: what the chroma gate must say for the page the host "holds" (the promise to the reader of a scanned book)
static int fz_colour_page(void) {
    if (host_colour <= 0) return 0;
    if (cfg.tf_chroma <= 0) return 1;
    long ch = (long)host_colour * host_chroma / 100;
    return ch * 100 >= (long)cfg.tf_chroma * host_colour && ch * 10000 >= (long)cfg.tf_chroma_min * host_px;
}

// 1.8.29, the white intermediate: the frame is covered only with a page owed; the frame and its shadow always hold
// the picture as it was painted (sampled at every step, compared in full when a hide ends); a tick that finds the
// page owed and nothing in its way brings it
static int fz_wh_must(void) {
    return wh_owed && !sleeping && !strcmp(host_rc, "ok") && host_sw == 1 && !tr_active && !promote_live &&
           !(sd_visible && tiles_busy(now_s()));
}
static void fz_tick(const char *t) {
    int must = fz_wh_must();
    ggq("qml", t);
    if (must && wh_owed) inv("the page stayed owed at a tick with nothing in its way");
}
static void fz_wh_picture(int was_active) {
    if (wh_active && !wh_owed) inv("frame covered without a page owed");
    if (wh_active && !wh_shadow) { inv("frame covered without a shadow"); return; }
    const uint32_t *f = (const uint32_t *)(const void *)frame, *w = (const uint32_t *)(const void *)want;
    if (was_active && !wh_active && memcmp(frame, want, (size_t)SW * SH * 4)) inv("the frame is not the painted picture after a hide ended");
    for (int y = 3; y < SH; y += 29)
        for (int x = y % 31; x < SW; x += 31) {
            size_t k = (size_t)y * SW + (size_t)x;
            uint32_t v = f[k];
            if (v == WH_SENT) {
                if (!wh_active) { inv("a sentinel in the frame with nothing hidden"); return; }
                v = wh_shadow[k];
            }
            if (v != w[k]) { inv("the frame or its shadow lost a painted pixel"); return; }
        }
}

int main(int argc, char **argv) {
    long steps = argc > 1 ? atol(argv[1]) : 200000;
    int trace = getenv("FUZZ_TRACE") != NULL;
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 10) * 0x9E3779B97F4A7C15ULL;
    mkdir(TESTRUN, 0755);
    frame = malloc((size_t)SW * SH * 4);
    memset(frame, 255, (size_t)SW * SH * 4);
    want = malloc((size_t)SW * SH * 4);
    memset(want, 255, (size_t)SW * SH * 4);
    fb = frame;
    fb_bpl = SW * 4;
    cfg.quiet_s = 0;
    xo_state = 0;
    cfg.defer = 1;
    ggq("qml", "GGQ 0 hb");
    line("swapBuffers: QFlags() QRegion(0,0 16x16) EPFramebuffer::PresentationMode::Default");
    static const char *cls[] = {"ui ........", "content ...", "pen .......", "pen (ui) ..", "pen (clamp)", "pen mdither",
                                "mono ......", "animation .", "overlay ..."};
    static const char *rcs[] = {"unsupported", "ok", "busy", "pending", "notready", "sleepmap", "empty"};
    static const char *modes[] = {"SettingsOpened", "SettingsClosed", "DocumentViewDialogOpened", "DocumentViewDialogClosed"};
    unsigned long bad = 0, prev_sum = 0;
    char b[300];
    for (long i = 0; i < steps; i++) {
        unsigned op = rnd(119); // 1.8.26: 116..118 = a reader's page turns
        int ticked = 0;
        unsigned long ok_before = n_rclean_ok, promote_before = n_promote, auto_before = n_auto_clean, trc_before = n_tr_clean;
        unsigned long kbo_clean_before = n_kbo_clean, kbo_arm_before = n_kbo;
        unsigned long sp_before = n_second_pass, tf_before = n_turn_full; // 1.8.26
        unsigned long whr_before = n_wh_reveal; // 1.8.29
        int wh_was = wh_active;
        int slept_before = sleeping, tick_pen = 0, promote_before_live = promote_live;
        cur_step = i;
        if (trace) fprintf(stderr, "STEP %ld op %u\n", i, op);
        // 1.8.3: the panel's state as the gate reads it, and the temperature
        if (!rnd(3)) host_sw = rnd(3) ? (rnd(2) ? 1 : 0) : (rnd(8) ? 0 : -1);
        if (!rnd(50)) host_temp = rnd(4) ? 18.0 + rnd(150) / 10.0 : NAN;
        if (!rnd(6)) host_text_ok = rnd(2); // 1.8.15: whether a clean goes out with TEXT targets
        if (!rnd(6)) host_queued = rnd(2);  // 1.8.15: an update waits to be accepted
        if (!rnd(4)) host_cd_verdict = rnd(3); // 1.8.13: what the host helper answers to the next page turns
        if (!rnd(6)) host_colour = rnd(3) ? 300 * rnd(2) : -1; // 1.8.22: what the page on the screen "holds" (turn_full)
        if (!rnd(8)) { host_chroma = rnd(3) ? 100 : (int)rnd(101); host_px = rnd(2) ? 0 : 218700; } // 1.8.26: how much of it is real colour
        if (op >= 116) {
            // 1.8.26: a reader turns the pages of an open document (what turn_full 2 is for). Sometimes the scene is
            // set first (the view up, the pen away, more than 10 s after the open, a colour page or a scanned one);
            // then one to three turns, each frame as its byte says; then the reader waits (the clock's ticks every
            // 250 ms while the panel finishes the frame) or the next steps interrupt.
            const char *sdc = "xofm.modules.devicesceneview.screendriver";
            static int rd_pg;
            if (!rnd(3)) {
                ggq(sdc, " - reset");
                ggq(sdc, "visibleChanged: true");
                ggq(sdc, "penCloseChanged: false");
                ggq(sdc, "gestureModeChanged: false");
                ggq("qml", "GGQ 9 set codrive 1");
                ggq("qml", "GGQ 9 set page_clean 1");
                { // 1.8.29: the white intermediate (the default), the double pass or the single flash
                    unsigned m = rnd(8);
                    ggq("qml", m < 4 ? "GGQ 9 set turn_full 3" : m < 7 ? "GGQ 9 set turn_full 2" : "GGQ 9 set turn_full 1");
                    if (!rnd(4)) wh_off = 0; // as after a restart
                    host_wh_frame = rnd(12) != 0;
                    host_wh_state = rnd(10) ? 1 : (int)rnd(2) - 1;
                    if (rnd(8)) { xo_state = 0; hb_t = now_s(); ggq("qml", "GGQ 81 set defer 1"); } // the clock is alive
                }
                if (rnd(5)) { host_colour = 300; host_chroma = 100; host_px = 0; }
                else { host_colour = 12000; host_chroma = 6; host_px = 218700; } // a scanned black-and-white page
                shift_time(11.0);
            }
            unsigned turns = 1 + rnd(3);
            for (unsigned q = 0; q < turns; q++) {
                rd_pg ^= 1;
                ggq(sdc, "finalizeNewPage:");
                int prom = promote_live && turn_full_np;
                shift_time(0.05);
                // the page before's two text lines go, this page's come: the changed box is the page (cd_area)
                paint(160, 100 + 30 * !rd_pg, 1300, 12, 255); paint(160, 2000 + 30 * !rd_pg, 1300, 12, 255);
                paint(160, 100 + 30 * rd_pg, 1300, 12, 0); paint(160, 2000 + 30 * rd_pg, 1300, 12, 0);
                if (prom) {
                    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                    line(" - full update, STD");
                } else {
                    line("swapBuffers: QFlags() QRegion(112,0 1508x2160) EPFramebuffer::PresentationMode::Default");
                    line(" - content ...: QRegion(size=2, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 1508x72)])");
                    line(" - merged UI/Content");
                }
                if (q + 1 < turns) shift_time((double)rnd(2500) / 1000.0);
            }
            if (rnd(4)) {
                host_rc = rnd(6) ? "ok" : rcs[rnd(7)];
                tick_pen = !rnd(8);
                unsigned n = 1 + rnd(14), busy = rnd(7);
                for (unsigned q = 0; q < n; q++) {
                    shift_time(0.25);
                    host_sw = q < busy ? 0 : rnd(10) ? 1 : -1;
                    promote_before_live = promote_live;
                    fz_tick(tick_pen ? "GGQ 0 settle pen" : "GGQ 0 settle");
                    if (n_rclean_ok != ok_before) break; // the checks below read the panel's state as that clean saw it
                }
                ticked = 1;
            }
        } else if (op >= 100) {
            // 1.8: ScreenDriver, battery manager, keyboard frames, options
            static const char *sdl[] = {"visibleChanged: true", "visibleChanged: false", " - clearing on next update",
                                        " - clear on next update", "gestureModeChanged: true", "gestureModeChanged: false",
                                        " - updating mono state, full -> mono", " - updating mono state, mono -> full",
                                        " - doing nothing...", " - full update..", "pendingTilesChanged: true",
                                        "pendingTilesChanged: false", " - reset", "visibleChanged: tru", " - doing nothing",
                                        " - clean the screen, dirty=0.050000", " - clean the screen, dirty=0.004",
                                        " - clean the screen, dirty=nan", " - clean the screen, dirty=", " - clean the screen, dirty=7",
                                        "finalizeNewPage:", "finalizeNewPage:", "penCloseChanged: true",
                                        "penCloseChanged: false", "selectionModeChanged: true", "selectionModeChanged: false"};
            static const char *pwl[] = {"Changing display state from Normal to DeepSleep (setDisplayState /x)",
                                        "Changing display state from DeepSleep to Normal (setDisplayState /x)",
                                        "Waking up display from keyEvent=16777399, lastWakeupReason=Unknown (x)",
                                        "Light sleep timer timed out (operator() /x)", "Changing display state from ",
                                        "Changing display state from Normal to ", "Changing display state from X to Normal"};
            static const char *opts[] = {"GGQ 9 set sd_close 1", "GGQ 9 set sd_close 0", "GGQ 9 set gesture_clean 1",
                                         "GGQ 9 set page_clean 1", "GGQ 9 set kb_clean 1", "GGQ 9 set page_min 0.3",
                                         "GGQ 9 set stock_gap 0", "GGQ 9 set stock_gap 3", "GGQ 9 set kb_clean 0",
                                         "GGQ 9 set gesture_min 0", "GGQ 9 set gesture_min 0.05", "GGQ 9 set gesture_clean 0",
                                         "GGQ 94 set kb_open_clean 1", "GGQ 94 set kb_open_clean 1", "GGQ 9 set kb_open_clean 0",
                                         "GGQ 9 set kbo_quiet 0.2", "GGQ 9 set kbo_quiet 0.4",
                                         // 1.8.5: the page rule
                                         "GGQ 9 set page_gap 1", "GGQ 9 set page_gap 2", "GGQ 9 set page_gap 4",
                                         "GGQ 9 set page_px 0", "GGQ 9 set page_px 80000", "GGQ 9 set page_px 9e99",
                                         "GGQ 9 set page_every 0", "GGQ 9 set page_every 1", "GGQ 9 set page_every 6",
                                         "GGQ 9 set page_flip 0.5", "GGQ 9 set page_flip 2", "GGQ 9 set page_settle 0.4",
                                         "GGQ 9 set page_settle 3",
                                         // 1.8.6: a page turn's own threshold and timing
                                         "GGQ 9 set turn_px 0", "GGQ 9 set turn_px 6000", "GGQ 9 set turn_px 9e99",
                                         "GGQ 9 set page_quiet 0.2", "GGQ 9 set page_quiet 1.5",
                                         // 1.8.13: the co-drive's verdict path (the host helper answers host_cd_verdict)
                                         "GGQ 9 set codrive 0", "GGQ 9 set codrive 1", "GGQ 9 set codrive 1",
                                         "GGQ 9 set codrive 2", "GGQ 9 set cd_minv 0", "GGQ 9 set cd_minv 2.5",
                                         "GGQ 9 set clean_text 0", "GGQ 9 set clean_text 1", "GGQ 9 set clean_text 1",
                                         // 1.8.22: the turn_full experiment
                                         "GGQ 9 set turn_full 1", "GGQ 9 set turn_full 1", "GGQ 9 set turn_full 0",
                                         // 1.8.26: the second pass (the default) and the chroma gate
                                         "GGQ 9 set turn_full 2", "GGQ 9 set turn_full 2", "GGQ 9 set turn_full 2",
                                         "GGQ 9 set tf_idle 0", "GGQ 9 set tf_idle 1", "GGQ 9 set tf_chroma 0",
                                         "GGQ 9 set tf_chroma 25",
                                         // 1.8.29: the white intermediate
                                         "GGQ 9 set turn_full 3", "GGQ 9 set turn_full 3"};
            unsigned k = rnd(23);
            if (k < 7) ggq("xofm.modules.devicesceneview.screendriver", sdl[rnd(26)]);
            else if (k < 9) ggq("rm.batterymanager", pwl[rnd(7)]);
            else if (k < 11) {
                const char *o = opts[rnd(sizeof opts / sizeof opts[0])]; // 1.8.26: 54 of them
                ggq("qml", o);
                // 1.8.3: the text option never turns the band clean on (an old clock sends "set kb_open_clean 1")
                if (strstr(o, "kb_open_clean") && cfg.kb_open_clean) inv("kb_open_clean turned on from text");
                if (!rnd(3)) {
                    cfg.kb_open_clean = rnd(2); // test-only: the machinery stays exercised under the gate
                    // 1.8.4: turning it off drops a waiting band request, as set_param does on the tablet (where the
                    // option is always 0); seeds 1 and 2 flagged this harness's own flip, not ggsense
                    if (!cfg.kb_open_clean && kbo_req) {
                        pthread_mutex_lock(&mu);
                        kbo_drop("test: kb_open_clean off", now_s());
                        pthread_mutex_unlock(&mu);
                    }
                }
            }
            else if (k < 13) {
                line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                line(" - ui ........: QRegion(size=2, bounds=(0,0 1620x1544) - [(0,0 1620x230), (0,298 1620x1246)])");
                line(rnd(2) ? " - overlay ...: QRegion(size=2, bounds=(0,1544 1620x616) - [(0,1544 1620x88), (0,1632 1620x528)])"
                            : " - animation .: QRegion(size=2, bounds=(0,1544 1620x616) - [(0,1544 1620x88), (0,1632 1620x528)])");
                if (rnd(2)) {
                    // the keyboard flow of the journals: the text cursor 0.445 s later (sometimes), a pause, a tick
                    if (rnd(2)) {
                        line("swapBuffers: QFlags() QRegion(237,254 2x36) EPFramebuffer::PresentationMode::Default");
                        line(" - ui ........: QRegion(237,254 2x36)");
                    }
                    shift_time((double)rnd(1200) / 1000.0);
                    host_rc = rnd(3) ? "ok" : rcs[rnd(7)];
                    tick_pen = !rnd(4);
                    // the frames above took any promoted byte (a frame always does): the promote check is for this tick
                    promote_before_live = promote_live;
                    fz_tick(tick_pen ? "GGQ 0 settle pen" : "GGQ 0 settle");
                    ticked = 1;
                }
            } else if (k < 15) {
                if (rnd(2)) paint(112, 0, 1508, 2088, rnd(2) ? 0 : 255);
                if (rnd(2)) ggq("xofm.modules.devicesceneview.screendriver", "finalizeNewPage:");
                if (promote_live && turn_full_np) { // 1.8.22: the turn's byte makes its frame a complete refresh
                    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                    line(" - full update, STD");
                } else {
                    line("swapBuffers: QFlags() QRegion(112,0 1508x2160) EPFramebuffer::PresentationMode::Default");
                    line(" - content ...: QRegion(size=2, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 1508x72)])");
                    line(" - merged UI/Content");
                }
            } else if (k == 15) {
                line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                line(rnd(2) ? " - sleep" : " - full screen animation");
            } else if (k == 19 || k == 20) {
                line("swapBuffers: QFlags() QRegion(237,254 2x36) EPFramebuffer::PresentationMode::Default");
                line(" - ui ........: QRegion(237,254 2x36)");
            } else if (k == 21) {
                line("swapBuffers: QFlags() QRegion(668,1641 139x118) EPFramebuffer::PresentationMode::Default");
                line(rnd(2) ? " - overlay ...: QRegion(668,1641 139x118)" : " - partial animation");
            } else if (k == 22) {
                line("swapBuffers: QFlags() QRegion(1448,1556 20x20) EPFramebuffer::PresentationMode::Default");
                line(" - ui ........: QRegion(1448,1556 20x20)");
            } else {
                // a whole gesture (or a selection) up to the ScreenDriver's decision, with random interruptions
                const char *sdc = "xofm.modules.devicesceneview.screendriver";
                ggq(sdc, rnd(4) ? "gestureModeChanged: true" : "selectionModeChanged: true");
                ggq(sdc, " - updating mono state, full -> mono");
                line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                line(" - full screen animation");
                ggq(sdc, "gestureModeChanged: false");
                if (!rnd(6)) ggq(sdc, rnd(2) ? "penCloseChanged: true" : "penCloseChanged: false");
                ggq(sdc, " - updating mono state, mono -> full");
                ggq(sdc, rnd(3) ? " - clean the screen, dirty=0.050000" : " - clean the screen, dirty=0.300000");
                ggq(sdc, rnd(4) ? " - doing nothing..." : " - full update..");
                unsigned nx = rnd(6);
                if (nx == 0) ggq(sdc, "gestureModeChanged: true");
                else if (nx == 1) ggq(sdc, "penCloseChanged: true");
                else if (nx == 2) { ggq(sdc, "visibleChanged: false"); ggq(sdc, " - clearing on next update"); }
                else if (nx == 3) ggq("rm.batterymanager", "Changing display state from Normal to DeepSleep (setDisplayState /x)");
                ggq(sdc, " - reset");
                if (rnd(2)) {
                    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
                    line(" - full update, STD");
                }
            }
        } else if (op < 30 && rnd(7) == 0) {
            // a status-bar update: the sync indicator (ui only, top band), sometimes with another class or larger
            static const char *st[] = {"QRegion(size=18, bounds=(1394,44 178x32) - [(1394,44 32x5), (1434,44 32x5), (1474,44 32x5)])",
                                       "QRegion(1394,44 178x32)", "QRegion(1300,0 320x120)", "QRegion(1394,44 178x90)",
                                       "QRegion(0,0 1620x110)", "QRegion(34,686 278x37)", "QRegion(1000,1000 200x180)"};
            unsigned q = rnd(7);
            snprintf(b, sizeof b, "swapBuffers: QFlags() %s EPFramebuffer::PresentationMode::Default", st[q]);
            line(b);
            snprintf(b, sizeof b, " - %s: %s", rnd(5) ? "ui ........" : cls[rnd(9)], st[q]);
            line(b);
        } else if (op < 30 && rnd(8) == 0) {
            // a menu or popup (side menu, + menu, toolbar strip)
            static const char *menus[] = {"0,0 414x2160", "581,1448 457x468", "0,80 112x2080", "300,300 1020x900"};
            snprintf(b, sizeof b, "swapBuffers: QFlags() QRegion(%s) EPFramebuffer::PresentationMode::Default", menus[rnd(4)]);
            line(b);
            snprintf(b, sizeof b, " - ui ........: QRegion(%s)", menus[rnd(4)]);
            line(b);
        } else if (op < 30) {
            int x = (int)rnd(1700) - 40, y = (int)rnd(2240) - 40, w = (int)rnd(900), h = (int)rnd(900);
            if (rnd(3) == 0 && x >= 0 && y >= 0) paint(x, y, w, h, rnd(2) ? 0 : 255);
            snprintf(b, sizeof b, "swapBuffers: QFlags() QRegion(%d,%d %dx%d) EPFramebuffer::PresentationMode::Default", x, y, w, h);
            line(b);
            snprintf(b, sizeof b, " - %s: QRegion(%d,%d %dx%d)", cls[rnd(9)], x, y, w, h);
            line(b);
        } else if (op < 38) {
            line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
            line(rnd(5) ? " - full update, STD" : " - sleep");
        } else if (op < 48) {
            snprintf(b, sizeof b, "viewChange xofm::libs::ghostbuster::IGhostBuster::ViewChangeMode::%s", modes[rnd(4)]);
            ggq("xofm.libs.ghostbuster", b);
            if (rnd(2)) tr_withdrawn = 1;
        } else if (op < 64) {
            host_rc = rcs[rnd(7)];
            tick_pen = !rnd(4);
            fz_tick(tick_pen ? "GGQ 0 settle pen" : "GGQ 0 settle");
            ticked = 1;
        } else if (op < 70) {
            static const char *au[] = {"GGQ 900001 auto doc", "GGQ 900002 auto closed", "GGQ 900003 auto doc pen",
                                       "GGQ 900004 auto", "GGQ 900005 auto xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"};
            host_rc = rcs[rnd(7)];
            unsigned a = rnd(5);
            tick_pen = a == 2;
            ggq("qml", au[a]);
        } else if (op < 74) {
            host_rc = rcs[rnd(7)];
            tick_pen = rnd(2);
            fz_tick(tick_pen ? "GGQ 0 hb pen" : "GGQ 0 hb");
            ticked = 1;
        } else if (op < 77) {
            ggq("qml", rnd(2) ? "GGQ 82 set defer 0" : "GGQ 81 set defer 1");
        } else if (op < 80) {
            static const char *ps[] = {"GGQ 9 set tr_quiet 7", "GGQ 9 set tr_quiet -1", "GGQ 9 set tr_quiet nan",
                                       "GGQ 9 set tr_full 2", "GGQ 9 set tr_pad 9999", "GGQ 9 set tr_full inf",
                                       "GGQ 9 rclean -5 -5 99999 99999", "GGQ 9 status", "GGQ 9 set auto_pad 1e40",
                                       "GGQ 9 set auto_quiet -inf", "GGQ 9 set clean_wf 3", "GGQ 9 set w_content 1.2",
                                       "GGQ 9 set min_cells -9e99", "GGQ 9 set quiet_s 0.4"};
            const char *o = ps[rnd(14)];
            ggq("qml", o);
            // 1.8.4: the text option never brings the regional view-change clean back
            if (strstr(o, "tr_full") && cfg.tr_full != 0.0) inv("tr_full above 0 from text");
            if (!rnd(3)) cfg.tr_full = rnd(2) ? 0.0 : 0.35; // test-only: the regional machinery stays exercised
        } else if (op < 95) {
            double s = (double)rnd(4000) / 1000.0;
            if (rnd(10) == 0) s = 40.0;
            shift_time(s);
        } else {
            static const char *junk[] = {"swapBuffers: QFlags(0x1", "swapBuffers: QRegion(size=5, bounds=(0,0 1x1) - [(",
                                         " - pen", " - full update", "viewChange", "GGQ 0 settle extra args here x y"};
            unsigned j = rnd(6);
            if (j == 4) ggq("xofm.libs.ghostbuster", junk[j]);
            else if (j == 5) ggq("qml", junk[j]);
            else line(junk[j]);
        }
        if (tr_active && tr_box.w > 0 && (tr_box.x < 0 || tr_box.y < 0 || tr_box.x + tr_box.w > SW || tr_box.y + tr_box.h > SH)) {
            if (bad++ < 5) printf("box off screen at step %ld: %d,%d %dx%d\n", i, tr_box.x, tr_box.y, tr_box.w, tr_box.h);
        }
        if (tr_active && !cfg.defer) {
            if (bad++ < 5) printf("cycle active with defer off at step %ld\n", i);
        }
        unsigned long sum = n_tr + n_wd_late + n_tr_clean + n_tr_fail + n_tr_absorbed + n_tr_aged + n_auto + n_auto_clean +
                            n_auto_none + n_auto_aged + n_auto_fail;
        // a tick never leaves a request older than AUTO_MAX_AGE behind (it is dropped before anything is cleaned)
        if (sleeping && slept_before && n_rclean_ok != ok_before) { if (bad++ < 5) printf("clean while sleeping at step %ld\n", i); }
        if (sleeping && slept_before && n_promote != promote_before) { if (bad++ < 5) printf("promote while sleeping at step %ld\n", i); }
        if (tr_active && sleeping) { if (bad++ < 5) printf("cycle active while sleeping at step %ld\n", i); }
        // review fixes: never an auto clean with the pen near or the keyboard up; a promote only after a gesture
        // mono state, never with the pen near; a promoted byte never outlives a frame or a sleep
        // (1.8.29: the page behind the white goes out after WH_HOLD whatever waits)
        if (n_rclean_ok != ok_before && promote_before_live && n_wh_reveal == whr_before) {
            if (bad++ < 5) printf("clean of ours between a promote and its redraw at step %ld\n", i);
        }
        if (n_auto_clean != auto_before && (tick_pen || (sd_visible && sd_pen) || kb_on)) {
            if (bad++ < 5) printf("auto clean with the pen near or the keyboard up at step %ld\n", i);
        }
        // 1.8.26: the second pass of a full STD turn is a flash on the page it was armed for: one per step at most, never
        // with the document view hidden, asleep, the pen near or the keyboard up
        if (n_second_pass != sp_before &&
            (n_second_pass != sp_before + 1 || !sd_visible || (sleeping && slept_before) || tick_pen || sd_pen || kb_on)) {
            if (bad++ < 5) printf("second pass with the view hidden, asleep, the pen near or the keyboard up at step %ld\n", i);
        }
        // 1.8.26: a turn is promoted to the full STD only with turn_full on and only on a page that holds real colour
        // (never on a scanned black-and-white page: balanced primaries, or a speck of colour)
        if (n_turn_full != tf_before && (!cfg.turn_full || !fz_colour_page())) {
            if (bad++ < 5) printf("turn promoted with turn_full off or on a page without real colour at step %ld\n", i);
        }

        // 1.8.15: a full-screen clean that went out with TEXT targets leaves no STD level behind
        if (n_rclean_ok != ok_before && last_clean_text && rect_full(host_last) && cd_std_next) {
            if (bad++ < 5) printf("cd_std_next set after a full text clean at step %ld\n", i);
        }
        if (cd_std_next && cd_text_next) { if (bad++ < 5) printf("cd_std_next and cd_text_next both set at step %ld\n", i); }
        if (n_rclean_ok != ok_before && last_clean_text && host_sw != 1 && host_queued) {
            if (bad++ < 5) printf("text clean over a waiting update at step %ld\n", i);
        }
        // review round 2 (liveness-3): a view change's clean never with the keyboard up, never before the screen
        // (status-bar updates aside) was quiet for tr_quiet
        if (n_tr_clean != trc_before && kb_on) { if (bad++ < 5) printf("view-change clean with the keyboard up at step %ld\n", i); }
        if (n_tr_clean != trc_before && now_s() - t_last_major < cfg.tr_quiet - 1e-6) {
            if (bad++ < 5) printf("view-change clean %.3f s after an update at step %ld\n", now_s() - t_last_major, i);
        }
        if (kb_on && (kb_box.x < 0 || kb_box.y < 0 || kb_box.x + kb_box.w > SW || kb_box.y + kb_box.h > SH)) {
            if (bad++ < 5) printf("keyboard box off screen at step %ld\n", i);
        }
        if (auto_req && (auto_kind <= AK_NONE || auto_kind > AK_QML)) { if (bad++ < 5) printf("bad auto kind at step %ld\n", i); }
        // 1.8.1 keyboard-open clean: only while the keyboard is up and before anything but minor updates came after
        // its frame (a key press, ink, a popup, a stock flash), after kbo_quiet, at most once per opening, never late
        if (n_kbo != kbo_arm_before) kbo_cleans_this_open = 0;
        if (n_kbo_clean != kbo_clean_before) {
            kbo_cleans_this_open += n_kbo_clean - kbo_clean_before;
            if (!kb_on) { if (bad++ < 5) printf("keyboard-open clean with the keyboard down at step %ld\n", i); }
            if (t_last_major > kbo_t) { if (bad++ < 5) printf("keyboard-open clean after a key press or another update at step %ld\n", i); }
            if (t_stock_full > kbo_t) { if (bad++ < 5) printf("keyboard-open clean after a stock flash at step %ld\n", i); }
            if (now_s() - t_last_major < cfg.kbo_quiet - 1e-6) { if (bad++ < 5) printf("keyboard-open clean too soon at step %ld\n", i); }
            if (kbo_cleans_this_open > 1) { if (bad++ < 5) printf("two keyboard-open cleans for one opening at step %ld\n", i); }
            if (kbo_box.y < SH / 2 - 100 || kbo_box.x < 0 || kbo_box.x + kbo_box.w > SW || kbo_box.y + kbo_box.h > SH ||
                (long long)kbo_box.w * kbo_box.h >= (long long)SW * SH * 95 / 100) {
                if (bad++ < 5) printf("keyboard-open clean outside the lower band at step %ld\n", i);
            }
            if (n_rclean_ok == ok_before) { if (bad++ < 5) printf("keyboard-open clean not counted at step %ld\n", i); }
            // 1.8.2: never at a tick with the pen near, never started later than KBO_MAX_AGE after the keyboard frame,
            // never within stock_gap of a stock full refresh (time does not move between the clean and these checks)
            if (tick_pen) { if (bad++ < 5) printf("keyboard-open clean at a tick with the pen near at step %ld\n", i); }
            if (now_s() - kbo_t > KBO_MAX_AGE + 1e-3) {
                if (bad++ < 5) printf("keyboard-open clean %.3f s after the keyboard frame at step %ld\n", now_s() - kbo_t, i);
            }
            if (t_stock_full > 0 && now_s() - t_stock_full < cfg.stock_gap - 1e-6) {
                if (bad++ < 5) printf("keyboard-open clean within stock_gap of a stock full refresh at step %ld\n", i);
            }
        }
        if (kbo_req && (!kb_on || sleeping || !cfg.kb_open_clean)) {
            if (bad++ < 5) printf("keyboard-open request with the keyboard down, asleep or off at step %ld\n", i);
        }
        if (ticked && kbo_req && now_s() - kbo_t > KBO_MAX_AGE) { if (bad++ < 5) printf("stale keyboard-open request after a tick at step %ld\n", i); }
        if (ticked && auto_req && now_s() - auto_t > AUTO_PEN_MAX_AGE) { if (bad++ < 5) printf("stale auto request after a tick at step %ld\n", i); }
        // 1.8.3 (rule K1): a clean of less than the whole screen only with the panel idle; a full one may go out anyway
        if (n_rclean_ok != ok_before && !rect_full(host_last) && last_sw != 1) {
            if (bad++ < 5) printf("box clean (%d,%d %dx%d) with the panel %d at step %ld\n", host_last.x, host_last.y, host_last.w,
                                  host_last.h, last_sw, i);
        }
        // 1.8.4: with the default tr_full 0 a view change's clean is always the whole screen (a regional STD stretches
        // the updates that run outside it); only the test-only band clean and an explicit rclean may be smaller
        if (cfg.tr_full == 0.0 && n_tr_clean != trc_before && n_kbo_clean == kbo_clean_before && !rect_full(host_last)) {
            if (bad++ < 5) printf("view-change box clean (%d,%d %dx%d) with tr_full 0 at step %ld\n", host_last.x, host_last.y,
                                  host_last.w, host_last.h, i);
        }
        if (sum < prev_sum) { if (bad++ < 5) printf("counter went down at step %ld\n", i); }
        fz_wh_picture(wh_was); // 1.8.29
        prev_sum = sum;
        if (hb_t > 0 && now_s() - hb_t > 80) ggq("qml", "GGQ 0 hb");
    }
    printf("steps=%ld tr=%lu late=%lu clean=%lu fail=%lu absorbed=%lu aged=%lu restore=%lu auto=%lu/%lu/%lu/%lu/%lu bad=%lu\n",
           steps, n_tr, n_wd_late, n_tr_clean, n_tr_fail, n_tr_absorbed, n_tr_aged, n_restore, n_auto, n_auto_clean,
           n_auto_none, n_auto_aged, n_auto_fail, bad);
    printf("promote=%lu/%lu unpromote=%lu withdraw=%lu\n", n_promote, n_promote_skip, n_unpromote, n_withdraw_host);
    printf("1.8.1: kbOpen armed=%lu cleaned=%lu dropped=%lu failed=%lu\n", n_kbo, n_kbo_clean, n_kbo_drop, n_kbo_fail);
    printf("1.8.3: inflight=%lu upgrade=%lu fullBusy=%lu (1.8.4: tr_full now %.2f)\n", n_inflight, n_upgrade, n_full_busy,
           cfg.tr_full);
    printf("1.8: sdClose=%lu sdSkip=%lu sleepBack=%lu sleepFrames=%lu force=%lu arm=%lu/%lu/%lu/%lu drop=%lu/%lu/%lu\n",
           n_sd_close, n_sd_skip, n_sleep_back, n_sleep_frame, n_force, n_arm[AK_PAGE], n_arm[AK_GESTURE], n_arm[AK_KB],
           n_arm[AK_QML], n_auto_stock, n_auto_ink, n_auto_covered);
    printf("1.8.22: turnFull=%lu took=%lu\n", n_turn_full, n_turn_full_took);
    printf("1.8.26: secondPass=%lu\n", n_second_pass);
    printf("1.8.29: hide=%lu absorb=%lu reveal=%lu stock=%lu early=%lu bad=%lu no=%lu off=%d\n", n_wh_hide, n_wh_absorb,
           n_wh_reveal, n_wh_stock, n_wh_early, n_wh_bad, n_wh_no, wh_off);
    bad += bad_inv;
    printf("invariant violations: %lu\n", bad_inv);
    return bad != 0;
}
