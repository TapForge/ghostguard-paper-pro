// Host-side test harness for ggsense.c (macOS / Linux). Includes the sensor directly and drives it
// with a synthetic RGB32 frame and log lines in the exact format xochitl 3.29.1 prints.
#define RUNDIR TESTRUN
#include "ggsense-1.8.30.c"

static uint8_t *frame;
static int fails;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (cond) printf("  ok   " __VA_ARGS__);                           \
        else { printf("  FAIL " __VA_ARGS__); fails++; }                   \
        printf("\n");                                                      \
    } while (0)

static void px_set(int x, int y, int v) {
    uint8_t *p = frame + (size_t)y * SW * 4 + (size_t)x * 4;
    p[0] = p[1] = p[2] = (uint8_t)v;
    p[3] = 255;
}
static void fill(int x, int y, int w, int h, int v) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px_set(xx, yy, v);
}
// "text": 1/6 of the 2x2 blocks black
static void text(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if ((xx / 2) % 3 == 0 && (yy / 2) % 2 == 0) px_set(xx, yy, 0);
}
// 1.8.4: the Mac's CLOCK_MONOTONIC counts in whole microseconds (clock_getres: 1000 ns), so two events handled
// within the same microsecond got the same time, and a frame that came right after a view change looked like one
// from before it (T18 "status ... trActive" failed about one run in three: "the newest view has not drawn yet",
// t_last_major <= tr_vc_last). On the tablet the clock counts in nanoseconds and xochitl's lines are far apart.
// Each event of this harness now first waits until the clock has moved past the END of the previous event
// (tock), so every time taken while handling one event is earlier than any time taken while handling the next
// (waiting only for the previous event's start was not enough: the view change sets tr_vc_last after its start,
// and the frame's pend_t could still fall in that microsecond). At most 1 us per event.
static double t_prev_event;
static void tick(void) {
    while (now_s() <= t_prev_event) {}
}
static void tock(void) { t_prev_event = now_s(); }
static void line(const char *s) {
    tick();
    pthread_mutex_lock(&mu);
    on_update_line(s);
    pthread_mutex_unlock(&mu);
    tock();
}
static void partial(const char *cls, int x, int y, int w, int h) {
    char b[256];
    snprintf(b, sizeof b, "swapBuffers: QFlags() QRegion(%d,%d %dx%d) EPFramebuffer::PresentationMode::Default", x, y, w, h);
    line(b);
    snprintf(b, sizeof b, " - %s: QRegion(%d,%d %dx%d)", cls, x, y, w, h);
    line(b);
}
static void full(void) {
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full update, STD");
}
static Score score(void) {
    pthread_mutex_lock(&mu);
    finalize_pending();
    Score s = compute_score();
    pthread_mutex_unlock(&mu);
    return s;
}
static void show(const char *what, Score s) {
    printf("  [%s] ghost=%ld active=%ld hot=%d maxCell=%d bbox=%d,%d..%d,%d light=%.2f\n", what, s.ghost,
           s.ghost_active, s.hot, s.maxcell, s.x0, s.y0, s.x1, s.y1, s.light);
}
static void ggq(const char *cat, const char *text_) {
    uint16_t u[600];
    size_t n = strlen(text_);
    for (size_t i = 0; i < n; i++) u[i] = (uint16_t)(unsigned char)text_[i];
    QString qs = {NULL, u, (long long)n};
    QMessageLogContext ctx = {2, 0, NULL, NULL, cat};
    tick();
    our_handler(0, &ctx, &qs);
    tock();
}
static void vc(const char *mode) {
    char b[160];
    snprintf(b, sizeof b, "viewChange xofm::libs::ghostbuster::IGhostBuster::ViewChangeMode::%s", mode);
    ggq("xofm.libs.ghostbuster", b);
}
// pretend s seconds have passed since the last frame / view change / auto request / cell change
static void elapse(double s) {
    t_last_update -= s;
    t_last_major -= s;
    tr_vc_last -= s;
    tr_t0 -= s;
    auto_t -= s;
    if (cd_pend_ms) cd_pend_ms -= (long)(s * 1000.0); // 1.8.13: the co-drive's verdict times are wall clock too
    if (cd_turn_ms) cd_turn_ms -= (long)(s * 1000.0);
    if (cd_fail_ms) cd_fail_ms -= (long)(s * 1000.0);
    kbo_t -= s;
    if (cd_turn_prev_t > 0) cd_turn_prev_t -= s; // 1.8.14
    if (tr_wait_t > 0) tr_wait_t -= s;
    if (t_stock_full > 0) t_stock_full -= s;
    if (pend_active) pend_t -= s; // the frame being assembled is finalized later with this time
    if (t_our_full > 0) t_our_full -= s;
    if (t_popup > 0) t_popup -= s; // 1.8.22
    if (t_ink > 0) t_ink -= s;
    if (sleep_soon_t > 0) sleep_soon_t -= s;
    if (sd_full_t > 0) sd_full_t -= s;
    if (sd_newpage_t > 0) sd_newpage_t -= s;
    if (sd_newpage_prev_t > 0) sd_newpage_prev_t -= s; // 1.8.26
    if (auto_own_full > 0) auto_own_full -= s;
    if (turn_full_t > 0) turn_full_t -= s;
    if (wh_t0 > 0) wh_t0 -= s; // 1.8.29
    if (sd_open_t > 0) sd_open_t -= s;
    if (t_pchg_prev > 0) t_pchg_prev -= s; // 1.8.5
    for (int c = 0; c < NCELL; c++)
        if (cell_touch[c] > 0) cell_touch[c] -= s;
}
// a second "text" pattern, disjoint from text(): a page turn from one to the other erases every old glyph sample
static void text2(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if ((xx / 2) % 3 == 1 && (yy / 2) % 2 == 1) px_set(xx, yy, 0);
}
static void sd(const char *m);
static void redraw_page(int to2) { // a large content update of the page that is not a page turn (no finalizeNewPage)
    fill(112, 0, 1508, 2088, 255);
    if (to2) text2(112, 0, 1508, 2088); else text(112, 0, 1508, 2088);
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), "
         "(940,2088 680x72)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), (940,2088 680x72)])");
    line(" - merged UI/Content");
}
static void page_turn(int to2) {
    sd("finalizeNewPage:");
    fill(112, 0, 1508, 2088, 255);
    if (to2) text2(112, 0, 1508, 2088); else text(112, 0, 1508, 2088);
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), "
         "(940,2088 680x72)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), (940,2088 680x72)])");
    line(" - merged UI/Content");
}

static void sd(const char *m) { ggq("xofm.modules.devicesceneview.screendriver", m); }
static void pw(const char *m) { ggq("rm.batterymanager", m); }
static void elapse(double s);
// a swipe or pinch: gesture mode, mono frames, the hold time, then the ScreenDriver measures and skips its clean
static void gesture_skip(const char *dirty) {
    char b[80];
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    snprintf(b, sizeof b, " - clean the screen, dirty=%s", dirty);
    sd(b);
    sd(" - doing nothing...");
}
static unsigned long cleans(void) { return n_rclean_ok; }
static void flush(void) { pthread_mutex_lock(&mu); finalize_pending(); pthread_mutex_unlock(&mu); }
static void dbg(const char *w) {
    double t = now_s();
    printf("     [%s] req=%d kind=%d tr=%d g=%d m=%d tiles=%d vis=%d quiet=%.2f stock=%.2f our=%.2f age=%.2f sleeping=%d\n", w,
           auto_req, auto_kind, tr_active, sd_gesture, sd_mono, sd_tiles, sd_visible, t - t_last_update,
           t_stock_full > 0 ? t - t_stock_full : -1, t_our_full > 0 ? t - t_our_full : -1, t - auto_t, sleeping);
}
// the stock keyboard frame of the "New notebook" dialog (measured 2026-11-09), drawn with class cls
static void kb_frame(const char *cls) {
    char b[600];
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - ui ........: QRegion(size=5, bounds=(0,0 1620x1632) - [(0,0 1620x230), (0,230 52x68), (1568,230 52x68), "
         "(0,298 1620x1246), (1448,1556 112x76)])");
    snprintf(b, sizeof b, " - %s: QRegion(size=4, bounds=(0,1544 1620x616) - [(0,1544 1620x12), (0,1556 1448x76), "
             "(1560,1556 60x76), (0,1632 1620x528)])", cls);
    line(b);
}
// the keyboard redrawn while up (the shift state after the first key), as logged 2026-11-09 09:23:32.011 UTC: the
// band again, but only ~350 px of ui (an opening frame draws the dialog behind it, ~2.4 Mpx of ui)
static void kb_redraw(const char *cls) {
    char b[600];
    line("swapBuffers: QFlags() QRegion(size=6, bounds=(0,254 1620x1906) - [(51,254 188x36), (51,290 186x6), "
         "(0,1544 1620x12), (0,1556 1450x76), (1558,1556 62x76), (0,1632 1620x528)]) EPFramebuffer::PresentationMode::Default");
    line(" - animation .: QRegion(size=2, bounds=(52,254 187x42) - [(52,254 187x36), (52,290 185x6)])");
    line(" - ui ........: QRegion(size=3, bounds=(51,254 1509x1378) - [(51,254 1x42), (1448,1556 2x76), (1558,1556 2x76)])");
    snprintf(b, sizeof b, " - %s: QRegion(size=4, bounds=(0,1544 1620x616) - [(0,1544 1620x12), (0,1556 1448x76), "
             "(1560,1556 60x76), (0,1632 1620x528)])", cls);
    line(b);
}
// the sync indicator in the status bar, as logged on the tablet (2026-11-09 03:19:10 UTC, after a document close)
static void status_frame(void) {
    line("swapBuffers: QFlags() QRegion(size=18, bounds=(1394,44 178x32) - [(1394,44 32x5), (1434,44 32x5), (1474,44 32x5), "
         "(1394,49 32x7), (1434,49 32x7), (1474,49 32x7), (1522,49 48x7)]) EPFramebuffer::PresentationMode::Default");
    line(" - ui ........: QRegion(size=18, bounds=(1394,44 178x32) - [(1394,44 32x5), (1434,44 32x5), (1474,44 32x5), "
         "(1394,49 32x7), (1434,49 32x7), (1474,49 32x7), (1522,49 48x7)])");
}
static void key_press(const char *cls) {
    char b[200];
    line("swapBuffers: QFlags() QRegion(668,1641 139x118) EPFramebuffer::PresentationMode::Default");
    snprintf(b, sizeof b, " - %s", cls);
    line(b);
    line("swapBuffers: QFlags() QRegion(51,260 39x30) EPFramebuffer::PresentationMode::Default");
    line(" - animation .: QRegion(52,260 38x30)");
    line(" - ui ........: QRegion(51,260 1x30)");
}

static char *slurp(const char *path) {
    static char buf[4096];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

// 1.8.18: the co-drive's arithmetic (cd_quant, cd_cell, cds_*): synthetic cases, random inputs, and the hold cell
// patch that was measured on the tablet (testdata/)
static int tcp_sum(const int8_t *q, int n) {
    int s = 0;
    for (int f = 0; f < n; f++) s += cd_volt[q[f] & 7];
    return s;
}
// 1.8.21 (review): cd_quant as 1.8.20 had it, word for word: what the wrapper over cd_quant_m must still give
static int cd_quant_ref(const double *c, int nfr, int first, int maxv, double *accp, int8_t *q) {
    static const int lv[3] = {6, 12, 24};
    double acc = *accp, lim = 2.0 * maxv + 6.0;
    int nq = 0;
    if (!isfinite(acc)) acc = 0.0;
    for (int f = 0; f < nfr; f++) {
        acc += c[f];
        if (!isfinite(acc)) acc = 0.0;
        if (acc > lim) acc = lim; else if (acc < -lim) acc = -lim;
        q[f] = 0;
        if (f < first) continue;
        double a = fabs(acc);
        int v = 0, neg = acc < 0;
        for (int i = 0; i < 3; i++)
            if (lv[i] <= maxv && fabs(a - lv[i]) < fabs(a - v)) v = lv[i];
        if (!v) continue;
        q[f] = (int8_t)(neg ? (v == 6 ? 3 : v == 12 ? 2 : 1) : (v == 6 ? 4 : v == 12 ? 5 : 6));
        acc += neg ? v : -v;
        nq += (neg ? -v : v) / 6;
    }
    *accp = acc;
    return nq;
}
static void test_cd_pure(void) {
    printf("T60 1.8.18 co-drive arithmetic\n");
    double c[CD_MAXF], m[CD_MAXF], acc, sc = 0.0;
    int8_t q[CD_MAXF], q2[CD_MAXF];
    // the white phase of an STD turn: what goes in comes out, but for the carry
    for (int f = 0; f < 86; f++) { c[f] = f >= 38 && f < 60 ? -9.6 : 0.0; sc += c[f]; }
    acc = 0.0;
    int nq = cd_quant(c, 86, 0, 12, &acc, q);
    CHECK(nq * 6 == tcp_sum(q, 86) && fabs(sc - tcp_sum(q, 86) - acc) < 1e-9 && fabs(acc) <= 3.0 + 1e-9,
          "cd_quant keeps the dose: in %.1f, out %d, carry %.2f", sc, tcp_sum(q, 86), acc);
    int outside = 0;
    for (int f = 0; f < 86; f++)
        if (q[f] && (f < 38 || f > 60)) outside++;
    CHECK(!outside && q[38] == 2, "its codes stay in the phase (first: code %d)", q[38]);
    // frames before `first` get nothing, their dose comes after them (as far as the clamp lets it)
    for (int f = 0; f < 86; f++) c[f] = 3.0;
    acc = 0.0;
    cd_quant(c, 86, 10, 12, &acc, q);
    int early = 0;
    for (int f = 0; f < 10; f++) early |= q[f];
    CHECK(!early && q[10] == 5 && q[11] == 5, "no code before the first frame, the dose follows (%d %d %d)", q[10], q[11], q[12]);
    acc = 0.0;
    cd_quant(c, 86, 0, 6, &acc, q);
    int big = 0;
    for (int f = 0; f < 86; f++) big |= q[f] != 0 && q[f] != 4;
    CHECK(!big, "cd_max 6: only +6 V codes");
    for (int f = 0; f < 86; f++) c[f] = 1e12;
    c[40] = NAN; c[41] = -INFINITY;
    acc = NAN;
    cd_quant(c, 86, 0, 24, &acc, q);
    int bad = 0;
    for (int f = 0; f < 86; f++) bad |= q[f] < 0 || q[f] > 6;
    CHECK(!bad && isfinite(acc) && fabs(acc) <= 54.0 && q[0] == 6, "huge, NaN and infinite drives stay inside the levels (carry %.1f)", acc);
    // 1.8.21: the shape keys. Defaults: every group of cells gets the drive itself, bit for bit
    {
        double cs[CD_MAXF], o[CD_MAXF], a1, a2;
        int8_t qa[CD_MAXF], qb[CD_MAXF];
        Config d21 = cfg;
        for (int f = 0; f < 86; f++) cs[f] = f < 26 ? 2.1 : f < 46 ? ((f & 1) ? 6.3 : -8.7) * 0.9 : f < 60 ? -8.9 : f < 68 ? -3.7 : 0.0;
        int same = 1;
        for (int g = 0; g < 4; g++) { cds_shape(cs, 86, g, o); same &= !memcmp(o, cs, 86 * sizeof cs[0]); }
        CHECK(same && cds_lvm() == 3 && cfg.cds_carry == 1, "1.8.21 defaults: the shape is the drive itself (levels %d, carry %d)", cds_lvm(), cfg.cds_carry);
        a1 = a2 = 1.5;
        int n1 = cd_quant(cs, 78, 14, 12, &a1, qa), n2 = cd_quant_m(cs, 78, 14, cds_lvm(), cfg.cds_carry, &a2, qb);
        CHECK(n1 == n2 && a1 == a2 && !memcmp(qa, qb, 78), "... and its codes are cd_quant's (%d frames)", n1);
        // groups
        set_param("cds_k27", "0"); set_param("cds_k29", "0.5"); set_param("cds_k28", "2");
        int gbad2 = 0;
        for (int g = 0; g < 4; g++) {
            cds_shape(cs, 86, g, o);
            double k = (g & 1 ? 0.5 : 0.0) * (g & 2 ? 2.0 : 1.0);
            for (int f = 0; f < 86; f++) gbad2 += o[f] != cs[f] * k;
        }
        CHECK(!gbad2, "cds_k27 / cds_k29 / cds_k28 scale their own groups of cells (%d bad)", gbad2);
        cfg = d21;
        // the window
        set_param("cds_w0", "46"); set_param("cds_w1", "60"); set_param("cds_wk", "0.5"); set_param("cds_wadd", "-1");
        cds_shape(cs, 86, 0, o);
        int wbad = 0;
        for (int f = 0; f < 86; f++) wbad += o[f] != (f >= 46 && f < 60 ? cs[f] * 0.5 - 1.0 : cs[f]);
        CHECK(!wbad, "cds_w0..cds_w1: factor and offset inside the window only (%d bad)", wbad);
        cfg = d21;
        // levels: 24 V only keeps the dose with codes 1 and 6 only
        a1 = 0.0;
        n1 = cd_quant_m(cs, 86, 0, 4, 1, &a1, qa);
        int lbad2 = 0;
        double s21 = 0.0;
        for (int f = 0; f < 86; f++) { s21 += cs[f]; lbad2 += qa[f] != 0 && qa[f] != 1 && qa[f] != 6; }
        CHECK(!lbad2 && n1 * 6 == tcp_sum(qa, 86) && fabs(s21 - tcp_sum(qa, 86) - a1) < 1e-9 && fabs(a1) <= 12.0 + 1e-9,
              "levels 24 V only: codes of 24 V, the dose kept (in %.1f, out %d, carry %.2f)", s21, tcp_sum(qa, 86), a1);
        a1 = 0.0;
        cd_quant_m(cs, 86, 0, 2, 1, &a1, qa);
        lbad2 = 0;
        for (int f = 0; f < 86; f++) lbad2 += qa[f] != 0 && qa[f] != 2 && qa[f] != 5;
        CHECK(!lbad2 && fabs(s21 - tcp_sum(qa, 86) - a1) < 1e-9, "levels 12 V only: codes of 12 V, the dose kept");
        set_param("cds_lv", "4");
        CHECK(cds_lvm() == 4, "cds_lv is what cds_lvm gives (%d)", cds_lvm());
        cfg = d21;
        // carry 0: the frames before the first one count for nothing
        for (int f = 0; f < 86; f++) o[f] = f < 14 ? 0.0 : cs[f];
        a1 = a2 = -2.0;
        n1 = cd_quant_m(cs, 78, 14, 3, 0, &a1, qa);
        n2 = cd_quant_m(o, 78, 14, 3, 1, &a2, qb);
        CHECK(n1 == n2 && a1 == a2 && !memcmp(qa, qb, 78) && !qa[13], "cds_carry 0: the early frames' dose is dropped (%d frames, carry %.2f)", n1, a1);
        a2 = -2.0;
        cd_quant_m(cs, 78, 14, 3, 1, &a2, qb);
        CHECK(memcmp(qa, qb, 78) && qb[14] == 5, "... and with carry 1 it comes after them (code %d at the first frame)", qb[14]);
        // clamps
        set_param("cds_k27", "9"); set_param("cds_k29", "-1"); set_param("cds_k28", "4"); set_param("cds_lv", "99"); set_param("cds_carry", "5");
        set_param("cds_w0", "-3"); set_param("cds_w1", "999"); set_param("cds_wk", "-9"); set_param("cds_wadd", "50");
        CHECK(cfg.cds_k27 == 3.0 && cfg.cds_k29 == 0.0 && cfg.cds_k28 == 3.0 && cfg.cds_lv == 7 && cfg.cds_carry == 1 && cfg.cds_w0 == 0 &&
              cfg.cds_w1 == 320 && cfg.cds_wk == -3.0 && cfg.cds_wadd == 6.0, "the shape keys are clamped");
        cfg = d21;
    }
    // cd_cell: only frames the table leaves at 0 V, only frames the table has
    int8_t q5[5] = {4, 4, 4, 4, 4};
    CHECK(cd_cell(0x00d2, q5, 0, 86) == 0x48d2 && cd_cell(0x00d2, q5, 0, 4) == 0x08d2 && cd_cell(0x0000, q5, 0, 3) == 0x0124,
          "cd_cell keeps the table's own frames (%04x %04x %04x)", cd_cell(0x00d2, q5, 0, 86), cd_cell(0x00d2, q5, 0, 4), cd_cell(0x0000, q5, 0, 3));
    // a table of 10 frames: 0->30 at -24 V throughout, 30->30 (never taken by a held pixel) at +24 V
    static uint16_t tab[CD_NG * 1025];
    memset(tab, 0, sizeof tab);
    tab[0 * 1025 + ((0 << 5) | 30)] = tab[1 * 1025 + ((0 << 5) | 30)] = 0x1249;
    tab[0 * 1025 + ((30 << 5) | 30)] = tab[1 * 1025 + ((30 << 5) | 30)] = 0x6db6;
    CHECK(cds_rowsum(tab, 10, 30) == -240 && cds_rowabs(tab, 10, 30) == 240 && cds_rowsum(tab, 7, 30) == -168 &&
          cds_rowabs(tab, 10, (28 << 5) | 27) == 0 && cds_rowsum(tab, 10, (30 << 5) | 30) == 240, "cds_rowsum / cds_rowabs");
    uint8_t from[100], to[100];
    uint32_t hist[1024];
    for (int i = 0; i < 100; i++) { from[i] = i < 25 ? 0 : 30; to[i] = 30; }
    double chg = -1.0;
    CHECK(cds_histo(from, to, 100, hist) == 25 && hist[30] == 25 && hist[(30 << 5) | 30] == 75, "cds_histo counts the transitions");
    cds_mean(hist, 100.0, tab, 10, m, &chg);
    CHECK(fabs(m[0] + 6.0) < 1e-12 && fabs(m[9] + 6.0) < 1e-12 && fabs(chg - 0.25) < 1e-12, "cds_mean: a quarter at -24 V is -6 V, held pixels count as 0 V (%.2f, changed %.2f)", m[0], chg);
    cds_mean(hist, 0.0, tab, 10, m, &chg);
    CHECK(m[0] == 0.0 && chg == 0.0, "cds_mean of no samples is 0");
    m[0] = -10.0; m[1] = 10.0; m[2] = 0.0;
    double peak = cds_codrive(m, 3, 1.0, 0.4, c);
    CHECK(fabs(c[0] + 10.0) < 1e-12 && fabs(c[1] - 4.0) < 1e-12 && c[2] == 0.0 && fabs(peak - 10.0) < 1e-12, "cds_codrive: its own gain each way");
    // the sample grid stays on the screen, every row and every column of the 8 px raster is visited
    int gbad = 0, colseen = 0, rowseen = 0;
    for (int j = 0; j < CDS_NY; j++) {
        if (cds_y(j) < 0 || cds_y(j) >= SH || (j && cds_y(j) <= cds_y(j - 1))) gbad++;
        rowseen |= 1 << (cds_y(j) & 7);
        for (int i = 0; i < CDS_NX; i++) {
            if (cds_x(i, j) < 0 || cds_x(i, j) >= SW) gbad++;
            colseen |= 1 << (cds_x(i, j) & 7);
        }
    }
    uint32_t row[1632];
    uint8_t lv[CDS_NX];
    for (int x = 0; x < 1632; x++) row[x] = 0xabcd00u | (uint32_t)(x % 29);
    cds_row(row, 3, lv);
    CHECK(!gbad && colseen == 255 && rowseen == 255 && CDS_N == 54540 && lv[0] == cds_x(0, 3) % 29 && lv[CDS_NX - 1] == cds_x(CDS_NX - 1, 3) % 29,
          "the sample grid: %d samples on the screen, all phases of the 8 px raster", CDS_N);
    // random drives: the codes stay legal, nothing before `first`, the dose adds up; cells keep the table's frames
    uint32_t rs = 1818;
    int rbad = 0;
#define TCP_R() (rs = rs * 1664525u + 1013904223u, (rs >> 8) & 0xffff)
    for (int it = 0; it < 3000; it++) {
        int nfr = 8 + (int)(TCP_R() % 312), first = (int)(TCP_R() % (unsigned)(nfr + 2)), maxv = 6 << (TCP_R() % 3);
        double amp = (it & 1) ? 12.0 : 60.0, s = 0.0, lim = 2.0 * maxv + 6.0, a0 = (TCP_R() / 65535.0 - 0.5) * 6.0;
        for (int f = 0; f < nfr; f++) c[f] = (TCP_R() / 65535.0 - 0.5) * 2.0 * amp;
        acc = a0;
        int n = cd_quant(c, nfr, first, maxv, &acc, q);
        {
            double am = a0;
            int nm = cd_quant_ref(c, nfr, first, maxv, &am, q2); // 1.8.20's own arithmetic
            if (nm != n || am != acc || memcmp(q, q2, (size_t)nfr)) rbad++;
            if (it % 7 == 0) { // and with drives that are not finite
                double cb[CD_MAXF], a1 = NAN, a2 = NAN;
                int8_t qx[CD_MAXF], qy[CD_MAXF];
                memcpy(cb, c, sizeof(double) * (size_t)nfr);
                cb[TCP_R() % (unsigned)nfr] = NAN; cb[TCP_R() % (unsigned)nfr] = INFINITY; cb[TCP_R() % (unsigned)nfr] = -1e300;
                int n1 = cd_quant(cb, nfr, first, maxv, &a1, qx), n2 = cd_quant_ref(cb, nfr, first, maxv, &a2, qy);
                if (n1 != n2 || a1 != a2 || memcmp(qx, qy, (size_t)nfr)) rbad++;
            }
            am = a0;
            nm = cd_quant_m(c, nfr, first, maxv == 24 ? 7 : maxv == 12 ? 3 : 1, 1, &am, q2);
            if (nm != n || am != acc || memcmp(q, q2, (size_t)nfr)) rbad++;
            // any mask: legal codes of the mask's levels only, nothing before `first`
            int lvm = (int)(TCP_R() % 8), cy = (int)(TCP_R() & 1);
            am = a0;
            nm = cd_quant_m(c, nfr, first, lvm, cy, &am, q2);
            for (int f = 0; f < nfr; f++) {
                int av = abs(cd_volt[q2[f] & 7]);
                if (q2[f] < 0 || q2[f] > 6 || (f < first && q2[f]) || (av && !((lvm >> (av == 6 ? 0 : av == 12 ? 1 : 2)) & 1))) rbad++;
            }
            if (nm * 6 != tcp_sum(q2, nfr) || !isfinite(am) || fabs(am) > 54.0 + 1e-9) rbad++;
        }
        for (int f = 0; f < nfr; f++) {
            s += c[f];
            if (q[f] < 0 || q[f] > 6 || (f < first && q[f]) || abs(cd_volt[q[f]]) > maxv) rbad++;
        }
        if (n * 6 != tcp_sum(q, nfr) || fabs(acc) > lim + 1e-9) rbad++;
        if (amp < 20.0 && first == 0 && maxv == 24 && fabs(a0 + s - tcp_sum(q, nfr) - acc) > 1e-6) rbad++; // nothing clamped
        int g = (int)(TCP_R() % (unsigned)((nfr + 4) / 5));
        uint16_t old = (uint16_t)TCP_R(), val = cd_cell(old, q, g, nfr);
        for (int k = 0; k < 5; k++) {
            int o = (old >> (3 * k)) & 7, v = (val >> (3 * k)) & 7, f = g * 5 + k;
            if (v != (o || f >= nfr ? o : q[f])) rbad++;
        }
        if ((val ^ old) & 0x8000) rbad++;
    }
#undef TCP_R
    CHECK(!rbad, "3000 random drives and cells: legal codes, none before the first frame, the dose adds up (%d bad)", rbad);
#ifdef TESTDATA
    // the patch that was measured on the tablet (10.5 % -> 0.1 %): the same cells from the same data
    static uint16_t cells[CD_NG * 1025];
    memset(cells, 0, sizeof cells);
    FILE *f = fopen(TESTDATA "/std.cells", "rb");
    size_t got = f ? fread(cells, 2, 18 * 1025, f) : 0;
    if (f) fclose(f);
    CHECK(got == 18 * 1025, "testdata: the STD table (%zu cells)", got);
    CHECK(cds_rowsum(cells, 86, (0 << 5) | 30) < -100 && cds_rowsum(cells, 86, (30 << 5) | 0) > 100 && !cds_rowabs(cells, 86, (28 << 5) | 27) &&
          !cds_rowabs(cells, 86, (29 << 5) | 27) && !cds_rowabs(cells, 86, (30 << 5) | 27) && !cds_rowabs(cells, 86, (31 << 5) | 27),
          "the STD table passes cds_arm's checks (0->30 %d, 30->0 %d)", cds_rowsum(cells, 86, 30), cds_rowsum(cells, 86, 30 << 5));
    {
        // 1.8.25: the two whites on the measured table
        static uint16_t fc[CD_NG * 1025], fz[CD_NG * 1025];
        static uint32_t fh[1024];
        double fm0[CD_MAXF], fm1[CD_MAXF], ch0, ch1, s0 = 0, s1 = 0;
        int k0 = cdf_kind(cells, 86), q0 = cdf_quiet(cells, 86), nz = 0, other = 0, top = 0;
        memcpy(fz, cells, sizeof fz);
        for (int g = 0; g < 18; g++) {
            fz[g * 1025 + CDF_A] = cdf_cell(cells, 86, g, CDF_A, 1);
            fz[g * 1025 + CDF_B] = cdf_cell(cells, 86, g, CDF_B, 1);
            nz += fz[g * 1025 + CDF_A] != cells[g * 1025 + CDF_A];
            nz += fz[g * 1025 + CDF_B] != cells[g * 1025 + CDF_B];
            top += (fz[g * 1025 + CDF_A] ^ cells[g * 1025 + CDF_A]) & 0x8000;
        }
        for (int i = 0; i < 18 * 1025; i++) other += fz[i] != cells[i] && i % 1025 != CDF_A && i % 1025 != CDF_B;
        // the last group holds frame 85 only: the bits of frames 86..89 stay
        uint16_t lastg = cdf_cell(cells, 86, 17, CDF_A, 1);
        CHECK(k0 == 1 && q0 >= 60 && q0 <= 77 && cdf_kind(fz, 86) == 2 && cdf_quiet(fz, 86) == q0 && !other && !top && nz >= 20 &&
              !cds_rowabs(fz, 86, CDF_A) && !cds_rowabs(fz, 86, CDF_B) && (lastg & 0x7ff8) == (cells[17 * 1025 + CDF_A] & 0x7ff8),
              "cdf_kind: the STD table's flip rows are the refresh rows (kind %d, idle from frame %d); written to 0 V: kind %d, %d cells, no other cell (%d)",
              k0, q0, cdf_kind(fz, 86), nz, other);
        memcpy(fc, fz, sizeof fc);
        for (int g = 0; g < 18; g++) {
            fc[g * 1025 + CDF_A] = cdf_cell(fz, 86, g, CDF_A, 0);
            fc[g * 1025 + CDF_B] = cdf_cell(fz, 86, g, CDF_B, 0);
        }
        CHECK(!memcmp(fc, cells, sizeof fc), "cdf_cell: put back from the refresh rows, the table is the stock one again");
        // not the table we know: every one of the signs
        int kbad = 0;
        memcpy(fc, cells, sizeof fc); fc[3 * 1025 + ((28 << 5) | 28)] ^= 1; kbad += cdf_kind(fc, 86) != 0;       // refresh rows differ
        memcpy(fc, cells, sizeof fc); fc[3 * 1025 + ((28 << 5) | 12)] ^= 1; kbad += cdf_kind(fc, 86) != 0;       // from 28 != from 30
        memcpy(fc, cells, sizeof fc); fc[3 * 1025 + ((30 << 5) | 29)] ^= 8; kbad += cdf_kind(fc, 86) != 0;       // a hold column differs
        memcpy(fc, cells, sizeof fc); fc[2 * 1025 + ((28 << 5) | 27)] |= 2; fc[2 * 1025 + ((30 << 5) | 27)] |= 2; kbad += cdf_kind(fc, 86) != 0; // hold cells not free
        memcpy(fc, cells, sizeof fc); fc[5 * 1025 + CDF_A] ^= 1; kbad += cdf_kind(fc, 86) != 0;                  // a flip row of its own
        memcpy(fc, fz, sizeof fc); fc[5 * 1025 + CDF_B] |= 7; kbad += cdf_kind(fc, 86) != 0;                     // code 7 is not free
        memcpy(fc, fz, sizeof fc); fc[9 * 1025 + CDF_A] |= 3 << 6; kbad += cdf_kind(fc, 86) != 0;                // one flip row half written
        memset(fc, 0, sizeof fc); kbad += cdf_kind(fc, 86) != 0;                                                 // nothing driven at all
        kbad += cdf_kind(cells, 7) != 0; kbad += cdf_kind(cells, CD_MAXF) != 0;
        // the rows to 31 may differ (they do in the stock table) and the rows to 28 / 30 from other levels too
        memcpy(fc, cells, sizeof fc); fc[4 * 1025 + ((28 << 5) | 31)] ^= 1; fc[4 * 1025 + ((13 << 5) | 28)] ^= 1;
        CHECK(!kbad && cdf_kind(fc, 86) == 1, "cdf_kind: refresh rows that differ, a row from 28 unlike that from 30, a hold column, hold cells in use, a flip row of its own, code 7, a half written row, an idle table, a bad length: none is taken (%d); rows to 31 and into the whites may differ", kbad);
        // only the measured table: the white tail's signature, with stock and with 0 V flip rows; not a table of
        // another length or one whose ink -> white rows end otherwise
        memcpy(fc, cells, sizeof fc);
        int m0 = cdf_meas(fc, 86), same0 = !memcmp(fc, cells, sizeof fc);
        memcpy(fc, fz, sizeof fc);
        int m1 = cdf_meas(fc, 86), same1 = !memcmp(fc, fz, sizeof fc);
        memcpy(fc, cells, sizeof fc);
        for (int g = 14; g < 18; g++) fc[g * 1025 + ((5 << 5) | 30)] = 0; // one ink -> white row idles from frame 70
        int m2 = cdf_meas(fc, 86), m3 = cdf_meas(fc, 85);
        memcpy(fc, cells, sizeof fc);
        CHECK(m0 == 1 && same0 && m1 == 1 && same1 && !m3 && !cdf_meas(fc, 91), "cdf_meas: the STD table of 22-28 C is the measured one, with stock and with 0 V flip rows, and is not changed; not with another length (one row changed: %d)", m2);
        { // 1.8.30: the white tail standing in the table (wt_static)
            static uint16_t fw[CD_NG * 1025], ft[CD_NG * 1025];
            uint16_t r2[CDS_WT_ROWS], r3[CDS_WT_ROWS];
            int8_t q7[CD_MAXF];
            int sg0 = wts_sig(cells, 86, r2), ncell = 0, back = 0;
            memcpy(ft, cells, sizeof ft);
            cds_wt_q(86, 77, 7, q7);
            int nm = cds_wt_mark(ft, 86, q7, r3); // the per-turn tail of 1.8.23, cds_wt 7
            memcpy(fw, cells, sizeof fw);
            for (int i = 0; i < CDS_WT_NR && sg0 == 0; i++)
                for (int g = 13; g <= 15; g++) {
                    uint16_t o = fw[g * 1025 + r2[i]], v = wts_cell(o, g, 1);
                    ncell += v != o; back += wts_cell(v, g, 0) != o;
                    fw[g * 1025 + r2[i]] = v;
                }
            CHECK(sg0 == 0 && nm == 43 && !memcmp(r2, r3, 43 * sizeof r2[0]) && ncell == 129 && !back && !memcmp(fw, ft, sizeof fw),
                  "wts: the stock table is state 0; the standing tail is the per-turn tail of cds_wt 7 cell for cell (129 cells, %d), and comes out again (%d %d %d)", ncell, sg0, nm, back);
            CHECK(fw[13 * 1025 + ((2 << 5) | 28)] == 0x24d3 && cells[13 * 1025 + ((2 << 5) | 28)] == 0x00d3 && wts_cell(0, 12, 1) == 0 && wts_cell(0x7fff, 16, 0) == 0x7fff,
                  "wts_cell: the cell of the hand trial (13417: 00d3 -> 24d3); groups without a tail frame are not touched");
            memcpy(ft, fw, sizeof ft);
            int sg1 = wts_sig(fw, 86, r3), m1 = cdf_meas(ft, 86), same = !memcmp(ft, fw, sizeof ft), again = cds_wt_mark(ft, 86, q7, r3);
            CHECK(sg1 == 43 && m1 == 1 && same && again == 0 && cdf_kind(fw, 86) == 1,
                  "wts: the table with the tail is state 43 and still the measured one for cdf_meas (unchanged); no per-turn tail goes on top (%d %d %d)", sg1, m1, again);
            memcpy(ft, fw, sizeof ft); ft[14 * 1025 + ((8 << 5) | 30)] = cells[14 * 1025 + ((8 << 5) | 30)]; // one row lost a part of its tail
            int mx1 = wts_sig(ft, 86, r3);
            memcpy(ft, fw, sizeof ft); for (int g = 13; g <= 15; g++) ft[g * 1025 + ((8 << 5) | 30)] = cells[g * 1025 + ((8 << 5) | 30)]; // one row stock
            int mx2 = wts_sig(ft, 86, r3);
            memcpy(ft, fw, sizeof ft); ft[14 * 1025 + ((8 << 5) | 30)] ^= 3; // frame 70 at -24 V instead of -12 V
            int mx3 = wts_sig(ft, 86, r3);
            memcpy(ft, cells, sizeof ft); for (int g = 14; g < 18; g++) ft[g * 1025 + ((5 << 5) | 30)] = 0;
            int mx4 = wts_sig(ft, 86, r3);
            CHECK(mx1 == -1 && mx2 == -1 && mx3 == -1 && mx4 == m2 - 1 + 0 * mx4 && wts_sig(fw, 85, r3) == -1 && wts_sig(cells, 91, r3) == -1,
                  "wts_sig: a row with a part of the tail, one row without it, another code in a tail frame, another length: not our table (%d %d %d %d)", mx1, mx2, mx3, mx4);
        }
        // a turn of which a quarter flips between the whites: the mean drive without the flips' row
        memset(fh, 0, sizeof fh);
        fh[(30 << 5) | 30] = 500; fh[CDF_A] = 120; fh[CDF_B] = 140; fh[(30 << 5) | 0] = 60; fh[(0 << 5) | 30] = 70; fh[(12 << 5) | 28] = 50; fh[(28 << 5) | 12] = 60;
        cds_mean(fh, 1000.0, cells, 86, fm0, &ch0);
        cds_mean(fh, 1000.0, fz, 86, fm1, &ch1);
        for (int fr = 0; fr < 86; fr++) { s0 += fabs(fm0[fr]); s1 += fabs(fm1[fr]); }
        CHECK(s1 < 0.6 * s0 && s1 > 0 && ch0 == ch1, "cds_mean: with the flip rows at 0 V the mean drive of a turn with 26 %% flips falls (%.0f -> %.0f V x frames)", s0, s1);
    }
    {
        // 1.8.23: the white tail on the measured table
        static uint16_t wc[CD_NG * 1025];
        static int8_t wq[CD_MAXF];
        uint16_t rows[CDS_WT_ROWS];
        int wl = cds_wt_q(86, 77, 7, wq), nw = 0;
        for (int fr = 0; fr < 86; fr++) nw += wq[fr] == 2;
        CHECK(wl == 76 && nw == 7 && wq[68] == 2 && wq[69] == 2 && wq[70] == 2 && wq[71] == 2 && !wq[72] && !wq[73] && wq[74] == 2 &&
              wq[75] == 2 && wq[76] == 2 && !wq[77] && !wq[67], "cds_wt_q: 7 frames are 68-71 and 74-76 (last %d, %d codes)", wl, nw);
        CHECK(cds_wt_q(86, 77, 3, wq) == 70 && cds_wt_q(86, 70, 7, wq) == 69 && cds_wt_q(86, 86, 99, wq) == 76,
              "cds_wt_q: fewer frames, an earlier end, no end (never past frame 76)");
        int w72 = cds_wt_q(86, 72, 7, wq), n72 = 0, w73, n73 = 0;
        for (int fr = 0; fr < 86; fr++) n72 += wq[fr] == 2;
        w73 = cds_wt_q(86, 73, 7, wq);
        for (int fr = 0; fr < 86; fr++) n73 += wq[fr] == 2;
        CHECK(w72 == 71 && n72 == 4 && w73 == 71 && n73 == 4 && !wq[72] && !wq[73], "cds_wt_q: an end at 72 / 73 stops before the table's own two frames (%d %d, %d %d)", w72, n72, w73, n73);
        int z = 0;
        z += cds_wt_q(85, 77, 7, wq) != -1; for (int fr = 0; fr < 85; fr++) z += wq[fr] != 0;
        z += cds_wt_q(86, 77, 0, wq) != -1; for (int fr = 0; fr < 86; fr++) z += wq[fr] != 0;
        z += cds_wt_q(86, 68, 7, wq) != -1; for (int fr = 0; fr < 86; fr++) z += wq[fr] != 0;
        z += cds_wt_q(87, 77, 7, wq) != -1; for (int fr = 0; fr < 87; fr++) z += wq[fr] != 0;
        CHECK(!z, "cds_wt_q: another table length, 0 frames, an end before the idle run: no code (%d)", z);
        memcpy(wc, cells, sizeof wc);
        cds_wt_q(86, 77, 7, wq);
        int nr = cds_wt_mark(wc, 86, wq, rows), wbad = 0, ndiff = 0, known = 0;
        static const int meas[9] = {2, 4, 8, 12, 15, 16, 19, 20, 24}; // the sources of the measured patch
        for (int i = 0; i < nr; i++) {
            int idx = rows[i], from = idx >> 5, to = idx & 31;
            if (from >= 27 || (to != 28 && to != 30) || cds_rowsum(wc, 86, idx) != cds_rowsum(cells, 86, idx) - 84) wbad++;
            for (int fr = 0; fr < 86; fr++) {
                int o = (cells[(fr / 5) * 1025 + idx] >> (3 * (fr % 5))) & 7, v = (wc[(fr / 5) * 1025 + idx] >> (3 * (fr % 5))) & 7;
                if (v != (wq[fr] ? 2 : o) || (wq[fr] && o)) wbad++;
            }
            for (int j = 0; j < 9; j++) known += from == meas[j];
        }
        for (int i = 0; i < 18 * 1025; i++)
            if (wc[i] != cells[i]) {
                int idx = i % 1025, g = i / 1025, in = 0;
                for (int j = 0; j < nr; j++) in |= rows[j] == idx;
                if (!in || g < 13 || g > 15) wbad++;
                ndiff++;
            }
        // the 40 long rows and the three short ones (15, 19, 21 -> 30: the greys at a letter's edge, idle from frame 30 to
        // 76 / 78, then the vendor's own late nudges). 0 -> 30 (black text to white) idles from frame 30 to the end, the
        // rows from 1, 3, 7, 11, 23 are not used at all: they stay as they are
        int blk = 0, shr = 0;
        for (int i = 0; i < nr; i++) {
            blk += rows[i] == 30 || rows[i] == ((1 << 5) | 30) || rows[i] == ((3 << 5) | 30) || rows[i] == ((7 << 5) | 30) ||
                   rows[i] == ((11 << 5) | 30) || rows[i] == ((23 << 5) | 30);
            shr += rows[i] == ((15 << 5) | 30) || rows[i] == ((19 << 5) | 30) || rows[i] == ((21 << 5) | 30);
        }
        CHECK(nr == 43 && known == 18 && shr == 3 && !blk && !wbad && ndiff == nr * 3,
              "cds_wt_mark on the STD table: %d rows (of the 9 measured sources to 28 and 30: %d; short rows %d; black -> white and unused rows: %d), each -84 V x frames, %d cells differ, %d bad",
              nr, known, shr, blk, ndiff, wbad);
        printf("     white tail rows:");
        for (int i = 0; i < nr; i++) printf(" %d>%d", rows[i] >> 5, rows[i] & 31);
        printf("\n");
        // the co-drive follows the tail: the mean drive of the tail's frames grows by 12 V x the part of the pixels in those rows
        memset(hist, 0, sizeof hist);
        hist[(12 << 5) | 30] = 10; hist[(30 << 5) | 30] = 80; hist[(30 << 5) | 12] = 10;
        double m0[CD_MAXF], m1[CD_MAXF], ch0, ch1;
        cds_mean(hist, 100.0, cells, 86, m0, &ch0);
        cds_mean(hist, 100.0, wc, 86, m1, &ch1);
        int mbad = 0;
        for (int fr = 0; fr < 86; fr++) mbad += fabs(m1[fr] - m0[fr] - (wq[fr] ? -1.2 : 0.0)) > 1e-9;
        CHECK(!mbad && ch0 == ch1, "the mean drive with the tail: -1.2 V more in its 7 frames for 10 %% of the pixels (%d frames off)", mbad);
        // rows that do not end like the measured ones stay as they are
#define WSET(idx, fr, c) wc[((fr) / 5) * 1025 + (idx)] = (uint16_t)((wc[((fr) / 5) * 1025 + (idx)] & ~(7 << (3 * ((fr) % 5)))) | ((c) << (3 * ((fr) % 5))))
        static uint16_t wc2[CD_NG * 1025];
        static const int mut[9] = {(12 << 5) | 30, (16 << 5) | 30, (20 << 5) | 28, (24 << 5) | 30, (19 << 5) | 30, (8 << 5) | 30, (4 << 5) | 28, (26 << 5) | 30, (2 << 5) | 30};
        memcpy(wc, cells, sizeof wc);
        WSET((12 << 5) | 30, 74, 2); // a tail frame of a long row is driven (-12 V)
        WSET((16 << 5) | 30, 60, 7); // the 0 V code 7 in the white phase, outside the tail's frames
        WSET((20 << 5) | 28, 55, 6); // +24 V in the white phase
        WSET((24 << 5) | 30, 47, 0); WSET((24 << 5) | 30, 48, 0); WSET((24 << 5) | 30, 49, 0); // 24 -> 30 idles from frame 47 to the end
        for (int g = 10; g < 18; g++) wc[g * 1025 + ((24 << 5) | 30)] &= (uint16_t)~0x7fff;
        WSET((19 << 5) | 30, 76, 3); // frame 76 of the short row 19 -> 30 is driven (-6 V)
        WSET((8 << 5) | 30, 55, 4);  // +6 V in the white phase
        WSET((4 << 5) | 28, 60, 5);  // +12 V in the white phase
        WSET((26 << 5) | 30, 47, 5); // +12 V in the rule's first frame
        WSET((2 << 5) | 30, 70, 7);  // code 7 in a tail frame
        WSET((14 << 5) | 30, 46, 5); // +12 V one frame before the rule's first frame: still a tail row
        int nr2 = 0, hit = 0;
        for (int from = 0; from < 27; from++)
            for (int to = 28; to <= 30; to += 2) {
                int idx = (from << 5) | to;
                if (!cds_wt_row(wc, 86, idx)) continue;
                nr2++;
                for (int j = 0; j < 9; j++) hit += idx == mut[j];
            }
        CHECK(nr2 == nr - 9 && !hit && cds_wt_row(wc, 86, (14 << 5) | 30) && !cds_wt_row(cells, 86, (30 << 5) | 0) && !cds_wt_row(cells, 85, (12 << 5) | 30) &&
              cds_wt_row(cells, 86, (12 << 5) | 30) && cds_wt_row(cells, 86, (15 << 5) | 30) && !cds_wt_row(cells, 86, 30),
              "cds_wt_row: a driven tail frame (long and short row), a code 7 (in and outside the tail), +6 / +12 / +24 V in the white phase (frame 47 too), a row idle to the end, a draw row, another length: no tail (%d rows left, %d wrong)", nr2, hit);
        // the tail only on the measured table: any other set of rows gets none, and the copy stays as it was
        memcpy(wc2, wc, sizeof wc);
        int mk9 = cds_wt_mark(wc, 86, wq, rows), same9 = !memcmp(wc, wc2, sizeof wc);
        memcpy(wc, cells, sizeof wc);
        WSET((20 << 5) | 30, 60, 5); // one long row less: 42 rows
        memcpy(wc2, wc, sizeof wc);
        int mk42 = cds_wt_mark(wc, 86, wq, rows), same42 = !memcmp(wc, wc2, sizeof wc);
        memcpy(wc, cells, sizeof wc);
        WSET((15 << 5) | 30, 60, 5); // 43 rows again, but a short row is not among them (an unused row ends like a long one)
        for (int g = 0; g < 18; g++) wc[g * 1025 + ((1 << 5) | 30)] = cells[g * 1025 + ((2 << 5) | 30)];
        int n43 = 0;
        for (int from = 0; from < 27; from++)
            for (int to = 28; to <= 30; to += 2) n43 += cds_wt_row(wc, 86, (from << 5) | to);
        memcpy(wc2, wc, sizeof wc);
        int mksig = cds_wt_mark(wc, 86, wq, rows), samesig = !memcmp(wc, wc2, sizeof wc);
        memcpy(wc, cells, sizeof wc);
        for (int g = 0; g < 18; g++) wc[g * 1025 + ((1 << 5) | 30)] = cells[g * 1025 + ((2 << 5) | 30)]; // one row more: 44
        memcpy(wc2, wc, sizeof wc);
        int mk44 = cds_wt_mark(wc, 86, wq, rows), same44 = !memcmp(wc, wc2, sizeof wc);
        CHECK(!mk9 && same9 && !mk42 && same42 && n43 == 43 && !mksig && samesig && !mk44 && same44,
              "cds_wt_mark: a table whose rows are not the measured 43 (fewer, more, 43 without a short row: %d) gets no tail and is left as it is (%d %d %d %d / %d %d %d %d)",
              n43, mk9, mk42, mksig, mk44, same9, same42, samesig, same44);
#undef WSET
        set_param("cds_wt", "99"); int w99 = cfg.cds_wt; set_param("cds_wt", "-4"); int wm = cfg.cds_wt; set_param("cds_wt", "7");
        CHECK(w99 == 7 && wm == 0 && cfg.cds_wt == 7, "cds_wt is clamped to 0..7 (%d, %d)", w99, wm);
    }
    double tot = 0.0;
    int k;
    unsigned cnt;
    memset(hist, 0, sizeof hist);
    f = fopen(TESTDATA "/hist_u3b.txt", "r");
    while (f && fscanf(f, "%d %u", &k, &cnt) == 2)
        if (k >= 0 && k < 1024) { hist[k] = cnt; tot += cnt; }
    if (f) fclose(f);
    cds_mean(hist, tot, cells, 86, m, &chg);
    peak = cds_codrive(m, 86, 1.0, 0.4, c);
    acc = 0.0;
    cd_quant(c, 86, 0, 24, &acc, q);
    acc = 0.0;
    cd_quant(c, 86, 0, 12, &acc, q2);
    CHECK(tot == 814320.0 && chg > 0.3 && chg < 0.7 && m[45] < -7.0 && m[10] > 5.0, "the measured turn: %.3f of the page changes, mean drive %+.1f V at frame 10, %+.1f V at frame 45, co-drive peak %.1f V", chg, m[10], m[45], peak);
    CHECK(!memcmp(q, q2, 86), "cd_max 12 (the default) gives the codes of cd_max 24 (the measured patch)");
    int lines = 0, lbad = 0, more = 0;
    f = fopen(TESTDATA "/m_1_0_4.txt", "r");
    for (int g = 0; f && g * 5 < 86; g++)
        for (int from = 28; from < 32; from++)
            for (int ti = 0; ti < 2; ti++) {
                int idx = (from << 5) | (ti ? 29 : 27), i = -1;
                unsigned o = 0, v = 0;
                uint16_t old = cells[g * 1025 + idx], val = cd_cell(old, q2, g, 86);
                if (val == old) continue;
                if (fscanf(f, "%d %x %x", &i, &o, &v) != 3 || i != g * 1025 + idx || o != old || v != val) lbad++;
                lines++;
            }
    if (f) { more = fscanf(f, "%d", &k) == 1; fclose(f); }
    CHECK(f && lines == 120 && !lbad && !more, "the measured hold cell patch comes out cell for cell (%d cells, %d differ)", lines, lbad);
    // the same turn as the library samples it: the whole screen's mean with the default gains is that patch's drive
    tot = 0.0;
    memset(hist, 0, sizeof hist);
    f = fopen(TESTDATA "/grid_u3b.txt", "r");
    while (f && fscanf(f, "%d %u", &k, &cnt) == 2)
        if (k >= 0 && k < 1024) { hist[k] = cnt; tot += cnt; }
    if (f) fclose(f);
    Config dflt = cfg;
    CHECK(cfg.cds_gain == 1.07 && cfg.cds_gpos == 0.25, "1.8.20: the STD co-drive's defaults (%.2f, %.2f)", cfg.cds_gain, cfg.cds_gpos);
    set_param("cd_std", "1"); set_param("cds_gain", "1.07"); set_param("cds_gpos", "0.43");
    cds_mean(hist, tot, cells, 86, m, &chg);
    cds_codrive(m, 86, cfg.cds_gain, cfg.cds_gpos, c);
    acc = 0.0;
    cd_quant(c, 86, 0, 12, &acc, q2);
    int ndiff = 0;
    for (int fr = 0; fr < 86; fr++) ndiff += q[fr] != q2[fr];
    CHECK(tot == CDS_N && abs(tcp_sum(q, 86) - tcp_sum(q2, 86)) <= 12 && abs(tcp_sum(q + 38, 22) - tcp_sum(q2 + 38, 22)) <= 6,
          "the library's samples of that turn: dose %+d V (patch %+d), white phase %+d V (patch %+d), %d frames coded differently, changed %.3f",
          tcp_sum(q2, 86), tcp_sum(q, 86), tcp_sum(q2 + 38, 22), tcp_sum(q + 38, 22), ndiff, chg);
    set_param("cds_gain", "9"); set_param("cds_gpos", "-1"); set_param("cds_lead", "99"); set_param("cds_every", "-3"); set_param("cd_std", "7");
    CHECK(cfg.cds_gain == 3.0 && cfg.cds_gpos == 0.0 && cfg.cds_lead == 30 && cfg.cds_every == 0 && cfg.cd_std == 2, "the new keys are clamped");
    cfg = dflt;
#else
    printf("  (no TESTDATA: the measured patch is not checked)\n");
#endif
}

// 1.8.8 co-drive: when the helper is asked, with which parts, and what it does to the page clean
// 1.8.22: the turn_full experiment (a page turn away from a page with colour goes out as the full STD itself)
static void tf_settle(void) { // a settled page, nothing waiting
    elapse(40.0);
    ggq("qml", "GGQ 0 hb");
    full();
    flush();
    elapse(5.0);
    auto_req = 0; tr_active = 0; promote_live = 0; kb_on = 0;
}
static void tf_full_turn(int to2) { // the turn's frame, logged as the complete refresh the byte made of it
    fill(112, 0, 1508, 2088, 255);
    if (to2) text2(112, 0, 1508, 2088); else text(112, 0, 1508, 2088);
    full();
}
static void tf_turn_frame(int to2) { // the turn's frame as a normal update (no finalizeNewPage line)
    fill(112, 0, 1508, 2088, 255);
    if (to2) text2(112, 0, 1508, 2088); else text(112, 0, 1508, 2088);
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), "
         "(940,2088 680x72)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), (940,2088 680x72)])");
    line(" - merged UI/Content");
}
static void test_turn_full(void) {
    printf("T48 1.8.22 turn_full: the turn away from a page with colour is the full STD itself\n");
    Config sv = cfg;
    int sv_xo = xo_state;
    const char *sv_rc = host_rc;
    int sv_sw = host_sw;
    cfg.defer = 1; cfg.tr_quiet = 0.5; cfg.tr_full = 0.0; cfg.gesture_clean = 1;
    cfg.page_gap = 1; cfg.page_px = 80000; cfg.page_every = 6; cfg.page_flip = 2.0; cfg.page_settle = 1.5;
    cfg.turn_px = 6000; cfg.page_quiet = 0.2; cfg.auto_quiet = 0.4; cfg.stock_gap = 3.0;
    cfg.cd_gain = 1.0; cfg.cd_gpos = 0.7; cfg.cd_minv = 0.7; cfg.cd_area = 1000000;
    cfg.page_clean = 1; cfg.codrive = 1; cfg.turn_full = 0;
    xo_state = 0; host_rc = "ok"; host_sw = 1;
    sd(" - reset");
    sd("visibleChanged: true");
    sd("penCloseChanged: false");
    sd_gesture = 0; sd_mono = 0;
    host_cd_verdict = 1;
    host_colour = 300;
    fill(0, 0, SW, SH, 255);
    text(112, 0, 1508, 2088);
    tf_settle();

    printf("     turn_full 0 (the default): nothing changes\n");
    unsigned long f0 = n_force, r0 = host_cd_n, t0 = n_turn_full;
    page_turn(1);
    flush();
    CHECK(n_force == f0 && !promote_live && n_turn_full == t0, "turn_full 0: no byte set");
    CHECK(host_cd_n == r0 + 1 && host_cd_turn == 1, "turn_full 0: the turn is handed to the helper as before");

    printf("     turn_full 1, a page without colour (or an unreadable state buffer): nothing changes\n");
    ggq("qml", "GGQ 9 set turn_full 7");
    CHECK(cfg.turn_full == 3, "the key is clamped to 0..3 (%d)", cfg.turn_full);
    cfg.turn_full = 1; // 1.8.26: T48 is mode 1 (the full STD turn alone, no second pass); T49 is mode 2
    for (int k = 0; k < 2; k++) {
        host_colour = k ? -1 : 0;
        tf_settle();
        f0 = n_force; r0 = host_cd_n;
        page_turn(k);
        flush();
        CHECK(n_force == f0 && !promote_live && host_cd_n == r0 + 1, "colour %d: not promoted, handed to the helper", host_colour);
    }
    host_colour = 300;

    printf("     turn_full 1, a page with colour: one full STD, no co-drive, no clean after it\n");
    tf_settle();
    f0 = n_force; r0 = host_cd_n; t0 = n_turn_full;
    unsigned long k0 = n_turn_full_took, c0 = cleans();
    long id0 = cd_turn_id;
    host_cd_verdict = 2; // whatever the helper would say: it is not asked
    unsigned long tk0 = n_tfa_kick;
    sd("finalizeNewPage:");
    CHECK(n_force == f0 + 1 && promote_live && n_turn_full == t0 + 1 && turn_full_np == sd_newpage_n, "the byte is set at finalizeNewPage");
    CHECK(n_tfa_kick == tk0 + 1, "the take-back timer is started with it (%lu)", n_tfa_kick - tk0);
    elapse(0.1);
    tf_full_turn(0);
    CHECK(!promote_live && !turn_full_np, "the frame took the byte");
    CHECK(host_cd_n == r0 && cd_turn_id == id0 && n_turn_full_took == k0 + 1, "nothing handed to the helper, no id (%lu)", host_cd_n - r0);
    flush();
    CHECK(!auto_req && cd_fail_seen >= cd_fail_id, "no clean armed after the full STD");
    // a later frame of the same turn (pending tiles): a later frame of the newest id, which the refresh covers
    elapse(0.3);
    fill(112, 0, 1508, 1000, 255);
    partial("content ...", 112, 0, 1508, 2088);
    flush();
    CHECK(host_cd_n == r0 + 1 && host_cd_turn == 2 && cd_turn_id == id0, "the later frame is a later frame (turn %d), no new id", host_cd_turn);
    ggq("qml", "GGQ 0 settle");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_force == f0 + 1, "one flash in all: no clean after it (%lu)", cleans() - c0);
    host_cd_verdict = 1;

    printf("     review: the same before any turn was handed to the helper (no id yet): the later frame arms no clean\n");
    {
        long s_id = cd_turn_id, s_ok = cd_ok_id, s_fail = cd_fail_id, s_seen = cd_fail_seen, s_pend = cd_pend_id, s_track = cd_track_id;
        tf_settle();
        cd_turn_id = cd_ok_id = cd_fail_id = cd_fail_seen = cd_pend_id = cd_track_id = 0;
        c0 = cleans();
        sd("finalizeNewPage:");
        elapse(0.1);
        tf_full_turn(1);
        CHECK(cd_turn_id == 0 && turn_full_done_n == sd_newpage_n, "the promoted turn took no id");
        elapse(0.25);
        fill(112, 0, 1508, 1000, 255);
        partial("content ...", 112, 0, 1508, 2088);
        flush();
        CHECK(host_cd_turn == 2 && cd_turn_id == 0, "its later frame: still no id (turn %d)", host_cd_turn);
        CHECK(!auto_req, "the later frame of a promoted turn arms no page clean");
        ggq("qml", "GGQ 0 settle");
        elapse(3.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 && !auto_req, "no clean after it (%lu)", cleans() - c0);
        // the turn after it, not promoted (key off): an ordinary turn again
        cfg.turn_full = 0;
        tf_settle();
        r0 = host_cd_n;
        page_turn(0);
        flush();
        CHECK(host_cd_n == r0 + 1 && host_cd_turn == 1 && cd_turn_id == 1, "the next turn is an ordinary one (turn %d id %ld)", host_cd_turn, cd_turn_id);
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        cfg.turn_full = 1;
        auto_req = 0;
        cd_turn_id = s_id; cd_ok_id = s_ok; cd_fail_id = s_fail; cd_fail_seen = s_seen; cd_pend_id = s_pend; cd_track_id = s_track;
    }

    printf("     the take-backs: pen near, a new gesture, no frame at all\n");
    tf_settle();
    f0 = n_force; r0 = host_cd_n;
    unsigned long w0 = n_withdraw_host, u0 = n_unpromote;
    sd("finalizeNewPage:");
    CHECK(promote_live && n_force == f0 + 1, "promoted");
    sd("penCloseChanged: true");
    CHECK(!promote_live && n_withdraw_host == w0 + 1 && n_unpromote == u0 + 1, "the pen came near: taken back");
    elapse(0.1);
    tf_turn_frame(1);
    flush();
    CHECK(host_cd_n == r0 + 1 && host_cd_turn == 1, "its frame goes the usual way (turn %d)", host_cd_turn);
    f0 = n_force;
    sd("finalizeNewPage:");
    CHECK(!promote_live && n_force == f0, "pen near: not promoted");
    sd("penCloseChanged: false");
    tf_settle();
    f0 = n_force; w0 = n_withdraw_host;
    sd("finalizeNewPage:");
    sd("gestureModeChanged: true");
    CHECK(!promote_live && n_force == f0 + 1 && n_withdraw_host == w0 + 1, "a new gesture: taken back");
    sd("finalizeNewPage:");
    CHECK(!promote_live && n_force == f0 + 1, "in a gesture: not promoted");
    sd("gestureModeChanged: false");
    sd(" - reset");
    tf_settle();
    f0 = n_force; w0 = n_withdraw_host; u0 = n_unpromote;
    sd("finalizeNewPage:");
    CHECK(promote_live && n_force == f0 + 1, "promoted");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(promote_live && n_withdraw_host == w0, "0.5 s without its frame: still waiting");
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(!promote_live && !turn_full_np && n_withdraw_host == w0 + 1 && n_unpromote == u0 + 1, "1.2 s without its frame: taken back at the tick");
    // the same through the 20 s heartbeat
    tf_settle();
    w0 = n_withdraw_host;
    sd("finalizeNewPage:");
    elapse(21.0);
    ggq("qml", "GGQ 0 hb");
    CHECK(!promote_live && n_withdraw_host == w0 + 1, "taken back at the heartbeat");
    // second review: a second finalizeNewPage without a frame does not make the first one's byte younger
    tf_settle();
    w0 = n_withdraw_host; f0 = n_force;
    sd("finalizeNewPage:");
    elapse(0.6);
    sd("finalizeNewPage:");
    CHECK(promote_live && n_force == f0 + 1, "a second finalizeNewPage: the byte is set once");
    elapse(0.6);
    turn_full_age("the test");
    CHECK(!promote_live && n_withdraw_host == w0 + 1, "1.2 s after the byte was set: taken back, whatever came after it");

    printf("     second review: a view change before the turn's frame takes the turn's byte back first\n");
    tf_settle();
    {
        unsigned long kk0 = n_kick;
        w0 = n_withdraw_host; u0 = n_unpromote;
        sd("finalizeNewPage:");
        CHECK(promote_live && turn_full_np, "promoted");
        vc("SettingsOpened");
        CHECK(!promote_live && !turn_full_np && tr_active && n_withdraw_host == w0 + 1 && n_unpromote == u0 + 1 && n_kick == kk0 + 1,
              "the view change: our byte taken back, then the helper is asked for the view change's own");
        // end the cycle the usual way: the new view's frame, then its in-place clean
        fill(0, 0, SW, SH, 255);
        partial("settings", 0, 0, SW, SH);
        flush();
        for (int i = 0; i < 20 && (tr_active || auto_req); i++) {
            elapse(0.25);
            ggq("qml", "GGQ 0 settle");
            flush();
        }
        vc("SettingsClosed");
        fill(112, 0, 1508, 2088, 255);
        partial("content ...", 0, 0, SW, SH);
        flush();
        for (int i = 0; i < 40 && (tr_active || auto_req); i++) {
            elapse(0.25);
            ggq("qml", "GGQ 0 settle");
            flush();
        }
        CHECK(!tr_active, "the view-change cycles ended (%d)", tr_active);
        tf_settle();
    }

    printf("     another frame takes the byte before the page is drawn: the turn's own frame goes the usual way\n");
    tf_settle();
    r0 = host_cd_n; k0 = n_turn_full_took;
    sd("finalizeNewPage:");
    elapse(0.05);
    full(); // nothing changed on the screen
    CHECK(!promote_live && n_turn_full_took == k0 && host_cd_n == r0 && cd_np_n != sd_newpage_n, "an unchanged frame is not the turn's");
    elapse(0.1);
    tf_turn_frame(0);
    flush();
    CHECK(host_cd_n == r0 + 1 && host_cd_turn == 1, "the turn's frame is handed to the helper (turn %d)", host_cd_turn);
    // review: the same for a frame that changes a small spot only (the page number's box, a status mark)
    tf_settle();
    r0 = host_cd_n; k0 = n_turn_full_took;
    sd("finalizeNewPage:");
    elapse(0.05);
    fill(681, 2088, 258, 72, 0);
    full();
    CHECK(!promote_live && n_turn_full_took == k0 && host_cd_n == r0 && cd_np_n != sd_newpage_n && turn_full_done_n != sd_newpage_n,
          "a frame that changes a small spot is not the turn's");
    elapse(0.1);
    fill(681, 2088, 258, 72, 255);
    tf_turn_frame(1);
    flush();
    CHECK(host_cd_n == r0 + 1 && host_cd_turn == 1, "the turn's frame is handed to the helper and can get its clean (turn %d)", host_cd_turn);

    printf("     never while something else waits or is switched off\n");
    tf_settle();
    f0 = n_force;
    kb_on = 1; sd("finalizeNewPage:"); kb_on = 0;
    CHECK(n_force == f0 && !promote_live, "keyboard up: not promoted");
    tr_active = 1; sd("finalizeNewPage:"); tr_active = 0;
    CHECK(n_force == f0 && !promote_live, "a view change waits for its clean: not promoted");
    auto_req = 1; auto_page_idle = 0; sd("finalizeNewPage:"); auto_req = 0;
    CHECK(n_force == f0 && !promote_live, "a request waits: not promoted");
    sd_mono = 1; sd("finalizeNewPage:"); sd_mono = 0;
    CHECK(n_force == f0 && !promote_live, "mono state: not promoted");
    pw("Changing display state from Normal to DeepSleep (setDisplayState /x)");
    sd("finalizeNewPage:");
    pw("Changing display state from DeepSleep to Normal (setDisplayState /x)");
    CHECK(n_force == f0 && !promote_live, "asleep: not promoted");
    cfg.codrive = 2; sd("finalizeNewPage:"); cfg.codrive = 0; sd("finalizeNewPage:"); cfg.codrive = 1;
    CHECK(n_force == f0 && !promote_live, "codrive 0 / 2: not promoted");
    cfg.page_clean = 0; sd("finalizeNewPage:"); cfg.page_clean = 1;
    CHECK(n_force == f0 && !promote_live, "page_clean 0: not promoted");
    promote_live = 1; turn_full_np = 0; sd("finalizeNewPage:");
    CHECK(n_force == f0 && promote_live && !turn_full_np, "a gesture end's byte is in flight: left to it");
    promote_live = 0;
    sd("finalizeNewPage:"); // none of the above is left standing: the same line is promoted now
    CHECK(n_force == f0 + 1 && promote_live, "with nothing in the way it is promoted");
    sd("penCloseChanged: true"); sd("penCloseChanged: false");
    f0 = n_force;
    sd("visibleChanged: false"); sd("finalizeNewPage:");
    CHECK(n_force == f0 && !promote_live, "document view hidden: not promoted");

    printf("     review: not at a document's open (finalizeNewPage 16-48 ms after visibleChanged: true)\n");
    sd("visibleChanged: true");
    elapse(0.03);
    sd("finalizeNewPage:");
    CHECK(n_force == f0 && !promote_live, "the open's finalizeNewPage: not promoted");
    elapse(0.2);
    tf_turn_frame(0); // the open's first page
    flush();
    elapse(1.5);
    sd("finalizeNewPage:"); // xochitl's own in-place STD of the open has not come yet
    CHECK(n_force == f0 && !promote_live, "before the open's own full refresh: not promoted");
    elapse(0.1);
    tf_turn_frame(1);
    flush();
    full(); // the open's in-place STD
    flush();
    elapse(2.0);
    auto_req = 0; tr_active = 0;
    sd("finalizeNewPage:");
    CHECK(n_force == f0 + 1 && promote_live, "after the open's full refresh: a turn is promoted again");
    elapse(0.1);
    tf_full_turn(0);
    flush();

    printf("     a gesture end's promoted redraw is not taken for a turn\n");
    tf_settle();
    sd("finalizeNewPage:");
    sd("penCloseChanged: true"); // taken back; turn_full_np keeps the turn's number
    sd("penCloseChanged: false");
    promote_live = 1; turn_full_np = 0; // what the gesture end's promotion leaves
    k0 = n_turn_full_took; r0 = host_cd_n;
    elapse(0.1);
    tf_full_turn(1);
    flush();
    CHECK(n_turn_full_took == k0, "not counted as a turn's full STD");

    ggq("qml", "GGQ 9 set turn_full 0");
    CHECK(cfg.turn_full == 0, "set turn_full 0 switches it off");
    tf_settle();
    f0 = n_force;
    page_turn(0);
    flush();
    CHECK(n_force == f0 && !promote_live, "off again: no byte set");
    elapse(5.0);
    ggq("qml", "GGQ 0 settle");

    host_colour = 0; host_cd_verdict = 0;
    auto_req = 0; tr_active = 0; promote_live = 0; kb_on = 0; turn_full_np = 0;
    sd("visibleChanged: false");
    cfg = sv; xo_state = sv_xo; host_rc = sv_rc; host_sw = sv_sw;
}

static void test_codrive(void) {
    printf("T47 1.8.8 co-drive requests\n");
    Config sv = cfg;
    int sv_xo = xo_state;
    const char *sv_rc = host_rc;
    int sv_sw = host_sw;
    cfg.page_clean = 0;
    cfg.codrive = 0;
    cfg.cd_gain = 1.0; cfg.cd_gpos = 0.7; cfg.cd_minv = 0.7; cfg.cd_area = 1000000;
    sd(" - reset");
    sd("visibleChanged: true");
    int H26 = 560; // 560/2160 = 25.9% of the screen
    double frac = (double)H26 / SH;

    #define CD_BASE() do { fill(0, 0, SW, SH, 255); fill(0, 0, SW, H26, 0); full(); flush(); elapse(5.0); } while (0)
    #define CD_ERASE() do { fill(0, 0, SW, H26, 255); partial("ui ........", 0, 0, SW, SH); flush(); elapse(5.0); } while (0)
    #define CD_DRAW() do { fill(0, 0, SW, H26, 0); partial("ui ........", 0, 0, SW, SH); flush(); elapse(5.0); } while (0)

    printf("     codrive 0: no request\n");
    CD_BASE();
    unsigned long r0 = host_cd_n;
    CD_ERASE();
    CHECK(host_cd_n == r0, "codrive=0: no request (%lu)", host_cd_n - r0);

    printf("     codrive 1: the erase and the draw update\n");
    cfg.codrive = 1;
    CD_BASE();
    r0 = host_cd_n;
    unsigned long q0 = n_cd[CD_REQ];
    CD_ERASE();
    CHECK(host_cd_n == r0 + 1 && n_cd[CD_REQ] == q0 + 1, "erase: exactly one request (%lu)", host_cd_n - r0);
    CHECK(fabs(host_cd_fe - frac) <= 0.1 * frac, "erase: fe %.4f ~ %.4f", host_cd_fe, frac);
    CHECK(host_cd_fd < 0.1 * frac, "erase: fd %.4f ~ 0", host_cd_fd);
    CHECK(fabs(cd_lfe - frac) <= 0.1 * frac, "last large update: fe %.4f", cd_lfe);
    r0 = host_cd_n;
    CD_DRAW();
    CHECK(host_cd_n == r0 + 1, "draw (gpos 0.7): one request (%lu)", host_cd_n - r0);
    CHECK(fabs(host_cd_fd - frac) <= 0.1 * frac, "draw: fd %.4f ~ %.4f", host_cd_fd, frac);
    CHECK(host_cd_fe < 0.1 * frac, "draw: fe %.4f ~ 0", host_cd_fe);
    cfg.cd_gpos = 0.0;
    CD_ERASE();
    r0 = host_cd_n;
    CD_DRAW();
    CHECK(host_cd_n == r0, "draw with cd_gpos 0: no request");
    cfg.cd_gpos = 0.7;

    printf("     codrive 1: small, balanced and sleeping updates\n");
    CD_BASE();
    r0 = host_cd_n;
    fill(0, 0, 400, 400, 255); // 160000 px, below cd_area
    partial("ui ........", 0, 0, 400, 400);
    flush();
    elapse(5.0);
    CHECK(host_cd_n == r0, "small update (below cd_area): no request");
    fill(0, 0, SW / 2, SH, 0); fill(SW / 2, 0, SW - SW / 2, SH, 255); full(); flush(); elapse(5.0);
    r0 = host_cd_n;
    fill(0, 0, SW / 2, SH, 255); fill(SW / 2, 0, SW - SW / 2, SH, 0);
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(5.0);
    CHECK(host_cd_n == r0 + 1 && fabs(host_cd_fe - 0.5) < 0.05 && fabs(host_cd_fd - 0.5) < 0.05,
          "balanced erase / draw: asked, the helper decides from the rows (fe %.3f fd %.3f)", host_cd_fe, host_cd_fd);
    CD_BASE();
    r0 = host_cd_n;
    __atomic_store_n(&sleeping, 1, __ATOMIC_RELEASE);
    CD_ERASE();
    __atomic_store_n(&sleeping, 0, __ATOMIC_RELEASE);
    CHECK(host_cd_n == r0, "sleeping: no request");

    printf("     the page turn's clean: codrive 1 replaces it, codrive 2 keeps it\n");
    cfg.defer = 1; cfg.tr_quiet = 0.5; cfg.tr_full = 0.0; cfg.gesture_clean = 1;
    cfg.page_gap = 1; cfg.page_px = 80000; cfg.page_every = 6; cfg.page_flip = 2.0; cfg.page_settle = 1.5;
    cfg.turn_px = 6000; cfg.page_quiet = 0.2; cfg.auto_quiet = 0.4; cfg.stock_gap = 3.0;
    xo_state = 0; host_rc = "ok"; host_sw = 1;
    auto_req = 0; tr_active = 0; promote_live = 0; kb_on = 0;
    for (int mode = 1; mode <= 4; mode++) { // 3: codrive 1, a turn the helper could not do; 4 (1.8.13): no answer at all
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 0;
        cfg.codrive = 0;
        page_turn(0);
        flush();
        full();
        flush();
        elapse(5.0);
        auto_req = 0;
        cfg.page_clean = 1;
        cfg.codrive = mode >= 3 ? 1 : mode;
        host_cd_verdict = mode == 1 ? 1 : mode == 3 ? 2 : 0;
        unsigned long c0 = cleans();
        page_turn(1);
        flush();
        if (mode == 1) {
            CHECK(!auto_req, "codrive 1: the page turn arms no clean (count %d)", n_pchg);
            elapse(0.6);
            ggq("qml", "GGQ 0 settle");
            elapse(2.5);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0, "codrive 1: no clean performed");
        } else {
            if (mode == 4) {
                CHECK(!auto_req, "mode 4: nothing armed while the helper may still answer");
                elapse(3.1); // 1.8.24: the answer is given up on after 3 s (CD_PEND_MS), it was 2 s
                ggq("qml", "GGQ 0 settle");
            }
            if (mode == 3) ggq("qml", "GGQ 0 settle");
            // mode 4: the page has settled for 3.1 s by the time the answer is given up on, so the clean runs at once
            CHECK(mode == 4 ? cleans() == c0 + 1 : auto_req && auto_kind == AK_PAGE, "mode %d: the page clean is armed (count %d)", mode, n_pchg);
            CHECK(cd_fail_seen >= cd_fail_id && !cd_pend_id, "mode %d: the answer is used up", mode);
            elapse(mode >= 3 ? 1.6 : 0.6);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0 + 1, "mode %d: the clean is performed", mode);
        }
        host_cd_verdict = 0;
    }
    elapse(5.0);
    ggq("qml", "GGQ 0 settle");

    printf("     1.8.14: a single failed turn is cleaned at once, a failed turn's clean survives leafing\n");
    elapse(40.0);
    ggq("qml", "GGQ 0 hb");
    cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
    host_cd_verdict = 2;
    unsigned long c1 = cleans();
    page_turn(0);
    flush();
    ggq("qml", "GGQ 0 settle");
    CHECK(auto_req && auto_kind == AK_PAGE && fabs(auto_quiet_need - cfg.page_quiet) < 1e-9,
          "single failed turn: armed, waits page_quiet (%.2f)", auto_quiet_need);
    host_cd_verdict = 1;
    for (int i = 0; i < 6; i++) { elapse(0.9); page_turn(i & 1 ? 0 : 1); flush(); ggq("qml", "GGQ 0 settle"); }
    CHECK(auto_req && auto_kind == AK_PAGE, "5.4 s of leafing after the failure: the request is still alive");
    CHECK(fabs(auto_quiet_need - cfg.page_settle) < 1e-9, "... and waits until the turning stopped (%.2f)", auto_quiet_need);
    CHECK(cleans() == c1, "no clean while the user leafs");
    elapse(1.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c1 + 1, "the clean is performed once the turning stopped (%lu)", cleans() - c1);

    printf("     1.8.14: a full refresh covers the turns before it; a late answer is no new failure\n");
    elapse(40.0);
    ggq("qml", "GGQ 0 hb");
    auto_req = 0;
    host_cd_verdict = 0;
    page_turn(1);
    flush();
    CHECK(cd_pend_id == cd_turn_id, "the turn waits for its answer");
    full();
    flush();
    CHECK(!cd_pend_id && cd_fail_seen >= cd_turn_id, "full refresh: nothing waits, the turn is covered");
    cd_fail_id = cd_turn_id; cd_fail_ms = (long)(now_s() * 1000.0); // the helper's answer comes after the refresh
    elapse(4.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req, "a failure answered after the full refresh arms nothing");
    c1 = cleans();
    host_cd_verdict = 0;
    elapse(40.0);
    ggq("qml", "GGQ 0 hb");
    page_turn(0);
    flush();
    elapse(2.1);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c1 && cd_pend_id == cd_turn_id, "1.8.24: a turn unanswered for 2.1 s still waits (the limit is 3 s)");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle"); // unconfirmed: armed and (settled) cleaned
    long idu = cd_turn_id;
    CHECK(cleans() == c1 + 1 && cd_fail_seen >= idu, "unanswered turn: cleaned, its id is dealt with");
    cd_fail_id = idu; cd_fail_ms = (long)(now_s() * 1000.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req, "the late failure for the same turn arms nothing");
    host_cd_verdict = 0;
    elapse(5.0);
    ggq("qml", "GGQ 0 settle");

    // ------------------------------------------------------------------ 1.8.15
    #define WFC(i) (n_clean_wf[i])
    printf("     1.8.15: a full-screen clean with TEXT targets leaves no STD level behind (cd_std_next), clean_text off / warm panel / no text support do not\n");
    {
        double sv_temp = host_temp;
        int sv_tok = host_text_ok;
        int sv_ct = cfg.clean_text;
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        host_rc = "ok"; host_sw = 1; host_temp = 21.5; auto_req = 0;
        // (a) text on: full rclean
        host_text_ok = 1; cfg.clean_text = 1;
        cd_std_next = 1;
        unsigned long w2 = WFC(2), w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(last_clean_text == 1 && cd_std_next == 0, "text full rclean: cd_std_next cleared (%d)", cd_std_next);
        CHECK(cd_text_next == 1, "text full rclean: cd_text_next set (%d)", cd_text_next);
        CHECK(WFC(3) == w3 + 1 && WFC(2) == w2, "text full rclean: counted in wf 3 (%lu / %lu)", WFC(3) - w3, WFC(2) - w2);
        pthread_mutex_lock(&mu); write_status("t15", ""); pthread_mutex_unlock(&mu);
        char *js = slurp(TESTRUN "/status.json");
        unsigned long sw_[8] = {0};
        const char *pw_ = js ? strstr(js, "\"wfCleans\":[") : NULL;
        if (pw_) sscanf(pw_ + 12, "%lu,%lu,%lu,%lu", &sw_[0], &sw_[1], &sw_[2], &sw_[3]);
        CHECK(pw_ && sw_[3] == WFC(3) && sw_[2] == WFC(2), "status wfCleans shows the text clean at index 3 (%lu, %lu)", sw_[3], sw_[2]);
        // (b) clean_text 0
        ggq("qml", "GGQ 6 set clean_text 0");
        CHECK(cfg.clean_text == 0, "set clean_text 0 -> %d", cfg.clean_text);
        cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(last_clean_text == 0 && cd_std_next == 1 && WFC(2) == w2 + 1 && WFC(3) == w3,
              "clean_text 0: the STD level stays (%d), wf 2 counted", cd_std_next);
        ggq("qml", "GGQ 6 set clean_text 1");
        CHECK(cfg.clean_text == 1, "set clean_text 1 -> %d", cfg.clean_text);
        // (c) host without text support
        host_text_ok = 0; cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(cd_std_next == 1 && WFC(2) == w2 + 1 && WFC(3) == w3, "host_text_ok 0: unchanged behaviour");
        CHECK(cd_text_next == 0, "a stock clean after a text clean takes cd_text_next back (%d)", cd_text_next);
        // (c2) review: the panel is busy. Behind an accepted update the TEXT targets are fine, not while one waits
        host_text_ok = 1; host_sw = 0; host_queued = 1; cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(last_clean_text == 0 && cd_std_next == 1 && cd_text_next == 0 && WFC(2) == w2 + 1 && WFC(3) == w3,
              "busy panel with a waiting update: the stock clean (%d %d)", last_clean_text, cd_std_next);
        host_queued = 0; cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(last_clean_text == 1 && cd_std_next == 0 && cd_text_next == 1 && WFC(3) == w3 + 1 && WFC(2) == w2,
              "busy panel, nothing waiting: TEXT targets (%d %d)", last_clean_text, cd_std_next);
        host_sw = 1;
        // (d) warm panel
        host_text_ok = 1; host_temp = 28.5; cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        CHECK(cd_std_next == 1 && WFC(2) == w2 + 1 && WFC(3) == w3, "28.5 C: no TEXT targets, the STD level stays");
        host_temp = 21.5;
        // (e) partial clean with text: counted, cd_std_next untouched
        cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        ggq("qml", "GGQ 5 rclean 100 100 800 800");
        CHECK(last_clean_text == 1 && cd_std_next == 1 && WFC(3) == w3 + 1 && WFC(2) == w2,
              "partial text clean: cd_std_next untouched (%d), wf 3 counted", cd_std_next);
        // (f) an automatic page clean after a failed turn
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
        host_cd_verdict = 2;
        unsigned long cf = cleans();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE, "failed turn: the page clean is armed");
        cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        elapse(1.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == cf + 1 && cd_std_next == 0 && WFC(3) == w3 + 1 && WFC(2) == w2,
              "automatic page clean with text: cd_std_next cleared (%d), wf 3 counted", cd_std_next);
        // the same with clean_text 0
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        auto_req = 0; cfg.clean_text = 0;
        host_cd_verdict = 2;
        cf = cleans();
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        cd_std_next = 1; w2 = WFC(2); w3 = WFC(3);
        elapse(1.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == cf + 1 && cd_std_next == 1 && WFC(2) == w2 + 1 && WFC(3) == w3,
              "automatic page clean, clean_text 0: the STD level stays (%d)", cd_std_next);
        host_cd_verdict = 0; host_text_ok = sv_tok; host_temp = sv_temp; cfg.clean_text = sv_ct; auto_req = 0;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
    }

    printf("     1.8.15: two turns inside one tick: the earlier one still counts for leafing\n");
    for (int variant = 0; variant < 2; variant++) {
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
        host_rc = "ok"; host_sw = 1;
        host_cd_verdict = 2; // the first turn fails
        unsigned long cb = cleans();
        page_turn(0);
        if (variant == 0) flush();
        elapse(0.1);
        host_cd_verdict = 1; // the second one is confirmed
        double prev0 = cd_turn_prev_t;
        page_turn(1);
        // variant 1: the first frame is still unfinalized when the second "finalizeNewPage" came (pend_t < sd_newpage_t)
        if (variant == 1) CHECK(cd_turn_prev_t > prev0 + 1.0, "variant 1: the older turn advanced cd_turn_prev_t");
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(cd_leaf == 1, "variant %d: the second turn counts as leafing (%d)", variant, cd_leaf);
        CHECK(auto_req && auto_kind == AK_PAGE && fabs(auto_quiet_need - cfg.page_settle) < 1e-9,
              "variant %d: the page request waits page_settle (%.2f)", variant, auto_quiet_need);
        CHECK(cleans() == cb, "variant %d: no clean at the tick right after", variant);
        elapse(1.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == cb + 1, "variant %d: the clean goes out once the turning stopped", variant);
        host_cd_verdict = 0;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
    }

    printf("     1.8.15: a full refresh that covered a waiting page request is not undone by a later confirmed turn\n");
    for (int variant = 0; variant < 2; variant++) {
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
        host_rc = "ok"; host_sw = 1;
        host_cd_verdict = 2;
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE, "variant %d: a request waits (page (no co-drive))", variant);
        unsigned long cc = cleans();
        unsigned long st0 = n_auto_stock, cv0 = n_auto_covered;
        if (variant == 0) { full(); flush(); }
        else {
            ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
            cc = cleans();
        }
        host_cd_verdict = 1;
        page_turn(1);
        flush();
        CHECK(auto_req && (variant == 0 ? t_stock_full : t_our_full) >= auto_t,
              "variant %d: the request was not refreshed by the later turn", variant);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req, "variant %d: the tick dropped the request", variant);
        CHECK(cleans() == cc, "variant %d: no clean was sent (%lu)", variant, cleans() - cc);
        CHECK(variant == 0 ? n_auto_stock == st0 + 1 : n_auto_covered == cv0 + 1, "variant %d: dropped for the right reason", variant);
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == cc, "variant %d: still no clean later", variant);
        host_cd_verdict = 0;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
    }

    printf("     1.8.15: a single later turn (not leafing) keeps the short quiet time of a held-back page clean\n");
    {
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
        host_rc = "ok"; host_sw = 1;
        host_cd_verdict = 2;
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE, "failed turn: armed");
        unsigned long cd0 = cleans();
        host_rc = "busy"; // the clean is held back (retried at the next tick, same deadline)
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && cleans() == cd0, "the clean is held back");
        elapse(1.9);
        host_cd_verdict = 1;
        page_turn(1);
        flush();
        CHECK(!cd_leaf, "the later turn (2.2 s after the first) is not leafing");
        CHECK(auto_req && auto_kind == AK_PAGE && fabs(auto_quiet_need - cfg.page_quiet) < 1e-9,
              "the held-back request waits page_quiet, not page_settle (%.2f)", auto_quiet_need);
        host_rc = "ok";
        host_cd_verdict = 0;
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
    }
    #undef WFC

    printf("     1.8.19: the later frames of one turn (pending tiles) share the turn's id and verdict\n");
    {
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        cfg.page_clean = 1; cfg.codrive = 1; auto_req = 0;
        host_cd_verdict = 1;
        unsigned long c2 = cleans(), rq = host_cd_n;
        long id0 = cd_turn_id;
        page_turn(0);
        flush();
        elapse(0.25);
        redraw_page(0); // the same page's tiles once more: no finalizeNewPage
        flush();
        elapse(0.15);
        redraw_page(0);
        flush();
        CHECK(cd_turn_id == id0 + 1, "three frames of one turn: one id (%ld)", cd_turn_id - id0);
        CHECK(host_cd_n == rq + 3 && host_cd_turn == 2, "... each of them is handed to the helper, the last as a later frame (%lu, %d)",
              host_cd_n - rq, host_cd_turn);
        CHECK(!cd_leaf, "the later frames are not leafing");
        ggq("qml", "GGQ 0 settle");
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cleans() == c2 && !cd_pend_id, "confirmed turn with later frames: no clean, nothing waits");
        // the later frame gets no id (and so no verdict) of its own
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        id0 = cd_turn_id;
        page_turn(1);
        flush();
        elapse(0.3);
        host_cd_verdict = 2;
        redraw_page(1);
        flush();
        CHECK(cd_turn_id == id0 + 1, "later frame: no new id (%ld)", cd_turn_id - id0);
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cleans() == c2, "... and no clean of its own");
        // the next finalizeNewPage is a new turn again
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        host_cd_verdict = 1;
        id0 = cd_turn_id;
        page_turn(0);
        flush();
        elapse(0.9);
        page_turn(1);
        flush();
        CHECK(cd_turn_id == id0 + 2, "two turns 0.9 s apart: two ids (%ld)", cd_turn_id - id0);
        CHECK(cd_leaf, "... and that is leafing");
        // after 0.6 s a large redraw is no frame of the turn
        elapse(0.7);
        rq = host_cd_n;
        id0 = cd_turn_id;
        redraw_page(0); // other content than the page's: a large change
        flush();
        CHECK(cd_turn_id == id0 && host_cd_n == rq + 1 && host_cd_turn == 0,
              "a large redraw 0.7 s after the turn: a request, but no turn's frame (%ld, %lu, %d)", cd_turn_id - id0, host_cd_n - rq, host_cd_turn);
        host_cd_verdict = 0;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
    }

    printf("     1.8.22: a turn left without co-drive for a colour (cd_std 2) cleans from turn_px_col on\n");
    {
        int sv_col = host_colour;
        double sv_w = cfg.w[CL_CONTENT];
        cfg.w[CL_CONTENT] = 1.2; // one turn of the test pages leaves ghost=131022 (hot ~12300)
        CHECK(sv.turn_px_col == 400 && sv.turn_px == 6000, "defaults: turn_px_col %d, turn_px %d", sv.turn_px_col, sv.turn_px);
        cfg.page_clean = 1; cfg.codrive = 1; cfg.turn_full = 0;
        // verdict 2: the helper says "not co-driven"; 0: no answer (the turn is given up on after 2 s)
        struct { int col, std, px, pxc, verdict, clean, mark; const char *what; } cs[] = {
            {300, 2, (int)NS, 400, 2, 1, 1, "colour, cd_std 2: turn_px_col decides, cleaned"},
            {0, 2, (int)NS, 400, 2, 0, 0, "no colour: turn_px decides, no clean"},
            {-1, 2, (int)NS, 400, 2, 0, 0, "state not readable: turn_px decides, no clean"},
            {300, 1, (int)NS, 400, 2, 0, 0, "cd_std 1: turn_px decides, no clean"},
            {300, 0, (int)NS, 400, 2, 0, 0, "cd_std 0: turn_px decides, no clean"},
            {300, 2, (int)NS, (int)NS - 1, 2, 0, 1, "turn_px_col above the residue (and below turn_px): it refuses, no clean"},
            {300, 2, 400000, 50000, 2, 1, 1, "residue between the two thresholds, colour: cleaned"},
            {300, 2, 400000, 200000, 2, 0, 1, "residue below both thresholds, colour: no clean"},
            {0, 2, 400000, 50000, 2, 0, 0, "residue between the two thresholds, no colour: no clean"},
            {300, 2, 6000, (int)NS, 2, 1, 1, "turn_px_col above turn_px: never the higher one, cleaned"},
            {300, 2, (int)NS, 400, 0, 1, 1, "colour, no answer from the helper: turn_px_col decides, cleaned"},
            {0, 2, (int)NS, 400, 0, 0, 0, "no colour, no answer: turn_px decides, no clean"},
        };
        for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            elapse(40.0);
            ggq("qml", "GGQ 0 hb");
            auto_req = 0;
            host_colour = cs[i].col; cfg.cd_std = cs[i].std; cfg.turn_px = cs[i].px; cfg.turn_px_col = cs[i].pxc;
            host_cd_verdict = cs[i].verdict;
            unsigned long c0 = cleans(), pn0 = n_page_none;
            page_turn((int)(i & 1));
            flush();
            if (cs[i].verdict == 2) {
                ggq("qml", "GGQ 0 settle");
                CHECK(auto_req && auto_kind == AK_PAGE && auto_page_col == cs[i].mark,
                      "%s: armed, colour mark %d", cs[i].what, auto_page_col);
                elapse(0.3);
            } else {
                elapse(3.1); // 1.8.24: CD_PEND_MS
            }
            ggq("qml", "GGQ 0 settle");
            CHECK(!auto_req && cleans() == c0 + (unsigned long)cs[i].clean && n_page_none == pn0 + (cs[i].clean ? 0 : 1),
                  "%s (cleans %lu, left alone %lu)", cs[i].what, cleans() - c0, n_page_none - pn0);
        }
        // a colour mark on a turn that passed must not lower the threshold of the next (black and white) turn
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        auto_req = 0;
        host_colour = 300; cfg.cd_std = 2; cfg.turn_px = (int)NS; cfg.turn_px_col = 400;
        host_cd_verdict = 1;
        unsigned long c0 = cleans();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cd_col_id == cd_turn_id, "a marked turn the helper confirmed: nothing armed");
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        host_colour = 0; host_cd_verdict = 2;
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_col, "the next turn (no colour) fails: armed without the colour mark");
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cleans() == c0, "... and turn_px decides: no clean");
        // a page request that waits (a black and white turn failed) takes the mark of a colour turn that fails after it
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        auto_req = 0;
        c0 = cleans();
        host_colour = 0; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_col, "leafing: a black and white turn fails, armed without the mark");
        host_colour = 300;
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && auto_page_col == 1, "... a colour turn fails while it waits: the request takes the mark (%d)", auto_page_col);
        unsigned long pa = n_arm[AK_PAGE], pn = n_page_none, pp;
        elapse(2.0);
        ggq("qml", "GGQ 0 settle");
        elapse(0.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cleans() == c0 + 1, "... and turn_px_col decides: one clean (%lu)", cleans() - c0);
        CHECK(n_arm[AK_PAGE] == pa && n_page_none == pn, "... by that request itself (%lu new requests, %lu refusals)",
              n_arm[AK_PAGE] - pa, n_page_none - pn);
        // ... and keeps a mark it has over the black and white turns that follow (the colour turn's residue is still there)
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        auto_req = 0;
        c0 = cleans();
        host_colour = 300; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_page_col == 1, "leafing: a colour turn fails, armed with the mark");
        host_colour = 0; host_cd_verdict = 1;
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_page_col == 1, "... a confirmed black and white turn after it: the request keeps the mark");
        elapse(2.0);
        ggq("qml", "GGQ 0 settle");
        elapse(0.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cleans() == c0 + 1, "... one clean on the page the turning stopped on (%lu)", cleans() - c0);
        // a failed turn dealt with while a NEWER turn carries the colour mark: the page shown is the newer turn's
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        auto_req = 0;
        c0 = cleans();
        host_colour = 0; host_cd_verdict = 2;
        page_turn(1);
        flush();
        host_colour = 300; host_cd_verdict = 0; // the colour turn: marked, the helper's answer is still out
        page_turn(0);
        flush();
        CHECK(cd_col_id == cd_turn_id && cd_fail_id == cd_turn_id - 1, "two turns before a tick: the older failed, the newer is marked");
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && auto_page_col == 1, "... the older turn's request takes the newer turn's mark (%d)", auto_page_col);
        pa = n_arm[AK_PAGE]; pn = n_page_none;
        elapse(2.0);
        ggq("qml", "GGQ 0 settle");
        elapse(0.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_page_none == pn, "... and is cleaned by turn_px_col (%lu, %lu refusals)", cleans() - c0, n_page_none - pn);
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1, "... once (%lu)", cleans() - c0);
        auto_req = 0;
        #define COL_RESET() do { elapse(40.0); ggq("qml", "GGQ 0 hb"); auto_req = 0; c0 = cleans(); } while (0)
        #define COL_TICKS(n) do { for (int k_ = 0; k_ < (n); k_++) { elapse(0.3); ggq("qml", "GGQ 0 settle"); } } while (0)
        cfg.cd_std = 2; cfg.turn_px = (int)NS; cfg.turn_px_col = 400;
        // black and white leafing never takes a mark (an older turn's mark is still set)
        COL_RESET();
        host_colour = 0; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        elapse(0.3);
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_col && cd_col_id > 0, "black and white leafing, both turns failed: the request has no mark");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0, "... turn_px decides: no clean (%lu)", cleans() - c0);
        // a newer turn that carries the mark but was confirmed (a GLR that needed nothing, marked by a later frame)
        COL_RESET();
        host_colour = 0; host_cd_verdict = 2;
        page_turn(1);
        flush();
        host_colour = 300; host_cd_verdict = 1;
        page_turn(0);
        flush();
        CHECK(cd_col_id == cd_turn_id && cd_ok_id == cd_turn_id && cd_fail_id == cd_turn_id - 1,
              "two turns before a tick: the older failed, the newer is marked and confirmed");
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_col, "... the older turn's request does not take a confirmed turn's mark");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0, "... turn_px decides: no clean (%lu)", cleans() - c0);
        // the mark that comes with a later frame of the turn the request was armed for (no new failure with it)
        COL_RESET();
        host_colour = 0; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_col && auto_page_id == cd_turn_id, "a turn fails without the mark: armed for its id");
        cd_col_id = cd_turn_id;
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_page_col == 1, "... a later frame of it is left without co-drive for a colour: the request takes the mark");
        pn = n_page_none;
        COL_TICKS(4);
        CHECK(!auto_req && cleans() == c0 + 1 && n_page_none == pn, "... and turn_px_col decides: one clean (%lu)", cleans() - c0);
        // review round 4: a menu drops the request that waits while a newer turn's failure is open behind it
        COL_RESET();
        host_colour = 300; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        elapse(0.3);
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && cd_fail_id > cd_fail_seen, "leafing over colour pages: the second turn's failure is open behind the request");
        elapse(0.3);
        pp = n_popup; pa = n_arm[AK_PAGE];
        partial("ui ........", 500, 400, 620, 1200);
        flush();
        CHECK(!auto_req && n_popup == pp + 1, "... a menu opens: the request is dropped");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "... and no second request is armed: no clean over the menu (%lu cleans, %lu requests)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // ... the same when a pen stroke drops it
        COL_RESET();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        elapse(0.3);
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && cd_fail_id > cd_fail_seen, "leafing over colour pages again");
        elapse(0.3);
        pa = n_arm[AK_PAGE];
        pp = n_auto_ink;
        partial("pen .......", 300, 1600, 200, 40);
        flush();
        COL_TICKS(10);
        CHECK(!auto_req && n_auto_ink == pp + 1 && cleans() == c0 && n_arm[AK_PAGE] == pa,
              "... a pen stroke: the request is dropped, no second one, no clean (%lu cleans, %lu requests)", cleans() - c0, n_arm[AK_PAGE] - pa);
        // ... and when the menu opens before the turn's answer armed anything
        COL_RESET();
        page_turn(1);
        flush();
        elapse(0.1);
        pa = n_arm[AK_PAGE];
        partial("ui ........", 500, 400, 620, 1200);
        flush();
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "a menu right after a failed colour turn, before its tick: no request, no clean (%lu, %lu)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // ... or while the helper's answer is still out (the turn is given up on after 2 s)
        COL_RESET();
        host_cd_verdict = 0;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(!auto_req && cd_pend_id == cd_turn_id, "a colour turn without an answer yet: nothing armed");
        elapse(1.0);
        pa = n_arm[AK_PAGE];
        partial("ui ........", 500, 400, 620, 1200);
        flush();
        elapse(1.2);
        ggq("qml", "GGQ 0 settle");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "... a menu opens meanwhile: no request, no clean (%lu, %lu)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // review round 5: the waiting request ages out (the pen hovers) with a newer turn's failure open behind it
        COL_RESET();
        host_colour = 300; host_cd_verdict = 2;
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        elapse(0.3);
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && cd_fail_id > cd_fail_seen, "leafing over colour pages, then the pen comes near");
        pa = n_arm[AK_PAGE]; pp = n_auto_aged;
        for (int k = 0; k < 18; k++) { elapse(0.5); ggq("qml", "GGQ 0 settle pen"); }
        CHECK(!auto_req && n_auto_aged == pp + 1 && n_arm[AK_PAGE] == pa, "... it hovers for 9 s: the request ages out, no second one (%lu aged, %lu requests)",
              n_auto_aged - pp, n_arm[AK_PAGE] - pa);
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "... the pen leaves: no clean long after the turn (%lu cleans, %lu requests)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // ... or a gesture drops it
        COL_RESET();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        elapse(0.3);
        page_turn(0);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && cd_fail_id > cd_fail_seen, "leafing over colour pages, then a gesture");
        pa = n_arm[AK_PAGE];
        sd("gestureModeChanged: true");
        CHECK(!auto_req, "... the gesture begins: the request is dropped");
        elapse(0.5);
        sd("gestureModeChanged: false");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "... and no second page request after it (%lu cleans, %lu requests)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // not a request a full refresh has covered: a turn after that refresh still gets its own clean
        COL_RESET();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE, "a failed colour turn: its request waits");
        ggq("qml", "GGQ 5 rclean 0 0 1620 2160");
        c0 = cleans();
        page_turn(0);
        flush();
        CHECK(auto_req && t_our_full >= auto_t && cd_fail_id > cd_fail_seen, "our full clean covers it, then another colour turn fails");
        COL_TICKS(8);
        CHECK(!auto_req && cleans() == c0 + 1, "... the covered request is dropped, the new turn gets its own clean (%lu)", cleans() - c0);
        // a popup-shaped frame that took a turn's id itself counts as after that turn
        COL_RESET();
        pa = n_arm[AK_PAGE];
        sd("finalizeNewPage:");
        partial("ui ........", 300, 400, 1000, 1100);
        pend_t -= 0.005; // on the tablet the frame's time is taken before its scan, the hand-over comes after it
        flush();
        CHECK(cd_fail_id == cd_turn_id && cd_fail_id > cd_fail_seen, "a large popup right after finalizeNewPage took the turn's id and failed");
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "... no clean lands on it (%lu cleans, %lu requests)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // ink right after a failed colour turn, before its tick armed anything
        COL_RESET();
        page_turn(1);
        flush();
        elapse(0.1);
        pa = n_arm[AK_PAGE];
        partial("pen .......", 300, 1600, 200, 40);
        flush();
        COL_TICKS(10);
        CHECK(!auto_req && cleans() == c0 && n_arm[AK_PAGE] == pa, "ink right after a failed colour turn, before its tick: no request, no clean (%lu, %lu)",
              cleans() - c0, n_arm[AK_PAGE] - pa);
        // a menu and ink BEFORE the turn hold nothing back
        COL_RESET();
        host_cd_verdict = 2;
        partial("ui ........", 500, 400, 620, 1200);
        flush();
        elapse(1.0);
        partial("pen .......", 300, 1600, 200, 40);
        flush();
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
        c0 = cleans();
        page_turn(1);
        flush();
        ggq("qml", "GGQ 0 settle");
        CHECK(auto_req && auto_kind == AK_PAGE && auto_page_col == 1, "a menu and ink before a failed colour turn: its request is armed");
        COL_TICKS(4);
        CHECK(!auto_req && cleans() == c0 + 1, "... and it is cleaned (%lu)", cleans() - c0);
        #undef COL_RESET
        #undef COL_TICKS
        // a new request starts without the mark
        auto_page_col = 1;
        arm_auto(AK_PAGE, "page");
        CHECK(auto_req && !auto_page_col && !auto_page_id, "arm_auto clears the colour mark and the turn's id");
        auto_req = 0;
        ggq("qml", "GGQ 94 set turn_px_col -5");
        CHECK(cfg.turn_px_col == 0, "turn_px_col -5 -> %d", cfg.turn_px_col);
        ggq("qml", "GGQ 95 set turn_px_col 99999999");
        CHECK(cfg.turn_px_col == (int)NS, "turn_px_col 99999999 -> %d", cfg.turn_px_col);
        host_colour = sv_col; host_cd_verdict = 0;
        cfg.cd_std = sv.cd_std; cfg.turn_px = 6000; cfg.turn_px_col = sv.turn_px_col;
        cfg.w[CL_CONTENT] = sv_w;
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
    }

    printf("     set_param clamps\n");
    ggq("qml", "GGQ 90 set codrive 5");
    CHECK(cfg.codrive == 2, "codrive 5 -> %d", cfg.codrive);
    ggq("qml", "GGQ 91 set cd_max 13");
    CHECK(cfg.cd_max == 12, "cd_max 13 -> %d", cfg.cd_max);
    ggq("qml", "GGQ 92 set cd_max 3");
    CHECK(cfg.cd_max == 6, "cd_max 3 -> %d", cfg.cd_max);
    ggq("qml", "GGQ 93 set cd_max 30");
    CHECK(cfg.cd_max == 24, "cd_max 30 -> %d", cfg.cd_max);
    #undef CD_BASE
    #undef CD_ERASE
    #undef CD_DRAW
    cfg = sv;
    xo_state = sv_xo; host_rc = sv_rc; host_sw = sv_sw;
    auto_req = 0;
}

// 1.8.26 (turn_full 2): the full STD turn's second pass, the chroma gate, what makes the pass wait and what drops it
static void sp_turn(int *pg) { // a promoted turn: the ScreenDriver's line, 0.1 s later the frame as the complete refresh
    sd("finalizeNewPage:");
    elapse(0.1);
    *pg ^= 1;
    tf_full_turn(*pg);
}
static void test_second_pass(void) {
    printf("T49 1.8.26 turn_full 2: the full STD turn gets one in-place clean when the panel is idle; the chroma gate\n");
    Config sv = cfg;
    int sv_xo = xo_state;
    const char *sv_rc = host_rc;
    int sv_sw = host_sw;
    cfg.defer = 1; cfg.tr_quiet = 0.5; cfg.tr_full = 0.0; cfg.gesture_clean = 1;
    cfg.page_gap = 1; cfg.page_px = 80000; cfg.page_every = 6; cfg.page_flip = 2.0; cfg.page_settle = 1.5;
    cfg.turn_px = 6000; cfg.page_quiet = 0.2; cfg.auto_quiet = 0.4; cfg.stock_gap = 3.0;
    cfg.cd_gain = 1.0; cfg.cd_gpos = 0.7; cfg.cd_minv = 0.7; cfg.cd_area = 1000000;
    cfg.page_clean = 1; cfg.codrive = 1; cfg.turn_full = 2; cfg.tf_idle = 1; cfg.tf_chroma = 25; cfg.tf_chroma_min = 30;
    xo_state = 0; host_rc = "ok"; host_sw = 1;
    sd(" - reset");
    sd("visibleChanged: true");
    sd("penCloseChanged: false");
    sd_gesture = 0; sd_mono = 0;
    host_cd_verdict = 1;
    host_colour = 300; host_chroma = 100; host_px = 0;
    fill(0, 0, SW, SH, 255);
    text(112, 0, 1508, 2088);
    int pg = 0;
    tf_settle();

    printf("     a page with colour: the turn's frame is the full STD, ONE in-place clean follows once the panel is idle\n");
    unsigned long c0 = cleans(), f0 = n_force, k0 = n_turn_full_took, a0 = n_auto_clean, r0 = host_cd_n, s0 = n_second_pass;
    sp_turn(&pg);
    host_sw = 0; // the panel drives the turn's frame
    CHECK(n_force == f0 + 1 && n_turn_full_took == k0 + 1 && !promote_live, "the turn's frame went out as the full STD");
    CHECK(auto_req && auto_kind == AK_PAGE && auto_page_idle && auto_page_cap && auto_own_full > 0, "its second pass is armed with the frame");
    CHECK(auto_quiet_need == cfg.page_quiet, "a single turn: the pass needs page_quiet without updates (%.2f)", auto_quiet_need);
    flush();
    CHECK(auto_req && auto_page_idle && t_stock_full > 0 && t_stock_full <= auto_own_full,
          "the turn's own frame, finalized, does not drop its pass (%.6f)", auto_own_full - t_stock_full);
    CHECK(host_cd_n == r0, "nothing is handed to the helper (%lu)", host_cd_n - r0);
    // a later frame of the same turn (its pending tiles): the pass goes on waiting
    elapse(0.3);
    fill(112, 0, 1508, 1000, 255);
    partial("content ...", 112, 0, 1508, 2088);
    flush();
    CHECK(auto_req && auto_page_idle, "a later frame of the turn leaves the pass waiting");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req && auto_page_idle, "right after that frame: the pass waits");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "0.8 s after the turn: the panel is busy, the pass waits");
    host_sw = 1;
    elapse(0.4);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req && n_auto_clean == a0 + 1 && n_second_pass == s0 + 1,
          "1.2 s, the panel is idle: the second pass (%lu)", cleans() - c0);
    CHECK(host_last.x == 0 && host_last.y == 0 && host_last.w == SW && host_last.h == SH, "it is the whole screen");
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req && n_force == f0 + 1, "one flash and one second pass in all (%lu)", cleans() - c0);

    printf("     leafing: every turn is promoted, ONE second pass page_flip after the last turn\n");
    tf_settle();
    c0 = cleans(); f0 = n_force; k0 = n_turn_full_took; s0 = n_second_pass;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req && auto_page_idle, "turn 1: its pass waits for the panel");
    elapse(0.4); // the second turn 0.8 s after the first, the panel still busy with the first one's frame
    sp_turn(&pg);
    CHECK(n_force == f0 + 2 && n_turn_full_took == k0 + 2, "turn 2 is promoted although turn 1's pass waits (%lu %lu)", n_force - f0,
          n_turn_full_took - k0);
    CHECK(auto_req && auto_page_idle && auto_page_cap && auto_quiet_need == cfg.page_flip,
          "its frame takes the pass over, which now needs page_flip without updates (%.2f)", auto_quiet_need);
    flush();
    host_sw = 1;
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    elapse(0.8);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "1.3 s after the last turn: the panel is idle, but the user may turn on: no pass yet");
    elapse(0.8);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req && n_second_pass == s0 + 1, "2.1 s after the last turn: one second pass for the two turns (%lu)",
          cleans() - c0);

    printf("     the pen comes near before the pass: it is dropped (no flash under the pen) and does not come back\n");
    tf_settle();
    c0 = cleans();
    unsigned long i0 = n_auto_ink;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    sd("penCloseChanged: true");
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && cleans() == c0 && n_auto_ink == i0 + 1, "the pen near (the ScreenDriver's line): the pass is dropped");
    host_sw = 1;
    sd("penCloseChanged: false");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "the pen left: no pass after all (%lu)", cleans() - c0);
    tf_settle();
    c0 = cleans(); i0 = n_auto_ink;
    sp_turn(&pg);
    flush();
    elapse(1.2);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!auto_req && cleans() == c0 && n_auto_ink == i0 + 1, "the pen near (the clock's tick): dropped as well");

    printf("     the user writes before the pass: dropped\n");
    tf_settle();
    c0 = cleans(); i0 = n_auto_ink;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.5);
    partial("pen .......", 300, 1600, 200, 40);
    flush();
    host_sw = 1;
    elapse(1.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && cleans() == c0 && n_auto_ink == i0 + 1, "ink after the turn: no second pass (%lu)", cleans() - c0);

    printf("     a menu opens before the pass: dropped, no flash on a menu\n");
    tf_settle();
    c0 = cleans();
    unsigned long p0 = n_popup;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.5);
    partial("ui ........", 0, 0, 520, 2160); // the side menu, a third of the screen
    flush();
    CHECK(!auto_req && n_popup == p0 + 1, "the menu's frame drops the waiting pass");
    host_sw = 1;
    elapse(1.5);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "no clean on the menu (%lu)", cleans() - c0);

    printf("     the user turns on and that turn gets no full STD: the waiting pass is dropped (no flash after a flash-free turn)\n");
    tf_settle();
    c0 = cleans(); f0 = n_force; r0 = host_cd_n;
    unsigned long v0 = n_auto_covered;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    CHECK(auto_req && auto_page_idle, "the pass waits");
    host_colour = 0; // the page turned to holds no colour
    elapse(0.7);
    pg ^= 1;
    page_turn(pg);
    flush();
    CHECK(n_force == f0 + 1 && host_cd_n == r0 + 1, "the next turn is not promoted, it is handed to the helper (%lu %lu)", n_force - f0,
          host_cd_n - r0);
    CHECK(!auto_req && n_auto_covered == v0 + 1, "the waiting second pass is dropped at that turn's frame");
    host_sw = 1;
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "no clean after it: the helper confirmed that turn's co-drive (%lu)", cleans() - c0);
    host_colour = 300;

    printf("     the chroma gate: a scanned black-and-white page (balanced primaries) or a speck of colour gets no full STD\n");
    static const struct { int col, chroma; long px; int tf_chroma, promoted; const char *what; } G[] = {
        {12000, 6, 218700, 25, 0, "a scanned page (12000 colour samples, 6 % of them unbalanced)"},
        {300, 100, 218700, 25, 0, "a speck of colour (300 samples of 218700)"},
        {9000, 80, 218700, 25, 1, "a colour page (9000 samples, 80 % unbalanced)"},
        {700, 100, 218700, 25, 1, "just above the smallest part (657 samples)"},
        {650, 100, 218700, 25, 0, "just below the smallest part"},
        {12000, 24, 218700, 25, 0, "24 % unbalanced: below tf_chroma"},
        {12000, 25, 218700, 25, 1, "25 % unbalanced: at tf_chroma"},
        {12000, 6, 218700, 0, 1, "tf_chroma 0: any colour sample will do (1.8.22)"},
    };
    for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        tf_settle();
        host_colour = G[i].col; host_chroma = G[i].chroma; host_px = G[i].px; cfg.tf_chroma = G[i].tf_chroma;
        f0 = n_force; r0 = host_cd_n; c0 = cleans();
        pg ^= 1;
        if (G[i].promoted) {
            sd("finalizeNewPage:");
            CHECK(n_force == f0 + 1 && promote_live, "%s: promoted", G[i].what);
            elapse(0.1);
            tf_full_turn(pg);
            flush();
            CHECK(auto_req && auto_page_idle, "%s: the second pass is armed", G[i].what);
            elapse(1.2);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0 + 1 && !auto_req, "%s: and it goes out", G[i].what);
        } else {
            page_turn(pg);
            flush();
            CHECK(n_force == f0 && !promote_live && host_cd_n == r0 + 1, "%s: not promoted, handed to the helper", G[i].what);
            elapse(1.2);
            ggq("qml", "GGQ 0 settle");
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0 && !auto_req, "%s: no clean either (%lu)", G[i].what, cleans() - c0);
        }
    }
    host_colour = 300; host_chroma = 100; host_px = 0; cfg.tf_chroma = 25;
    ggq("qml", "GGQ 9 set tf_chroma 999");
    ggq("qml", "GGQ 9 set tf_chroma_min 99999");
    ggq("qml", "GGQ 9 set tf_idle 7");
    CHECK(cfg.tf_chroma == 100 && cfg.tf_chroma_min == 10000 && cfg.tf_idle == 1, "the keys are clamped above (%d %d %d)", cfg.tf_chroma,
          cfg.tf_chroma_min, cfg.tf_idle);
    ggq("qml", "GGQ 9 set tf_chroma -3");
    ggq("qml", "GGQ 9 set tf_chroma_min -1");
    ggq("qml", "GGQ 9 set tf_idle -1");
    CHECK(cfg.tf_chroma == 0 && cfg.tf_chroma_min == 0 && cfg.tf_idle == 0, "the keys are clamped below (%d %d %d)", cfg.tf_chroma,
          cfg.tf_chroma_min, cfg.tf_idle);
    cfg.tf_chroma = 25; cfg.tf_chroma_min = 30; cfg.tf_idle = 1;

    printf("     tf_idle 0: the pass is queued behind the turn's frame; no reading of the panel: 1.4 s after the last update\n");
    tf_settle();
    cfg.tf_idle = 0;
    c0 = cleans();
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.1);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "tf_idle 0: 0.1 s after the frame it waits page_quiet");
    elapse(0.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "tf_idle 0: 0.3 s after the frame it goes out although the panel is busy (%lu)", cleans() - c0);
    cfg.tf_idle = 1;
    tf_settle();
    c0 = cleans();
    sp_turn(&pg);
    host_sw = -1; // the SWTCON's state cannot be read
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "no reading: 1.2 s after the frame the pass waits");
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "no reading: 1.5 s after the frame it goes out (%lu)", cleans() - c0);
    host_sw = 1;

    printf("     xochitl's own full refresh after the turn covers the page: no second pass\n");
    tf_settle();
    c0 = cleans();
    unsigned long st0 = n_auto_stock;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.5);
    full();
    flush();
    host_sw = 1;
    elapse(1.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && cleans() == c0 && n_auto_stock == st0 + 1, "a stock full refresh after the turn's own: the pass is dropped (%lu)",
          cleans() - c0);

    printf("     the SWTCON answers busy: tried again at the next tick, still the second pass\n");
    tf_settle();
    c0 = cleans(); s0 = n_second_pass;
    sp_turn(&pg);
    flush();
    host_rc = "busy";
    elapse(1.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req && auto_kind == AK_PAGE && auto_page_idle && auto_page_cap && auto_own_full > 0,
          "busy: the request stays the second pass");
    host_rc = "ok";
    elapse(0.25);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req && n_second_pass == s0 + 1, "the next tick: the clean (%lu)", cleans() - c0);

    printf("     the panel stays busy: the pass keeps the long deadline, then it is given up\n");
    tf_settle();
    c0 = cleans();
    sp_turn(&pg);
    host_sw = 0;
    flush();
    for (int k = 0; k < 3; k++) {
        elapse(2.0);
        ggq("qml", "GGQ 0 settle");
    }
    CHECK(cleans() == c0 && auto_req && auto_page_idle, "6 s: it still waits (AUTO_MAX_AGE %.0f s is not its deadline)", AUTO_MAX_AGE);
    unsigned long g0 = n_auto_aged;
    elapse(2.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_auto_aged == g0 + 1, "8.5 s: given up (%lu)", cleans() - c0);
    host_sw = 1;

    printf("     the document is closed before the pass: dropped, no flash on another view\n");
    tf_settle();
    c0 = cleans(); v0 = n_auto_covered;
    sp_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    sd("visibleChanged: false");
    host_sw = 1;
    elapse(0.9);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && cleans() == c0 && n_auto_covered == v0 + 1, "the view closed: the pass is dropped (%lu)", cleans() - c0);

    host_colour = 0; host_chroma = 100; host_px = 0; host_cd_verdict = 0;
    auto_req = 0; tr_active = 0; promote_live = 0; kb_on = 0; turn_full_np = 0;
    sd("visibleChanged: false");
    cfg = sv; xo_state = sv_xo; host_rc = sv_rc; host_sw = sv_sw;
}

// 1.8.29 (turn_full 3): the white intermediate. The turn's frame goes out white as the full STD, the page follows
// as the one complete refresh after it.
static uint8_t *wh_want; // what the frame should hold once nothing is hidden: the true picture
static size_t wh_bytes(void) { return (size_t)SW * SH * 4; }
static long wh_sent_n(void) { // marker pixels in the frame
    const uint32_t *f = (const uint32_t *)(const void *)frame;
    long n = 0;
    for (size_t i = 0; i < (size_t)SW * SH; i++) n += f[i] == WH_SENT;
    return n;
}
static int wh_hidden(void) { // the whole frame is the marker and the true picture stands behind it
    return wh_sent_n() == (long)SW * SH && wh_shadow && !memcmp(wh_shadow, wh_want, wh_bytes());
}
static int wh_shown(void) { return wh_sent_n() == 0 && !memcmp(frame, wh_want, wh_bytes()); }
static void wh_page(int to2) { // xochitl paints the page: into the frame, and into the copy of what should be shown
    uint8_t *f = frame;
    for (int i = 0; i < 2; i++) {
        frame = i ? wh_want : f;
        fill(112, 0, 1508, 2088, 255);
        if (to2) text2(112, 0, 1508, 2088); else text(112, 0, 1508, 2088);
    }
    frame = f;
}
static void wh_band(int v) { // ... or a band of it
    uint8_t *f = frame;
    for (int i = 0; i < 2; i++) {
        frame = i ? wh_want : f;
        fill(112, 400, 1508, 300, v);
    }
    frame = f;
}
static void wh_turn(int *pg) { // a promoted turn: the ScreenDriver's line, 0.1 s later its frame as the complete refresh
    sd("finalizeNewPage:");
    elapse(0.1);
    *pg ^= 1;
    wh_page(*pg);
    full();
}
static void test_white_turn(void) {
    printf("T50 1.8.29 turn_full 3: the turn's frame goes out white, the page follows as the one complete refresh\n");
    Config sv = cfg;
    int sv_xo = xo_state;
    const char *sv_rc = host_rc;
    int sv_sw = host_sw;
    cfg.defer = 1; cfg.tr_quiet = 0.5; cfg.tr_full = 0.0; cfg.gesture_clean = 1;
    cfg.page_gap = 1; cfg.page_px = 80000; cfg.page_every = 6; cfg.page_flip = 2.0; cfg.page_settle = 1.5;
    cfg.turn_px = 6000; cfg.page_quiet = 0.2; cfg.auto_quiet = 0.4; cfg.stock_gap = 3.0;
    cfg.cd_gain = 1.0; cfg.cd_gpos = 0.7; cfg.cd_minv = 0.7; cfg.cd_area = 1000000;
    cfg.page_clean = 1; cfg.codrive = 1; cfg.turn_full = 3; cfg.tf_idle = 1; cfg.tf_chroma = 25; cfg.tf_chroma_min = 30;
    xo_state = 0; host_rc = "ok"; host_sw = 1;
    sd(" - reset");
    sd("visibleChanged: true");
    sd("penCloseChanged: false");
    sd_gesture = 0; sd_mono = 0;
    host_cd_verdict = 1;
    host_colour = 300; host_chroma = 100; host_px = 0;
    fill(0, 0, SW, SH, 255);
    text(112, 0, 1508, 2088);
    wh_want = malloc(wh_bytes());
    memcpy(wh_want, frame, wh_bytes());
    int pg = 0;
    tf_settle();
    CHECK(!wh_active && !wh_owed && !wh_off, "nothing hidden at the start");

    printf("     a page with colour: the frame goes out white as the full STD, the page follows once the panel is idle\n");
    unsigned long c0 = cleans(), f0 = n_force, k0 = n_turn_full_took, h0 = n_wh_hide, v0 = n_wh_reveal, s0 = n_second_pass;
    unsigned long w0 = n_whw_kick, r0 = host_cd_n, ab0 = n_wh_absorb;
    wh_turn(&pg);
    host_sw = 0; // the panel drives the white
    CHECK(n_force == f0 + 1 && n_turn_full_took == k0 + 1 && !promote_live, "the turn's frame went out as the full STD");
    CHECK(wh_active && wh_owed && n_wh_hide == h0 + 1 && wh_hidden(), "the frame is the marker all over, the page stands behind it (%ld)",
          wh_sent_n());
    CHECK(!auto_req && n_whw_kick == w0 + 1 && host_cd_n == r0, "no second pass armed, the watchdog asked once, nothing for the helper");
    flush();
    CHECK(wh_hidden() && !auto_req, "finalized: still hidden, nothing armed");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && wh_owed && wh_active, "right after the frame: the panel drives the white, the page waits");
    elapse(0.3); // a later frame of the same turn (its pending tiles) is drawn behind the white
    wh_band(200);
    partial("content ...", 112, 400, 1508, 300);
    flush();
    CHECK(wh_hidden() && n_wh_absorb == ab0 + 1, "a later frame of the turn joins the page behind the white (%ld)", wh_sent_n());
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && wh_owed, "0.9 s after the turn: the panel is busy, the page waits");
    host_sw = 1;
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && !wh_active && n_wh_reveal == v0 + 1, "1.2 s, the panel is idle: the page goes out (%lu)",
          cleans() - c0);
    CHECK(rect_full(host_last) && !last_clean_text && wh_shown(), "as the stock clean of the whole screen; no marker is left (%ld)",
          wh_sent_n());
    CHECK(wh_chk_n > 0 && wh_chk_bad == 0 && !wh_off, "the self-check saw the white (%ld %ld)", wh_chk_n, wh_chk_bad);
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req && n_force == f0 + 1 && n_second_pass == s0, "one white and one page in all, no second pass (%lu)",
          cleans() - c0);
    {
        ggq("qml", "GGQ 7 status");
        char *st = slurp(TESTRUN "/status.json");
        CHECK(st && strstr(st, "\"wh\":[0,0,0,") && strstr(st, "\"v\":\"ggsense 1.8.30\"}"), "the status file carries the counters");
    }

    printf("     leafing: a turn while the page is owed is not promoted, its frame joins the page behind the white\n");
    tf_settle();
    c0 = cleans(); f0 = n_force; h0 = n_wh_hide; v0 = n_wh_reveal; r0 = host_cd_n; ab0 = n_wh_absorb;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && wh_owed, "turn 1: the page waits for the panel");
    elapse(0.3);
    sd("finalizeNewPage:");
    CHECK(n_force == f0 + 1 && !promote_live, "turn 2, 0.7 s later: not promoted (%lu)", n_force - f0);
    elapse(0.1);
    pg ^= 1;
    wh_page(pg);
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), "
         "(940,2088 680x72)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=3, bounds=(112,0 1508x2160) - [(112,0 1508x2088), (112,2088 568x72), (940,2088 680x72)])");
    line(" - merged UI/Content");
    flush();
    CHECK(wh_hidden() && n_wh_hide == h0 + 1 && n_wh_absorb == ab0 + 1 && host_cd_n == r0,
          "its frame joins the page behind the white: no co-drive, no second white (%lu)", host_cd_n - r0);
    host_sw = 1;
    elapse(0.4);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && wh_shown() && n_wh_reveal == v0 + 1, "the panel is idle: ONE refresh shows the newest page (%lu)",
          cleans() - c0);
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "nothing follows it (%lu, %s)", cleans() - c0, auto_req ? auto_why : "-");

    printf("     a complete full-screen refresh of xochitl's own while the page is hidden brings the page itself\n");
    tf_settle();
    c0 = cleans(); v0 = n_wh_reveal;
    unsigned long st0 = n_wh_stock, e0 = n_wh_early;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    wh_band(200);
    full();
    CHECK(!wh_active && !wh_owed && wh_shown() && n_wh_stock == st0 + 1 && n_wh_early == e0 + 1, "the frame is the page again, nothing is owed");
    flush();
    host_sw = 1;
    elapse(1.2);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && n_wh_reveal == v0, "no refresh of ours follows (%lu)", cleans() - c0);

    printf("     a complete refresh of a part while hidden: the frame is the page again, its full-screen refresh still follows\n");
    tf_settle();
    c0 = cleans(); e0 = n_wh_early; v0 = n_wh_reveal; st0 = n_wh_stock;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    wh_band(128);
    line("swapBuffers: QFlags(0x1) QRegion(112,400 1508x300) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(112,400 1508x300)");
    CHECK(!wh_active && wh_owed && wh_shown() && n_wh_early == e0 + 1 && n_wh_stock == st0, "un-hidden, the refresh still owed");
    flush();
    host_sw = 1;
    elapse(0.8);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && rect_full(host_last) && n_wh_reveal == v0 + 1, "idle: the full-screen refresh of the page (%lu)",
          cleans() - c0);

    printf("     the way to sleep while hidden: the frame is the page again at once\n");
    tf_settle();
    c0 = cleans(); e0 = n_wh_early; st0 = n_wh_stock;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.4);
    host_wh_sleepmap = 1;
    wh_band(60);
    partial("content ...", 112, 400, 1508, 300);
    host_wh_sleepmap = 0;
    CHECK(!wh_active && wh_owed && wh_shown() && n_wh_early == e0 + 1, "a frame with a Sleep part: un-hidden, the refresh still owed");
    flush();
    wh_band(255);
    full(); // the wake's own complete refresh
    CHECK(!wh_owed && n_wh_stock == st0 + 1, "the next complete full-screen refresh of xochitl's own settles the debt");
    flush();
    host_sw = 1;
    elapse(1.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "no refresh of ours (%lu)", cleans() - c0);

    printf("     a clean of ours for another reason while the page is owed doubles as the page's arrival\n");
    tf_settle();
    c0 = cleans(); v0 = n_wh_reveal;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 5 rclean 200 300 400 500");
    CHECK(cleans() == c0 && wh_owed && wh_hidden(), "while the panel drives the white: refused, the page stays hidden");
    host_sw = 1;
    elapse(0.7);
    ggq("qml", "GGQ 5 rclean 200 300 400 500");
    CHECK(!wh_owed && !wh_active && wh_shown() && rect_full(host_last) && n_wh_reveal == v0 + 1,
          "idle: the box's clean became the page's full-screen refresh (%d,%d %dx%d)", host_last.x, host_last.y, host_last.w, host_last.h);
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");

    printf("     a BlinkLater byte in the way for two ticks: taken back, the page goes out\n");
    tf_settle();
    c0 = cleans();
    unsigned long wd0 = n_withdraw_host;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(1.1);
    host_sw = 1;
    host_rc = "pending"; host_wd_ok = 1;
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && wh_owed && n_withdraw_host == wd0, "the first tick waits for xochitl's frame");
    elapse(0.05);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && n_withdraw_host == wd0 + 1 && wh_shown(), "the second tick takes the byte back and sends the page");
    host_wd_ok = 0; host_rc = "ok";

    printf("     the panel never looks idle: WH_HOLD after the turn the page goes out all the same\n");
    tf_settle();
    c0 = cleans();
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(2.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && wh_owed && wh_hidden(), "2.5 s: still waiting");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && wh_shown() && rect_full(host_last), "3.1 s: sent over the busy panel (%lu)", cleans() - c0);
    host_sw = 1;

    printf("     the panel is not white after the turn: the page follows all the same, then no white intermediate any more\n");
    tf_settle();
    c0 = cleans();
    unsigned long b0 = n_wh_bad, no0 = n_wh_no;
    host_wh_state = 0;
    wh_turn(&pg);
    host_sw = 0;
    flush();
    elapse(1.1);
    host_sw = 1;
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !wh_owed && wh_shown() && wh_off && n_wh_bad == b0 + 1 && wh_chk_bad > 0, "the page went out, the mode is off (%ld of %ld)",
          wh_chk_bad, wh_chk_n);
    host_wh_state = 1;
    tf_settle();
    h0 = n_wh_hide;
    wh_turn(&pg);
    CHECK(!wh_active && !wh_owed && n_wh_hide == h0 && n_wh_no == no0 + 1 && wh_sent_n() == 0 && auto_req && auto_page_idle,
          "the next turn: the double pass of 1.8.26");
    flush();
    wh_off = 0;

    printf("     no live clock, or the SWTCON reads another image: no white, the double pass instead\n");
    tf_settle();
    no0 = n_wh_no; h0 = n_wh_hide;
    double sv_hb = hb_t;
    hb_t = now_s() - 100.0;
    wh_turn(&pg);
    CHECK(!wh_active && !wh_owed && n_wh_hide == h0 && n_wh_no == no0 + 1 && wh_sent_n() == 0, "the clock is not alive: the frame stays the page (%lu)",
          n_wh_no - no0);
    CHECK(auto_req && auto_kind == AK_PAGE && auto_page_idle, "the second pass is armed instead");
    hb_t = sv_hb;
    flush();
    tf_settle();
    host_wh_frame = 0;
    no0 = n_wh_no;
    wh_turn(&pg);
    CHECK(!wh_active && !wh_owed && n_wh_no == no0 + 1 && wh_sent_n() == 0 && auto_req && auto_page_idle, "another image: the same");
    host_wh_frame = 1;
    flush();

    printf("     set turn_full 2: the double pass of 1.8.26; a page without colour: neither\n");
    ggq("qml", "GGQ 9 set turn_full 2");
    tf_settle();
    h0 = n_wh_hide; no0 = n_wh_no;
    wh_turn(&pg);
    CHECK(cfg.turn_full == 2 && n_wh_hide == h0 && n_wh_no == no0 && wh_sent_n() == 0 && auto_req && auto_page_idle, "turn_full 2: no white, the second pass armed");
    flush();
    ggq("qml", "GGQ 9 set turn_full 3");
    CHECK(cfg.turn_full == 3, "set turn_full 3 switches it on again");
    host_colour = 0;
    tf_settle();
    f0 = n_force; h0 = n_wh_hide; r0 = host_cd_n;
    page_turn(pg ^= 1);
    flush();
    CHECK(n_force == f0 && n_wh_hide == h0 && wh_sent_n() == 0 && host_cd_n == r0 + 1, "a page without colour: no full STD, no white, the flash-free turn");
    host_colour = 300;

    host_colour = 0; host_chroma = 100; host_px = 0; host_cd_verdict = 0;
    auto_req = 0; tr_active = 0; promote_live = 0; kb_on = 0; turn_full_np = 0;
    sd("visibleChanged: false");
    cfg = sv; xo_state = sv_xo; host_rc = sv_rc; host_sw = sv_sw;
    free(wh_want);
    wh_want = NULL;
}

int main(void) {
    int dflt_cd_std = cfg.cd_std, dflt_turn_full = cfg.turn_full; // 1.8.22 / 1.8.23: the compiled defaults, checked below
    cfg.turn_full = 0; // 1.8.26: 2 by default; the tests before T48 are those of the flash-free turn, T48 / T49 set their own
    cfg.codrive = 0; // 1.8.13: on by default; the tests of the page clean are 1.8.6's, test_codrive sets its own
    setvbuf(stdout, NULL, _IONBF, 0);
    mkdir(TESTRUN, 0755);
    frame = malloc((size_t)SW * SH * 4);
    fill(0, 0, SW, SH, 255);
    fb = frame;
    fb_bpl = SW * 4;
    cfg.quiet_s = 0; // no wall-clock waiting in the tests
    // 1.8.4: every view change cleans the whole screen by default (see the header of ggsense-1.8.29.c); the 1.6-1.8.3
    // tests below keep the old regional threshold, T44 runs the default
    CHECK(cfg.tr_full == 0.0, "1.8.4 default: tr_full 0, every view change cleans the whole screen (%.2f)", cfg.tr_full);
    CHECK(dflt_cd_std == 1 && dflt_turn_full == 1 && cfg.cds_wt == 7 && cfg.wt_static == 1,
          "1.8.30 defaults: cd_std 1 (no clean per flash-free turn), turn_full 1, cds_wt 7, wt_static 1 (%d %d %d %d)", dflt_cd_std, dflt_turn_full, cfg.cds_wt, cfg.wt_static);
    CHECK(cfg.tf_chroma == 25 && cfg.tf_chroma_min == 30 && cfg.tf_idle == 1,
          "1.8.26 defaults: tf_chroma 25, tf_chroma_min 30, tf_idle 1 (%d %d %d)", cfg.tf_chroma, cfg.tf_chroma_min, cfg.tf_idle);
    cfg.tr_full = 0.35;

    printf("T1 first update primes the model\n");
    partial("ui ........", 0, 0, 16, 16);
    Score s = score();
    CHECK(prev_valid && s.ghost == 0, "primed, no ghosts (ghost=%ld)", s.ghost);

    printf("T2 drawing dark text on white is not a ghost\n");
    text(100, 100, 400, 40);
    partial("ui ........", 100, 100, 400, 40);
    s = score();
    show("drawn", s);
    CHECK(s.ghost == 0, "no ghost while text is shown");

    printf("T3 erasing it with a partial update leaves ghost samples where the text was\n");
    fill(100, 100, 400, 40, 255);
    partial("ui ........", 100, 100, 400, 40);
    s = score();
    show("erased", s);
    long expect = 0;
    for (int py = 50; py < 70; py++)
        for (int px = 50; px < 250; px++)
            if (px % 3 == 0 && py % 2 == 0) expect++;
    CHECK(s.ghost == expect, "ghost == text samples (%ld vs %ld)", s.ghost, expect);
    CHECK(s.hot > 0 && s.x0 <= 100 && s.y0 <= 100 && s.x1 >= 496 && s.y1 >= 136, "hot cells cover the text box");

    printf("T4 a full STD refresh clears everything\n");
    full();
    s = score();
    CHECK(s.ghost == 0 && n_full == 1, "ghost=%ld full=%lu", s.ghost, n_full);

    printf("T5 content waveform weight 0.75: one draw+erase is just over tau\n");
    cfg.w[CL_CONTENT] = 0.75;
    text(100, 300, 160, 32);
    partial("content ...", 100, 300, 160, 32);
    fill(100, 300, 160, 32, 255);
    partial("content ...", 100, 300, 160, 32);
    s = score();
    show("content", s);
    CHECK(s.ghost > 0, "content erase counts (ghost=%ld)", s.ghost);
    cfg.w[CL_CONTENT] = 0.5;
    full();
    text(100, 300, 160, 32);
    partial("content ...", 100, 300, 160, 32);
    fill(100, 300, 160, 32, 255);
    partial("content ...", 100, 300, 160, 32);
    s = score();
    CHECK(s.ghost == 0, "with weight 0.5 one draw+erase stays below tau (ghost=%ld)", s.ghost);
    full();

    printf("T6 large draw+erase gives 'now' with regional scope\n");
    text(200, 400, 1000, 600);
    partial("ui ........", 200, 400, 1000, 600);
    fill(200, 400, 1000, 600, 255);
    partial("ui ........", 200, 400, 1000, 600);
    pthread_mutex_lock(&mu);
    write_status("t6", "");
    pthread_mutex_unlock(&mu);
    char *js = slurp(TESTRUN "/status.json");
    printf("  %s\n", js ? js : "(no status)");
    CHECK(js && strstr(js, "\"rec\":\"now\"") && strstr(js, "\"scope\":\"regional\""), "rec now, scope regional");

    printf("T7 regional clean resets only that rectangle in the model\n");
    pthread_mutex_lock(&mu);
    model_clean_rect((Rect){200, 400, 500, 600});
    pthread_mutex_unlock(&mu);
    s = score();
    show("after left half clean", s);
    CHECK(s.ghost > 0 && s.x0 >= 696, "right half still dirty, left half clean (x0=%d)", s.x0);
    pthread_mutex_lock(&mu);
    model_clean_rect((Rect){700, 400, 500, 600});
    pthread_mutex_unlock(&mu);
    s = score();
    CHECK(s.ghost == 0, "all clean after second rect (ghost=%ld)", s.ghost);

    printf("T8 multi-rect QRegion parsing\n");
    text(0, 1200, 64, 64);
    text(1500, 1200, 64, 64);
    line("swapBuffers: QFlags() QRegion(size=2, bounds=(0,1200 1564x64) - [(0,1200 64x64), (1500,1200 64x64)]) "
         "EPFramebuffer::PresentationMode::Default");
    line(" - ui ........: QRegion(size=2, bounds=(0,1200 1564x64) - [(0,1200 64x64), (1500,1200 64x64)])");
    fill(0, 1200, 64, 64, 255);
    fill(1500, 1200, 64, 64, 255);
    line("swapBuffers: QFlags() QRegion(size=2, bounds=(0,1200 1564x64) - [(0,1200 64x64), (1500,1200 64x64)]) "
         "EPFramebuffer::PresentationMode::Default");
    s = score();
    show("two boxes", s);
    CHECK(s.ghost > 0 && s.x0 == 0 && s.x1 >= 1564, "both boxes seen (x0=%d x1=%d)", s.x0, s.x1);
    full();

    printf("T9 cells that are still changing are excluded (animations)\n");
    cfg.quiet_s = 30;
    text(1500, 40, 64, 32);
    partial("ui ........", 1500, 40, 64, 32);
    fill(1500, 40, 64, 32, 255);
    partial("ui ........", 1500, 40, 64, 32);
    s = score();
    show("active", s);
    CHECK(s.ghost == 0 && s.ghost_active > 0, "counted as active, not as ghost");
    cfg.quiet_s = 0;
    s = score();
    CHECK(s.ghost > 0, "counted once quiet (ghost=%ld)", s.ghost);
    full();

    printf("T10 pen strokes then erase\n");
    text(300, 1600, 300, 30);
    partial("pen (ui) ..", 300, 1600, 300, 30);
    fill(300, 1600, 300, 30, 255);
    partial("pen (ui) ..", 300, 1600, 300, 30);
    s = score();
    CHECK(s.ghost > 0, "erased strokes are ghosts (ghost=%ld)", s.ghost);
    full();

    printf("T11 command channel through the message handler\n");
    ggq("qml", "GGQ 41 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"q\":\"41\""), "status answered with q=41");
    ggq("qml", "GGQ 42 set soon_px 1234");
    CHECK(cfg.soon_px == 1234, "set soon_px");
    ggq("qml", "GGQ 43 set w_content 0.6");
    CHECK(cfg.w[CL_CONTENT] > 0.59 && cfg.w[CL_CONTENT] < 0.61, "set w_content");
    ggq("qml", "GGQ 44 rclean 10 10 100 100");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"q\":\"44\"") && strstr(js, "\"rc\":\"unsupported\""), "rclean reports unsupported off-device");
    ggq("qml", "GGQ 45 map");
    struct stat st;
    CHECK(stat(TESTRUN "/ghostmap.pgm", &st) == 0 && st.st_size > NCELL, "ghost map written (%lld bytes)", (long long)st.st_size);
    ggq("rm.framebuffer.updates", "swapBuffers: QFlags() QRegion(5,5 20x20) EPFramebuffer::PresentationMode::Default");
    CHECK(pend_active == 1, "update line through the handler opens an update");
    ggq("qml", "GGQ 46 reset");
    CHECK(pend_active == 0, "status/reset finalizes it");
    {
        // 1.8.7: the test commands are off at start, check their arguments, and never touch the model
        const char *rc0 = host_rc;
        int n0 = host_test_n;
        ggq("qml", "GGQ 48 bleach");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"q\":\"48\"") && strstr(js, "\"rc\":\"off\"") && host_test_n == n0, "bleach refused while testcmd is 0");
        ggq("qml", "GGQ 49 send 8 17 0 0 0 1620 2160");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"off\"") && host_test_n == n0, "send refused while testcmd is 0");
        ggq("qml", "GGQ 50 set testcmd 1");
        CHECK(cfg.testcmd == 1, "set testcmd");
        ggq("qml", "GGQ 51 bleach");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"unsupported\"") && host_test_n == n0, "bleach reports unsupported off-device");
        host_rc = "ok";
        ggq("qml", "GGQ 52 bleach");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"ok\"") && host_test_n == n0 + 1 && host_test_last[0] == 1, "bleach sent");
        ggq("qml", "GGQ 53 send 8 17 0 0 0 1620 2160");
        CHECK(host_test_n == n0 + 2 && host_test_last[0] == 0 && host_test_last[1] == 8 && host_test_last[2] == 17 &&
              host_test_last[3] == 0 && host_last.w == 1620 && host_last.h == 2160, "send wf 8 sent");
        ggq("qml", "GGQ 54 send 2 14 2 100 200 3000 4000");
        CHECK(host_test_n == n0 + 3 && host_last.x == 100 && host_last.y == 200 && host_last.w == 1520 && host_last.h == 1960,
              "send is clipped to the screen (%d,%d %dx%d)", host_last.x, host_last.y, host_last.w, host_last.h);
        static const char *bad[] = {"send 0 17 0 0 0 1620 2160", "send 10 17 0 0 0 1620 2160", "send 8 17 1 0 0 1620 2160",
                                    "send 9 17 2 0 0 1620 2160", "send 2 5 0 0 0 1620 2160",   "send 2 17 16 0 0 1620 2160",
                                    "send 2 17 -1 0 0 1620 2160", "send 2 17 0 0 0 1620",      "send", "send a b c d e f g",
                                    "send -1 17 0 0 0 1620 2160"};
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            char line[96];
            snprintf(line, sizeof line, "GGQ 55 %s", bad[i]);
            ggq("qml", line);
            js = slurp(TESTRUN "/status.json");
            CHECK(js && strstr(js, "\"rc\":\"badargs\"") && host_test_n == n0 + 3, "refused: %s", bad[i]);
        }
        ggq("qml", "GGQ 56 send 2 17 0 5000 5000 10 10");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"empty\"") && host_test_n == n0 + 3, "send outside the screen is empty");
        host_sw = 0;
        ggq("qml", "GGQ 57 bleach");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"inflight\"") && host_test_n == n0 + 3, "bleach waits for an idle panel");
        host_sw = 1;
        sleeping = 1;
        ggq("qml", "GGQ 58 bleach");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"rc\":\"asleep\"") && host_test_n == n0 + 3, "bleach refused while asleep");
        sleeping = 0;
        ggq("qml", "GGQ 59 set testcmd 0");
        ggq("qml", "GGQ 59 bleach");
        CHECK(cfg.testcmd == 0 && host_test_n == n0 + 3, "testcmd 0 turns them off again");
        host_rc = rc0;
    }
    ggq("qml", "GG not a command");
    ggq("qml", "GGQ");
    ggq("qml", "GGQ 47");
    printf("  malformed commands ignored\n");

    printf("T12 robustness: odd regions\n");
    line("swapBuffers: QFlags() QRegion(1600,2150 100x100) EPFramebuffer::PresentationMode::Default");
    line("swapBuffers: QFlags() QRegion(-10,-10 30x30) EPFramebuffer::PresentationMode::Default");
    line("swapBuffers: QFlags() QRegion(0,0 0x0) EPFramebuffer::PresentationMode::Default");
    line("swapBuffers: QFlags() QRegion() EPFramebuffer::PresentationMode::Default");
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(0,0 1x1) - [(");
    line(" - ui ........: QRegion(size=999, bounds=(0,0 1620x2160) - [(0,0 1x1)");
    line("pmode!");
    line(" - merged UI/Content");
    line(" - sleep");
    s = score();
    printf("  survived (ghost=%ld)\n", s.ghost);

    printf("T14 shadow 'sim' forgets a rectangle without a refresh\n");
    full();
    text(200, 400, 400, 200);
    partial("ui ........", 200, 400, 400, 200);
    fill(200, 400, 400, 200, 255);
    partial("ui ........", 200, 400, 400, 200);
    s = score();
    long before = s.ghost;
    unsigned long nstd = n_std;
    ggq("qml", "GGQ 60 sim 200 400 200 200");
    s = score();
    CHECK(before > 0 && s.ghost > 0 && s.ghost < before && n_std == nstd, "half forgotten (%ld -> %ld), no refresh counted",
          before, s.ghost);
    ggq("qml", "GGQ 61 sim 0 0 1620 2160");
    s = score();
    js = slurp(TESTRUN "/status.json");
    CHECK(s.ghost == 0 && js && strstr(js, "\"sim\":2"), "all forgotten, sim=2");
    CHECK(js && strstr(js, "\"chg\":["), "per-class change counts: %.40s", js ? strstr(js, "\"chg\"") : "");

    printf("T15 debug line shows what a complete refresh removed (expect rec=now below)\n");
    cfg.debug = 1;
    text(0, 0, 800, 800);
    partial("ui ........", 0, 0, 800, 800);
    fill(0, 0, 800, 800, 255);
    partial("ui ........", 0, 0, 800, 800);
    full();
    score();
    cfg.debug = 0;
    fflush(stderr);

    printf("T16 a full STD that also changes the content keeps the old image; an in-place STD clears it\n");
    full();
    text(100, 1700, 400, 40);
    partial("ui ........", 100, 1700, 400, 40);
    fill(100, 1700, 400, 40, 255); // the content changes ...
    unsigned long chg0 = n_std_chg;
    full();                        // ... inside the same full STD (Settings open/close)
    s = score();
    show("transition STD", s);
    CHECK(s.ghost > 0 && n_std_chg > chg0, "old text stays as ghost (ghost=%ld, changed samples=%lu)", s.ghost,
          n_std_chg - chg0);
    full(); // content settled: in-place STD
    s = score();
    CHECK(s.ghost == 0, "in-place full STD clears it (ghost=%ld)", s.ghost);

    printf("T17 only /usr/bin/xochitl is observed; elsewhere the library is a pass-through\n");
    CHECK(exe_is_xochitl("/usr/bin/xochitl") && !exe_is_xochitl("/usr/bin/xochitl_pdf_renderer") &&
              !exe_is_xochitl("") && !exe_is_xochitl(NULL),
          "exe filter");
    gg_off = 1;
    partial("ui ........", 0, 0, 64, 64);
    CHECK(pend_active == 0, "nothing parsed while off");
    gg_off = 0;

    printf("T18 view change: deferred only with a live QML clock; the redrawn area is collected; one clean\n");
    full();
    xo_state = 0;
    cfg.defer = 1;
    unsigned long k0 = n_kick, tr0 = n_tr, f0 = n_tr_fail;
    const char *vc_open = "viewChange xofm::libs::ghostbuster::IGhostBuster::ViewChangeMode::SettingsOpened";
    ggq("xofm.libs.ghostbuster", vc_open);
    CHECK(n_tr == tr0 && !tr_active && n_kick == k0, "no heartbeat yet: stock behaviour kept");
    ggq("qml", "GGQ 0 hb");
    ggq("xofm.libs.ghostbuster", vc_open);
    CHECK(tr_active && n_kick == k0 + 1 && !strcmp(tr_what, "SettingsOpened"), "armed (%s), helper asked to withdraw", tr_what);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active, "nothing redrawn yet: keeps waiting");
    text(300, 500, 600, 300);
    partial("ui ........", 300, 500, 600, 300);
    partial("ui ........", 1400, 2000, 100, 100);
    CHECK(tr_box.x == 300 && tr_box.y == 500 && tr_box.w == 1200 && tr_box.h == 1600, "redrawn area %d,%d %dx%d",
          tr_box.x, tr_box.y, tr_box.w, tr_box.h);
    cfg.tr_quiet = 30;
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active, "screen changed too recently: waits");
    cfg.tr_quiet = 0;
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_fail == f0 + 1, "settled: one clean tried (unsupported off-device), nothing pending");
    ggq("qml", "GGQ 0 settle");
    CHECK(n_tr_fail == f0 + 1, "a later settle does nothing");
    printf("     a stock full refresh that still went out is noticed and widens the area\n");
    unsigned long l0 = n_wd_late;
    ggq("xofm.libs.ghostbuster", "viewChange xofm::libs::ghostbuster::IGhostBuster::ViewChangeMode::SettingsClosed");
    full();
    CHECK(n_wd_late == l0 + 1 && tr_box.w == SW && tr_box.h == SH, "late stock flash seen (late=%lu)", n_wd_late - l0);
    ggq("qml", "GGQ 0 settle");
    pthread_mutex_lock(&mu);
    write_status("t18", "");
    pthread_mutex_unlock(&mu);
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"defer\":1") && strstr(js, "\"trActive\":0"), "status: %.60s", js ? strstr(js, "\"defer\"") : "");
    cfg.defer = 0;
    hb_t = 0;
    xo_state = -1;
    full();

    printf("T19 rectangles from text never wrap\n");
    Rect ra = {2147483647, 0, 10, 10}, rb = {-5, -5, 2147483647, 20}, rc2 = {100, 100, -50, 10};
    CHECK(!clip_rect(&ra), "x near INT_MAX rejected");
    CHECK(clip_rect(&rb) && rb.x == 0 && rb.y == 0 && rb.w == SW && rb.h == 15, "huge width clipped (%d,%d %dx%d)", rb.x,
          rb.y, rb.w, rb.h);
    CHECK(!clip_rect(&rc2), "negative width rejected");
    ggq("qml", "GGQ 70 rclean 2147483647 0 10 10");
    ggq("qml", "GGQ 71 sim 2147483000 2147483000 2000 2000");
    line("swapBuffers: QFlags() QRegion(2147483000,5 2000x20) EPFramebuffer::PresentationMode::Default");
    s = score();
    printf("  survived (ghost=%ld)\n", s.ghost);

    // the 1.5.1 rules (deferred view-change flash)
    full();
    xo_state = 0;
    cfg.defer = 1;
    cfg.tr_quiet = 0;
    ggq("qml", "GGQ 0 hb");

    printf("T20 a second view change joins the cycle; the clean waits for its first frame too\n");
    unsigned long f1 = n_tr_fail;
    vc("SettingsOpened");
    text(300, 500, 400, 200);
    partial("ui ........", 300, 500, 400, 200); // Settings drawn
    vc("SettingsClosed");                       // closed again before the clean
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_fail == f1, "waits for the closing view's first frame");
    fill(300, 500, 400, 200, 255);
    partial("ui ........", 0, 0, 1620, 400); // the main screen is drawn
    Rect jb = tr_box;
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_fail == f1 + 1 && jb.x == 0 && jb.y == 0 && jb.w == SW && jb.h == 700,
          "then one clean over both views (%d,%d %dx%d)", jb.x, jb.y, jb.w, jb.h);
    vc("SettingsOpened");
    partial("ui ........", 300, 500, 400, 200);
    vc("SettingsClosed");
    elapse(TR_FRAME_MAX + 0.5); // its frame never comes
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_fail == f1 + 2, "a joining view change that never draws does not block the clean");

    printf("T21 a later stock in-place flash stands in for the clean; one that changes the area does not\n");
    full();
    unsigned long a0 = n_tr_absorbed, f2 = n_tr_fail;
    vc("DocumentViewDialogClosed");
    text(200, 300, 800, 600);
    partial("ui ........", 200, 300, 800, 600); // the view is drawn
    full();                                     // "clear on next update": same content, in place
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_absorbed == a0 + 1 && n_tr_fail == f2, "absorbed, no second flash");
    vc("DocumentViewDialogClosed");
    fill(200, 300, 800, 600, 255);
    partial("ui ........", 200, 300, 800, 600);
    text(200, 300, 800, 600); // this stock flash changes the area again
    full();
    ggq("qml", "GGQ 0 settle");
    CHECK(n_tr_absorbed == a0 + 1 && n_tr_fail == f2 + 1, "content-changing stock flash: the clean still follows");

    printf("T22 defer off mid-cycle: a withdrawn flash goes back only while its view has not drawn\n");
    full();
    unsigned long r0 = n_restore;
    vc("SettingsOpened");
    tr_withdrawn = 1; // the helper took the stock flash
    ggq("qml", "GGQ 82 set defer 0");
    CHECK(!tr_active && cfg.defer == 0 && n_restore == r0 + 1, "no frame yet: flash given back to xochitl");
    ggq("qml", "GGQ 81 set defer 1");
    vc("SettingsOpened");
    tr_withdrawn = 1;
    partial("ui ........", 300, 500, 400, 200);
    ggq("qml", "GGQ 82 set defer 0");
    CHECK(!tr_active && n_restore == r0 + 1, "already drawn: the cycle just ends");
    ggq("qml", "GGQ 81 set defer 1");
    cfg.tr_quiet = 0;

    printf("T23 a view change that draws nothing needs no clean; no deferral without update lines\n");
    unsigned long g0 = n_tr_aged, f3 = n_tr_fail;
    vc("SettingsOpened");
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active, "waits for its first frame");
    elapse(TR_FRAME_MAX + 0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_aged == g0 + 1 && n_tr_fail == f3, "nothing drawn: dropped at the next tick");
    vc("SettingsOpened");
    elapse(TR_FRAME_MAX + 0.5);
    partial("ui ........", 0, 2000, 300, 100); // an unrelated update much later
    CHECK(!tr_active && n_tr_aged == g0 + 2 && tr_box.w == 0, "a much later frame is not taken for it");
    unsigned long nu = n_updates;
    n_updates = 0;
    vc("SettingsOpened");
    CHECK(!tr_active, "no update lines seen yet: stock flash kept");
    n_updates = nu;
    vc("SettingsOpened");
    partial("ui ........", 300, 500, 400, 200);
    elapse(TR_MAX_AGE + 1);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_aged == g0 + 3 && n_tr_fail == f3, "a cycle older than %.0f s is dropped", TR_MAX_AGE);

    printf("T24 writing on the new view holds the clean until a pause with the pen away\n");
    unsigned long f4 = n_tr_fail;
    vc("DocumentViewDialogClosed");
    text(0, 0, 1620, 1500);
    partial("ui ........", 0, 0, 1620, 1500);
    partial("pen (ui) ..", 600, 420, 420, 10); // a pen tap on the UI is not writing
    CHECK(tr_active && !tr_inked, "pen (ui) is not ink");
    text(300, 1600, 200, 40);
    partial("pen .......", 300, 1600, 200, 40); // a stroke
    CHECK(tr_inked, "a stroke is ink");
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_fail == f4, "just written: waits (needs %.1f s without updates)", TR_INK_QUIET);
    elapse(TR_INK_QUIET + 0.1);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(tr_active && n_tr_fail == f4, "pen still near the screen: waits");
    ggq("qml", "GGQ 0 hb pen");
    CHECK(tr_active && n_tr_fail == f4, "heartbeat with the pen near: waits");
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_fail == f4 + 1, "pause with the pen away: one clean");
    vc("SettingsClosed");
    partial("ui ........", 300, 500, 400, 200);
    elapse(1.0);
    ggq("qml", "GGQ 0 hb");
    CHECK(tr_active, "heartbeat: needs %.0f s without updates", TR_HB_QUIET);
    elapse(TR_HB_QUIET);
    ggq("qml", "GGQ 0 hb");
    CHECK(!tr_active && n_tr_fail == f4 + 2, "the heartbeat finishes a clean the settle clock missed");

    printf("T25 another stock flash pending at clean time goes out first; the clean follows or is absorbed\n");
    full(); // in place, but it went out BEFORE the view change (applied only at the next update line)
    unsigned long c0 = n_tr_clean, a1 = n_tr_absorbed;
    vc("SettingsClosed");
    text(300, 500, 400, 200);
    partial("ui ........", 300, 500, 400, 200);
    CHECK(tr_active && n_tr_absorbed == a1, "a full refresh from before the view change is not taken for it");
    host_rc = "pending";
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && tr_box.w > 0, "pending stock flash: the cycle is kept");
    host_rc = "ok";
    full(); // that frame goes out in place
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_absorbed == a1 + 1 && n_tr_clean == c0, "it went over the area in place: absorbed");
    vc("SettingsClosed");
    fill(300, 500, 400, 200, 255);
    partial("ui ........", 300, 500, 400, 200);
    host_rc = "busy";
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active, "framebuffer busy: tried again later");
    host_rc = "ok";
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == c0 + 1, "then cleaned");
    host_rc = "unsupported";

    printf("T26 parameter limits\n");
    ggq("qml", "GGQ 83 set tr_quiet 5");
    double q1 = cfg.tr_quiet;
    ggq("qml", "GGQ 84 set tr_quiet 0");
    double q2 = cfg.tr_quiet;
    ggq("qml", "GGQ 85 set tr_quiet nan");
    CHECK(q1 == 1.0 && q2 == 0.1 && cfg.tr_quiet == 0.1, "tr_quiet kept within 0.1..1 s (%.2f %.2f %.2f)", q1, q2,
          cfg.tr_quiet);
    ggq("qml", "GGQ 86 set tr_full -3");
    CHECK(cfg.tr_full == 0.0, "tr_full -3: refused, stays 0 (%.2f)", cfg.tr_full);
    cfg.tr_full = 0.6;
    ggq("qml", "GGQ 86 set tr_full 0.35");
    CHECK(cfg.tr_full == 0.0, "1.8.4: tr_full 0.35 from text refused (the whole screen only; %.2f)", cfg.tr_full);
    ggq("qml", "GGQ 86 set tr_full nan");
    CHECK(cfg.tr_full == 0.0, "tr_full nan: ignored (%.2f)", cfg.tr_full);
    pthread_mutex_lock(&mu);
    write_status("t26", "");
    pthread_mutex_unlock(&mu);
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"trAbsorbed\":") && strstr(js, "\"v\":\"" GG_VERSION "\""), "status: %.48s",
          js ? strstr(js, "\"trAbsorbed\"") : "");
    cfg.defer = 0;
    hb_t = 0;
    xo_state = -1;
    cfg.tr_full = 0.6;

    printf("T27 1.6 need-based clean after a page turn (auto; 1.8: always the whole screen)\n");
    cfg.stock_gap = 0; // the 1.6 tests do not wait after a stock flash
    full();
    xo_state = 0;
    cfg.defer = 1;
    cfg.tr_quiet = 0;
    cfg.quiet_s = 2.0;
    cfg.w[CL_CONTENT] = 1.2;
    cfg.w_std = 1.2;
    ggq("qml", "GGQ 0 hb");
    host_rc = "ok";
    page_turn(0);
    full(); // the document opens: tiles first, then the stock flash in place
    elapse(1.0);
    unsigned long au0 = n_auto, auc0 = n_auto_clean, aun0 = n_auto_none;
    page_turn(1);
    ggq("qml", "GGQ 90 auto doc");
    CHECK(auto_req && n_auto == au0 + 1 && n_auto_clean == auc0, "just turned: the request waits for %.1f s of quiet",
          cfg.auto_quiet);
    elapse(0.5);
    pthread_mutex_lock(&mu);
    Score sa = compute_score();
    pthread_mutex_unlock(&mu);
    CHECK(sa.ghost == 0 && sa.ghost_active > 0, "the general score still counts the page as changing (quiet_s %.1f)",
          cfg.quiet_s);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && n_auto_clean == auc0 + 1, "settled: one in-place clean");
    s = score();
    CHECK(s.ghost == 0 && s.ghost_active == 0, "the model is clean afterwards (ghost=%ld active=%ld)", s.ghost, s.ghost_active);
    page_turn(0);
    ggq("qml", "GGQ 91 auto doc");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    pthread_mutex_lock(&mu);
    write_status("t27", "");
    pthread_mutex_unlock(&mu);
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"auto\":[2,2,0,0,0]") && strstr(js, "\"cleanWf\":2"), "status: %.40s",
          js ? strstr(js, "\"auto\"") : "");
    printf("     a page that only adds dark content (blank -> text) leaves nothing to clean\n");
    fill(112, 0, 1508, 2088, 255);
    partial("content ...", 112, 0, 1508, 2088);
    full();
    elapse(1.0);
    page_turn(1);
    ggq("qml", "GGQ 92 auto doc");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(n_auto_none == aun0 + 1 && n_auto_clean == auc0 + 2, "no clean (none=%lu)", n_auto_none - aun0);

    printf("T28 the auto request waits for a view change, the pen and the screen; it ages out; busy retries\n");
    unsigned long ag0 = n_auto_aged, auc1 = n_auto_clean;
    page_turn(0);
    ggq("qml", "GGQ 93 auto doc pen");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(auto_req && n_auto_clean == auc1, "pen near: waits");
    vc("DocumentViewDialogOpened");
    ggq("qml", "GGQ 0 settle");
    CHECK(auto_req && tr_active, "a view change waits for its own clean: the request waits");
    partial("ui ........", 300, 300, 400, 400); // a small dialog (below the 5 % of a menu or popup, see T39)
    elapse(0.5);
    host_rc = "busy";
    ggq("qml", "GGQ 0 settle"); // the view change's clean: busy -> re-armed
    CHECK(tr_active && auto_req, "cycle re-armed on busy, request still waits");
    host_rc = "ok";
    ggq("qml", "GGQ 0 settle"); // the cycle cleans
    CHECK(!tr_active && auto_req, "the view change cleaned first");
    host_rc = "busy";
    ggq("qml", "GGQ 0 settle"); // now the request: busy -> kept
    CHECK(auto_req, "busy: the request is kept");
    host_rc = "ok";
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req, "then decided");
    page_turn(1);
    ggq("qml", "GGQ 94 auto doc");
    elapse(AUTO_MAX_AGE + 0.5);
    partial("ui ........", 1400, 40, 100, 30); // the screen kept changing until the deadline
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && n_auto_aged == ag0 + 1, "a request older than %.0f s is dropped", AUTO_MAX_AGE);
    ggq("qml", "GGQ 95 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"autoReq\":0"), "nothing left waiting");

    printf("T29 (review F1) every view change starts its own ink state; a view being written on is kept longer\n");
    full();
    unsigned long g1 = n_tr_aged, c1 = n_tr_clean;
    vc("DocumentViewDialogClosed");
    text(0, 0, 1620, 1000);
    partial("ui ........", 0, 0, 1620, 1000);
    text(300, 1600, 200, 40);
    partial("pen .......", 300, 1600, 200, 40);
    CHECK(tr_inked, "ink on the new view");
    vc("DocumentViewDialogOpened");
    CHECK(tr_active && !tr_inked, "the next view change starts without ink");
    partial("ui ........", 0, 0, 800, 800);
    text(300, 1700, 200, 40);
    partial("pen .......", 300, 1700, 200, 40);
    elapse(TR_MAX_AGE + 5);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(tr_active && n_tr_aged == g1, "written on for %.0f s with the pen near: still waits", TR_MAX_AGE + 5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == c1 + 1, "pen away: cleaned");
    vc("DocumentViewDialogClosed");
    partial("ui ........", 0, 0, 800, 800);
    partial("pen .......", 300, 1700, 200, 40);
    elapse(TR_INK_MAX_AGE + 1);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!tr_active && n_tr_aged == g1 + 1, "after %.0f s it is dropped", TR_INK_MAX_AGE);

    printf("T30 (review F2) with the pen near, the clean also waits %.1f s after the last update\n", TR_INK_QUIET);
    unsigned long c2 = n_tr_clean;
    vc("SettingsOpened");
    partial("ui ........", 300, 500, 400, 200);
    elapse(0.6);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(tr_active && n_tr_clean == c2, "0.6 s after the frame, pen near: waits");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!tr_active && n_tr_clean == c2 + 1, "1.6 s, no ink: cleans");

    printf("T31 (review F3) defer off: the flash goes back when the NEWEST view has not drawn yet\n");
    unsigned long r1 = n_restore;
    vc("SettingsOpened");
    tr_withdrawn = 1;
    partial("ui ........", 300, 500, 400, 200);
    vc("SettingsClosed"); // joins; its frame has not come
    ggq("qml", "GGQ 82 set defer 0");
    CHECK(!tr_active && n_restore == r1 + 1, "given back (restore=%lu)", n_restore - r1);
    ggq("qml", "GGQ 81 set defer 1");

    printf("T32 parameters from text never overflow\n");
    ggq("qml", "GGQ 96 set tr_pad 1e30");
    ggq("qml", "GGQ 96 set cell_min -1e30");
    ggq("qml", "GGQ 96 set auto_pad inf");
    ggq("qml", "GGQ 96 set soon_px 9e99");
    ggq("qml", "GGQ 96 set debug 1e300");
    ggq("qml", "GGQ 96 set clean_wf 99");
    ggq("qml", "GGQ 96 set auto_quiet 0");
    CHECK(cfg.tr_pad == 256 && cfg.cell_min == 1 && cfg.auto_pad == 64 && cfg.soon_px == (int)NS && cfg.debug == 1 &&
              cfg.clean_wf == 2 && cfg.auto_quiet == 0.2,
          "clamped (pad %d cell %d apad %d soon %d dbg %d wf %d aq %.1f)", cfg.tr_pad, cfg.cell_min, cfg.auto_pad,
          cfg.soon_px, cfg.debug, cfg.clean_wf, cfg.auto_quiet);

    {
    printf("T33 1.8 document close: the stock flash is withdrawn, one full in-place clean after the library settled\n");
    cfg.stock_gap = 3.0;
    cfg.defer = 1;
    cfg.tr_quiet = 0.5;
    cfg.sd_close = 1;
    cfg.gesture_clean = 1;
    cfg.page_clean = 1;
    // T33-T44 test the 1.8-1.8.4 behaviour (1.8.5 defaults: T45): page_every 1 makes every page change due without
    // the model, as 1.8.4 promoted every gesture redraw; none of T33-T44 needs the model to refuse a page clean
    cfg.page_gap = 1;
    cfg.page_every = 1;
    cfg.page_px = cfg.now_px < cfg.soon_px ? cfg.now_px : cfg.soon_px;
    cfg.turn_px = cfg.page_px; // 1.8.6: page turns had page_px and the auto_quiet timing then
    cfg.page_quiet = cfg.auto_quiet;
    cfg.kb_clean = 1;
    xo_state = 0;
    host_rc = "ok";
    ggq("qml", "GGQ 0 hb");
    full();
    elapse(5.0);
    sd("visibleChanged: true");
    CHECK(sd_visible == 1, "document view visible");
    unsigned long k0 = n_kick, c0 = cleans(), tc0 = n_tr_clean;
    sd("pendingTilesChanged: true");
    sd("visibleChanged: false");
    sd(" - clearing on next update");
    sd(" - reset");
    CHECK(tr_active && tr_sd && n_kick == k0 + 1 && !strcmp(tr_what, "DocumentClosed") && !sd_visible && !sd_tiles,
          "a DocumentClosed cycle started and the withdraw helper was kicked");
    fill(0, 0, SW, SH, 255);
    text(0, 300, SW, 900);
    partial("ui ........", 0, 0, SW, SH); // the library, drawn by its normal update (no flash)
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && cleans() == c0, "not yet: the screen just changed");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc0 + 1 && cleans() == c0 + 1, "one clean after %.1f s of quiet", cfg.tr_quiet);
    CHECK(t_our_full > 0, "it was the whole screen");
    ggq("qml", "GGQ 0 settle");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "and nothing more");

    printf("T34 1.8 sleep guard: going to sleep gives the withdrawn flash back to the sleep frame\n");
    elapse(5.0);
    sd("visibleChanged: true");
    partial("content ...", 112, 0, 1508, 2088);
    elapse(5.0);
    unsigned long r0 = n_restore, sb0 = n_sleep_back, f0 = n_force;
    c0 = cleans();
    sd("visibleChanged: false");
    sd(" - clearing on next update");
    sd(" - reset");
    CHECK(tr_active, "the cycle starts (it cannot know yet)");
    pw("Changing display state from Normal to DeepSleep (setDisplayState /x/batterymanager.cpp:196)");
    CHECK(!tr_active && sleeping && n_restore == r0 + 1 && n_sleep_back == sb0 + 1, "cycle ended, flash given back");
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - sleep");
    ggq("qml", "GGQ 0 settle");
    CHECK(n_force == f0 && n_sleep_frame >= 1, "the sleep frame had its full refresh (no warning)");
    unsigned long kk = n_kick;
    vc("SettingsOpened");
    sd(" - clearing on next update");
    CHECK(n_kick == kk && !tr_active, "while the display sleeps nothing is withdrawn");
    elapse(10.0);
    ggq("qml", "GGQ 0 hb");
    CHECK(cleans() == c0, "and nothing is cleaned");
    pw("Waking up display from keyEvent=16777399, lastWakeupReason=Unknown (eventFilter /x)");
    double op0 = sd_open_t;
    sd("visibleChanged: true");
    CHECK(sd_visible && !sd_tiles && sd_open_t == op0, "a wake is not a document open (no tiles state, no open guard)");
    pw("Changing display state from DeepSleep to Normal (setDisplayState /x)");
    CHECK(!sleeping && n_wake >= 1, "awake again");
    full(); // the stock wake flash
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "after the stock wake flash nothing of ours follows (no double flash)");
    printf("     a QML request made right after a stock full refresh is dropped (review C1)\n");
    elapse(5.0);
    c0 = cleans();
    unsigned long st0 = n_auto_stock;
    full(); // a stock full refresh (the wake flash, a document close in stock)
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 9 auto doc"); // the request is tried at once (and at every tick until AUTO_MAX_AGE)
    elapse(2.7); // past stock_gap since the flash, within AUTO_MAX_AGE of the request
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_auto_stock == st0 + 1, "dropped as a second flash, no clean");
    printf("     the light-sleep timer: the next document close is not withdrawn\n");
    elapse(5.0);
    unsigned long sk0 = n_sd_skip;
    kk = n_kick;
    pw("Light sleep timer timed out (operator() /x/batterymanager.cpp:1)");
    sd("visibleChanged: false");
    sd(" - clearing on next update");
    CHECK(n_sd_skip == sk0 + 1 && n_kick == kk && !tr_active, "skipped");
    elapse(15.0);
    sd("visibleChanged: true");

    printf("T35 1.8 gesture: a skipped ScreenDriver clean becomes its redraw's full STD; its own clean gets nothing\n");
    full();
    elapse(5.0);
    c0 = cleans();
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    for (int i = 0; i < 5; i++) {
        line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
        line(" - full screen animation");
    }
    sd("gestureModeChanged: false");
    line("swapBuffers: QFlags() QRegion(112,0 1508x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!auto_req && cleans() == c0, "nothing during the ScreenDriver's hold time");
    elapse(1.5);
    sd(" - updating mono state, mono -> full");
    unsigned long gf0 = n_force, p0 = n_promote;
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    CHECK(n_force == gf0 + 1 && n_promote == p0 + 1 && !auto_req, "skipped clean -> BlinkLater set for the redraw, no request");
    sd(" - reset");
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full update, STD");
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_force == gf0 + 1, "the promoted redraw is the only flash");
    // a gesture that hardly changed anything (dirty below gesture_min): nothing
    elapse(5.0);
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.004000");
    sd(" - doing nothing...");
    CHECK(n_force == gf0 + 1 && n_promote_skip >= 1, "dirty below gesture_min: left alone");
    // a "doing nothing" without a measured value (format changed): left alone
    sd(" - doing nothing...");
    CHECK(n_force == gf0 + 1, "no dirty value: left alone");
    // asleep: never
    pw("Changing display state from Normal to DeepSleep (setDisplayState /x)");
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    CHECK(n_force == gf0 + 1, "asleep: left alone");
    pw("Changing display state from DeepSleep to Normal (setDisplayState /x)");
    // switched off: never
    ggq("qml", "GGQ 9 set gesture_clean 0");
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    CHECK(n_force == gf0 + 1, "gesture_clean 0: left alone");
    ggq("qml", "GGQ 9 set gesture_clean 1");
    elapse(5.0);
    c0 = cleans();
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.238044");
    sd(" - full update..");
    sd(" - reset");
    full();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "the ScreenDriver cleaned itself: nothing added");

    printf("     a lasso or selection mono state is never promoted (review C4)\n");
    elapse(5.0);
    unsigned long sp0 = n_promote, sk1 = n_promote_skip;
    gf0 = n_force;
    sd("selectionModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(400,400 300x300) EPFramebuffer::PresentationMode::Default");
    line(" - partial animation");
    sd("selectionModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    CHECK(n_promote == sp0 && n_force == gf0 && n_promote_skip == sk1 + 1 && !promote_live, "selection: left alone");
    sd(" - reset");
    printf("     a gesture made with the pen near is never promoted\n");
    elapse(5.0);
    sd("penCloseChanged: true");
    gesture_skip("0.050000");
    CHECK(n_promote == sp0 && n_force == gf0 && !promote_live, "pen near: left alone");
    sd(" - reset");
    sd("penCloseChanged: false");
    CHECK(!sd_pen, "pen away");

    printf("     a promoted byte is taken back when a gesture, the pen or a document close comes first (review C0)\n");
    elapse(5.0);
    unsigned long w0 = n_withdraw_host, u0 = n_unpromote;
    gesture_skip("0.050000");
    CHECK(promote_live && n_force == gf0 + 1, "promoted");
    sd(" - reset");
    sd("gestureModeChanged: true"); // the next swipe started before the redraw
    CHECK(!promote_live && n_withdraw_host == w0 + 1 && n_unpromote == u0 + 1, "a new gesture: taken back");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.060000");
    sd(" - doing nothing...");
    CHECK(promote_live && n_force == gf0 + 2, "its own end decides again: promoted");
    sd(" - reset");
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full update, STD");
    CHECK(!promote_live, "the redraw took it");
    sd("gestureModeChanged: true");
    CHECK(n_withdraw_host == w0 + 1, "a gesture after the redraw takes nothing back");
    sd(" - updating mono state, full -> mono");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.001000");
    sd(" - doing nothing...");
    sd(" - reset");
    elapse(5.0);
    gesture_skip("0.050000");
    sd(" - reset");
    CHECK(promote_live && n_force == gf0 + 3, "promoted");
    sd("penCloseChanged: true"); // the pen comes near before the redraw
    CHECK(!promote_live && n_withdraw_host == w0 + 2 && n_unpromote == u0 + 2, "the pen: taken back");
    sd("penCloseChanged: false");
    elapse(5.0);
    gesture_skip("0.050000");
    sd(" - reset");
    CHECK(promote_live && n_force == gf0 + 4, "promoted");
    unsigned long kk2 = n_kick, tc2 = n_tr_clean;
    c0 = cleans();
    sd("visibleChanged: false"); // the document closes before the redraw
    sd(" - clearing on next update");
    CHECK(!promote_live && n_withdraw_host == w0 + 3 && n_kick == kk2 + 1 && tr_active,
          "a document close: taken back, then the close's own flash is withdrawn");
    sd(" - reset");
    fill(0, 0, SW, SH, 255);
    text(0, 300, SW, 900);
    partial("ui ........", 0, 0, SW, SH);
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc2 + 1 && cleans() == c0 + 1, "one clean after the library settled");
    elapse(15.0);
    sd("visibleChanged: true"); // open the document again: tiles, then the stock in-place flash
    page_turn(0);
    sd("pendingTilesChanged: false");
    full();
    flush();

    printf("     a view change waiting for its clean during a document gesture waits, and the gesture's full STD\n"
           "     stands in for it (review C6)\n");
    elapse(5.0);
    c0 = cleans();
    gf0 = n_force;
    unsigned long ab0 = n_tr_absorbed;
    vc("SettingsOpened");
    CHECK(tr_active, "cycle started");
    partial("ui ........", 0, 0, SW, SH);
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    sd("gestureModeChanged: false");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && cleans() == c0, "no clean during the gesture or its mono hold");
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    CHECK(tr_active && tr_promoted && n_force == gf0 + 1, "promoted: the cycle waits for the redraw");
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && cleans() == c0, "no clean of ours between the promote and its redraw");
    sd(" - reset");
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full update, STD");
    CHECK(!tr_active && n_tr_absorbed == ab0 + 1, "the redraw took the byte: the cycle ends in it");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "one flash in all");
    elapse(5.0);
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH);
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.238044");
    sd(" - full update..");
    CHECK(!tr_active && n_tr_absorbed == ab0 + 2, "the ScreenDriver's own clean: the cycle ends in it");
    sd(" - reset");
    full();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "one flash in all");

    // the redraw that ends a gesture is not a page turn, also when the promotion is off (the user turned it off)
    ggq("qml", "GGQ 9 set gesture_clean 0");
    elapse(5.0);
    c0 = cleans();
    unsigned long pg0 = n_arm[AK_PAGE];
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    line("swapBuffers: QFlags() QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - full screen animation");
    page_turn(1); // a page turn during the gesture (content frame): finalized only after the gesture ended
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.050000");
    sd(" - doing nothing...");
    sd(" - reset");
    redraw_page(0); // its mono -> full redraw: most of the page as content, no flag
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(n_arm[AK_PAGE] == pg0 && cleans() == c0 && !auto_req, "gesture end redraw (promotion off): no page request");
    elapse(1.0);
    page_turn(1); // a real page turn (quick flick, no gesture mode) a moment later still counts
    flush();
    CHECK(n_arm[AK_PAGE] == pg0 + 1 && auto_req, "a page turn after it still asks");
    auto_req = 0;
    elapse(5.0);
    redraw_page(1); // a large content update without finalizeNewPage (undo, tiles, ...)
    flush();
    elapse(1.0);
    redraw_page(0);
    flush();
    CHECK(n_arm[AK_PAGE] == pg0 + 1 && !auto_req, "large content updates that are not page turns: no request");
    ggq("qml", "GGQ 9 set gesture_clean 1");

    printf("T36 1.8 page turn: model-gated full clean; waits for the pen; dropped when the user writes\n");
    elapse(5.0);
    c0 = cleans();
    unsigned long pa0 = n_arm[AK_PAGE];
    page_turn(1);
    partial("ui ........", 1400, 2088, 100, 40); // the page number
    CHECK(n_arm[AK_PAGE] == pa0 + 1 && auto_req && auto_kind == AK_PAGE, "page turn -> request");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "waits for quiet");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && t_our_full > 0, "one full clean");
    elapse(5.0);
    c0 = cleans();
    page_turn(0);
    partial("ui ........", 1400, 2088, 100, 40);
    elapse(0.5);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(cleans() == c0 && auto_req, "pen near: waits");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(cleans() == c0 && auto_req, "pen still near 3.5 s later: still waits (review C7)");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "pen away: one clean");
    elapse(5.0);
    c0 = cleans();
    page_turn(1);
    partial("ui ........", 1400, 2088, 100, 40);
    sd("penCloseChanged: true");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "the ScreenDriver's pen state holds it too");
    sd("penCloseChanged: false");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "pen away: one clean");
    elapse(5.0);
    c0 = cleans();
    page_turn(0);
    partial("ui ........", 1400, 2088, 100, 40);
    elapse(0.5);
    ggq("qml", "GGQ 0 settle pen");
    elapse(AUTO_PEN_MAX_AGE);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(cleans() == c0 && !auto_req, "the pen stayed near: aged out, no clean");
    elapse(5.0);
    c0 = cleans();
    unsigned long ink0 = n_auto_ink;
    page_turn(1);
    partial("ui ........", 1400, 2088, 100, 40);
    partial("pen .......", 500, 500, 40, 40); // the user writes right away
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_auto_ink == ink0 + 1, "dropped: the user writes");
    printf("     the document opens: tiles drawn, then the stock flash in place -> no clean of ours\n");
    elapse(5.0);
    c0 = cleans();
    sd("visibleChanged: false");
    cfg.sd_close = 0;
    sd(" - clearing on next update");
    cfg.sd_close = 1;
    sd("visibleChanged: true");
    page_turn(0);
    sd("pendingTilesChanged: false");
    sd(" - clear on next update");
    full();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "nothing (stock flash went over the screen)");
    printf("     a slow open: its page frame is left to the stock in-place flash that follows the tiles (review C5)\n");
    elapse(5.0);
    c0 = cleans();
    unsigned long pg1 = n_arm[AK_PAGE];
    sd("visibleChanged: false");
    cfg.sd_close = 0;
    sd(" - clearing on next update");
    cfg.sd_close = 1;
    elapse(5.0);
    sd("visibleChanged: true");
    CHECK(sd_tiles, "open: tiles pending");
    page_turn(1);
    flush();
    for (int i = 0; i < 6; i++) {
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
    }
    CHECK(cleans() == c0 && n_arm[AK_PAGE] == pg1, "1.5 s, tiles still rendering: no request, no clean");
    sd("pendingTilesChanged: false");
    sd(" - clear on next update");
    full();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "one flash in all (the stock one)");
    elapse(5.0);
    sd("visibleChanged: false");
    cfg.sd_close = 0;
    sd(" - clearing on next update");
    cfg.sd_close = 1;
    sd("visibleChanged: true");
    elapse(2.5);
    CHECK(!tiles_busy(now_s()), "an open whose tiles line never came lapses after 2 s");
    full();
    flush();
    printf("     toolbar, menus and small content updates in a document never ask\n");
    elapse(5.0);
    pa0 = n_arm[AK_PAGE];
    line("swapBuffers: QFlags() QRegion(size=4, bounds=(0,0 1588x2160) - [(0,0 112x32), (0,32 112x48), (1540,32 48x48), "
         "(0,80 112x2080)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=2, bounds=(80,124 20x132) - [(80,124 20x20), (80,236 20x20)])");
    line(" - ui ........: QRegion(size=4, bounds=(0,0 1588x2160) - [(0,0 112x32), (0,32 112x48), (1540,32 48x48), (0,80 112x2080)])");
    partial("content ...", 681, 2088, 258, 72);
    partial("ui ........", 0, 0, 414, 2160);
    partial("ui ........", 0, 0, 414, 2160);
    ggq("qml", "GGQ 0 status");
    CHECK(n_arm[AK_PAGE] == pa0 && !auto_req, "no request (toolbar strip bounds 1588x2160 is not its area)");

    printf("T37 1.8 keyboard: shown (Overlay or A2), typing never asks, closing gives one full clean\n");
    sd("visibleChanged: false");
    // xochitl's start logs "pendingTilesChanged: true" with the document view hidden and no "false" until a document
    // opens (tablet, 2026-11-09 02:57:03 UTC): the library's cleans must not wait for it
    sd("pendingTilesChanged: true");
    CHECK(sd_tiles && !sd_visible, "stale tiles state of a hidden document view (xochitl start)");
    elapse(5.0);
    for (int pass = 0; pass < 2; pass++) {
        const char *cls = pass ? "animation ." : "overlay ...";
        c0 = cleans();
        unsigned long ka0 = n_arm[AK_KB];
        partial("ui ........", 581, 1448, 457, 468); // the + menu
        kb_frame(cls);
        flush();
        CHECK(kb_on && kb_box.y == 1544 && kb_box.h == 616, "[%s] keyboard seen at y=%d h=%d", cls, kb_box.y, kb_box.h);
        for (int i = 0; i < 4; i++) {
            key_press(pass ? "partial animation" : "overlay ...: QRegion(668,1641 139x118)");
            ggq("qml", "GGQ 0 settle");
        }
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(n_arm[AK_KB] == ka0 && cleans() == c0, "[%s] typing: no request, no clean", cls);
        partial("ui ........", 0, 0, SW, SH); // Cancel: the library over the whole screen
        flush();
        CHECK(!kb_on && auto_req && auto_kind == AK_KB, "[%s] closed -> request", cls);
        elapse(0.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && t_our_full > 0, "[%s] one full clean", cls);
        elapse(5.0);
    }
    printf("     the keyboard shown again before its clean: nothing while it is up (review C3)\n");
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    partial("ui ........", 0, 0, SW, SH); // closed (focus moved)
    flush();
    CHECK(!kb_on && auto_req && auto_kind == AK_KB, "closed -> request");
    elapse(0.2);
    kb_frame("overlay ...");
    flush();
    CHECK(kb_on && !auto_req, "shown again 0.2 s later: the request is dropped");
    for (int i = 0; i < 3; i++) {
        key_press("overlay ...: QRegion(668,1641 139x118)");
        ggq("qml", "GGQ 0 settle");
    }
    elapse(2.0);
    ggq("qml", "GGQ 0 settle pen");
    ggq("qml", "GGQ 0 settle");
    ggq("qml", "GGQ 9 auto doc");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && auto_req, "typing and pauses: no clean while it is up (a request of another kind waits)");
    auto_req = 0;
    partial("ui ........", 0, 0, SW, SH); // closed for good
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !kb_on, "closed for good: one clean");
    elapse(5.0);
    printf("     side menu x5 and the + menu: zero requests, zero cleans\n");
    c0 = cleans();
    unsigned long arms0 = n_arm[AK_PAGE] + n_arm[AK_GESTURE] + n_arm[AK_KB] + n_arm[AK_QML];
    for (int i = 0; i < 10; i++) {
        partial("ui ........", 0, 0, 414, 2160);
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        elapse(2.0);
        ggq("qml", "GGQ 0 settle");
    }
    partial("ui ........", 581, 1448, 457, 468);
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    partial("ui ........", 581, 1448, 457, 468);
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && n_arm[AK_PAGE] + n_arm[AK_GESTURE] + n_arm[AK_KB] + n_arm[AK_QML] == arms0,
          "no request, no clean");

    printf("T39 1.8 review round 2: pen-held age, menus over a waiting request, promote and cycles, pen state\n");
    int pgs = 0;
#define TURN() page_turn(pgs ^= 1)
    elapse(5.0);
    sd("visibleChanged: true"); // a document opens: its page, tiles, the stock in-place flash
    TURN();
    sd("pendingTilesChanged: false");
    full();
    flush();
    printf("     a request the pen held keeps %.0f s when the pen leaves (liveness-2)\n", AUTO_PEN_MAX_AGE);
    elapse(5.0);
    c0 = cleans();
    TURN();
    partial("ui ........", 1400, 2088, 100, 40); // the page number: small, not a menu
    sd("penCloseChanged: true");
    for (int i = 0; i < 6; i++) {
        elapse(1.0);
        ggq("qml", "GGQ 0 settle pen");
    }
    CHECK(cleans() == c0 && auto_req && auto_pen_held, "pen near for 6 s: waits");
    sd("penCloseChanged: false");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "pen away after 6 s: one clean");
    elapse(5.0);
    c0 = cleans();
    TURN();
    partial("ui ........", 1400, 2088, 100, 40);
    sd("penCloseChanged: true");
    for (int i = 0; i < 9; i++) {
        elapse(1.0);
        ggq("qml", "GGQ 0 settle pen");
    }
    sd("penCloseChanged: false");
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "pen near for 9 s: aged out after %.0f s, no clean", AUTO_PEN_MAX_AGE);

    printf("     a menu or toolbar opened while a request waits drops it (liveness-1)\n");
    elapse(5.0);
    c0 = cleans();
    TURN();
    partial("ui ........", 1400, 2088, 100, 40);
    sd("penCloseChanged: true");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(auto_req && cleans() == c0, "waits for the pen");
    unsigned long pp0 = n_popup;
    line("swapBuffers: QFlags() QRegion(size=4, bounds=(0,0 1588x2160) - [(0,0 112x32), (0,32 112x48), (1540,32 48x48), "
         "(0,80 112x2080)]) EPFramebuffer::PresentationMode::Default");
    line(" - content ...: QRegion(size=2, bounds=(80,124 20x132) - [(80,124 20x20), (80,236 20x20)])");
    line(" - ui ........: QRegion(size=4, bounds=(0,0 1588x2160) - [(0,0 112x32), (0,32 112x48), (1540,32 48x48), (0,80 112x2080)])");
    flush();
    CHECK(!auto_req && n_popup == pp0 + 1, "the document toolbar opened: dropped");
    sd("penCloseChanged: false");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "no flash on it");
    sd("visibleChanged: false"); // to the library (no close cycle: no " - clearing on next update")
    elapse(5.0);
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    partial("ui ........", 0, 0, SW, SH); // Cancel: the close of the keyboard
    flush();
    CHECK(auto_req && auto_kind == AK_KB, "keyboard closed -> request (the close itself is not a menu)");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(auto_req && cleans() == c0, "pen near: waits");
    partial("ui ........", 0, 0, 414, 2160); // the side menu, opened with the pen
    flush();
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req && n_popup == pp0 + 2, "the side menu opened meanwhile: dropped, no flash on it");
    partial("ui ........", 0, 0, 414, 2160); // it closes
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "nor when it closes");

    printf("     a promote ends a waiting view-change cycle only at the frame that takes it; a take-back keeps it\n");
    elapse(5.0);
    sd("visibleChanged: true");
    TURN();
    sd("pendingTilesChanged: false");
    full();
    flush();
    elapse(5.0);
    c0 = cleans();
    unsigned long ab1 = n_tr_absorbed, tc3 = n_tr_clean, un0 = n_unpromote;
    vc("DocumentViewDialogClosed");
    partial("ui ........", 300, 300, 400, 400);
    gesture_skip("0.050000");
    CHECK(tr_active && tr_promoted && promote_live, "promoted: the cycle waits for the redraw");
    sd(" - reset");
    sd("penCloseChanged: true"); // the pen comes before the redraw
    CHECK(!promote_live && !tr_promoted && tr_active && n_unpromote == un0 + 1, "taken back: the cycle goes on waiting");
    partial("content ...", 112, 0, 1508, 2160); // the redraw goes out as a normal update
    sd("penCloseChanged: false");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc3 + 1 && n_tr_absorbed == ab1 && cleans() == c0 + 1,
          "the view change gets its own clean");

    printf("     a keyboard request waiting over a promote: no clean between the promote and its redraw\n");
    elapse(5.0);
    c0 = cleans();
    kb_frame("overlay ..."); // a text box keyboard in the document
    flush();
    partial("ui ........", 0, 1500, SW, 660); // it closes
    flush();
    CHECK(auto_req && auto_kind == AK_KB, "keyboard closed -> request");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(auto_req && cleans() == c0, "pen near: waits");
    gesture_skip("0.050000"); // a swipe meanwhile, promoted
    CHECK(promote_live && auto_req, "promoted; the keyboard request still waits");
    ggq("qml", "GGQ 0 settle"); // the pen left: a tick between the promote and its redraw
    CHECK(cleans() == c0, "no clean before the promoted redraw");
    sd(" - reset");
    full(); // the promoted redraw
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "the promoted STD went over the screen: one flash in all");

    printf("     the pen state survives a close and an open; it is reset at sleep\n");
    elapse(5.0);
    unsigned long p1 = n_promote;
    sd("penCloseChanged: true");
    sd("visibleChanged: false");
    sd("visibleChanged: true"); // another document, the pen still near (no new pen line)
    CHECK(sd_pen, "pen still near after close and open");
    sd("pendingTilesChanged: false");
    full();
    flush();
    elapse(5.0);
    gesture_skip("0.050000");
    CHECK(n_promote == p1 && !promote_live, "a gesture with the pen near: not promoted");
    sd(" - reset");
    pw("Changing display state from Normal to DeepSleep (setDisplayState /x)");
    CHECK(!sd_pen, "reset at sleep");
    pw("Changing display state from DeepSleep to Normal (setDisplayState /x)");
    full();
    flush();

    printf("     a page request made 1.4 s after a stock full refresh is kept and cleaned after stock_gap (C1 scope)\n");
    elapse(5.0);
    c0 = cleans();
    full(); // a stock full refresh (the wake flash)
    flush();
    elapse(1.4);
    TURN();
    partial("ui ........", 1400, 2088, 100, 40);
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(auto_req && cleans() == c0, "kept, waits for stock_gap");
    elapse(1.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "then one clean");

    printf("     the open's own STD releases the open guard; a slow open's page frame asks nothing (C5 window)\n");
    elapse(5.0);
    sd("visibleChanged: false");
    elapse(5.0);
    c0 = cleans();
    unsigned long pg2 = n_arm[AK_PAGE], wh0 = n_withdraw_host;
    sd("visibleChanged: true");
    TURN();
    flush();
    elapse(0.6);
    sd("pendingTilesChanged: false");
    sd(" - clear on next update");
    sd("penCloseChanged: true"); // between the open's own BlinkLater and its STD: nothing taken back
    CHECK(n_withdraw_host == wh0, "xochitl's own byte (open) is never taken back");
    sd("penCloseChanged: false");
    full();
    flush();
    elapse(3.3);
    TURN();
    partial("ui ........", 1400, 2088, 100, 40);
    CHECK(n_arm[AK_PAGE] == pg2 + 1 && auto_req, "a page turn 3.9 s after a normal open asks");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "one clean");
    elapse(5.0);
    sd("visibleChanged: false");
    elapse(5.0);
    c0 = cleans();
    pg2 = n_arm[AK_PAGE];
    sd("visibleChanged: true");
    elapse(3.0);
    TURN();
    flush();
    CHECK(n_arm[AK_PAGE] == pg2, "a page frame 3 s into a slow open asks nothing");
    elapse(2.0);
    sd("pendingTilesChanged: false");
    sd(" - clear on next update");
    full();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(3.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !auto_req, "one flash in all (the stock one)");

    printf("     the pen never takes back xochitl's own gesture clean\n");
    elapse(5.0);
    sd("gestureModeChanged: true");
    sd(" - updating mono state, full -> mono");
    sd("gestureModeChanged: false");
    elapse(2.0);
    sd(" - updating mono state, mono -> full");
    sd(" - clean the screen, dirty=0.238044");
    sd(" - full update..");
    sd("penCloseChanged: true");
    CHECK(n_withdraw_host == wh0, "nothing taken back");
    sd(" - reset");
    full();
    sd("penCloseChanged: false");
    ggq("qml", "GGQ 97 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"popup\":") && strstr(js, "\"pen\":0") && strstr(js, "\"promote\":["), "status: %.60s",
          js ? strstr(js, "\"promote\"") : "");
#undef TURN
    sd("visibleChanged: false");

    printf("T40 1.8 review round 2 (liveness-3): minor ui updates and the keyboard in a view change's clean\n");
    elapse(40.0);
    ggq("qml", "GGQ 0 settle");
    ggq("qml", "GGQ 0 settle");
    auto_req = 0;
    host_rc = "ok";
    CHECK(!tr_active && !kb_on && !sd_visible, "nothing waits");
    unsigned long tc = n_tr_clean, su = n_minor_upd;
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH); // the new view
    elapse(0.3);
    status_frame();
    elapse(0.3);
    status_frame(); // the indicator, 0 s before the tick
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1 && n_minor_upd == su + 2,
          "0.6 s after the view, the sync indicator just now: cleaned (minor updates +%lu)", n_minor_upd - su);
    elapse(5.0);
    tc = n_tr_clean;
    vc("SettingsClosed");
    elapse(0.05);
    status_frame();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc, "only the indicator drew since the view change: waits for the view's frame");
    partial("ui ........", 0, 0, SW, SH);
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc, "the view drew 0.3 s ago: waits");
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1, "0.6 s after the view: cleaned");
    printf("     the closed document's sync icon in the library list does not delay it either\n");
    elapse(5.0);
    tc = n_tr_clean;
    vc("SettingsClosed");
    partial("ui ........", 0, 0, SW, SH);
    elapse(0.3);
    line("swapBuffers: QFlags() QRegion(size=3, bounds=(34,699 236x24) - [(34,699 101x6), (34,705 236x10), (34,715 101x8)]) "
         "EPFramebuffer::PresentationMode::Default");
    line(" - ui ........: QRegion(size=3, bounds=(34,699 236x24) - [(34,699 101x6), (34,705 236x10), (34,715 101x8)])");
    elapse(0.3);
    partial("ui ........", 34, 686, 278, 37);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1, "0.6 s after the view, list icons 0.3 s and 0 s ago: cleaned");
    const char *notst[3][2] = {{"content ...", "1394,44 178x32"}, {"ui ........", "0,0 1620x60"},
                               {"ui ........", "1000,1000 200x180"}};
    const char *notst_why[3] = {"content in a small area", "a ui update of 2.8% (a toolbar strip)",
                                "a ui update of 1.03%"};
    for (int k = 0; k < 3; k++) {
        char b[200];
        elapse(5.0);
        tc = n_tr_clean;
        vc("SettingsOpened");
        partial("ui ........", 0, 0, SW, SH);
        elapse(0.6);
        snprintf(b, sizeof b, "swapBuffers: QFlags() QRegion(%s) EPFramebuffer::PresentationMode::Default", notst[k][1]);
        line(b);
        snprintf(b, sizeof b, " - %s: QRegion(%s)", notst[k][0], notst[k][1]);
        line(b);
        ggq("qml", "GGQ 0 settle");
        CHECK(tr_active && n_tr_clean == tc, "%s counts as drawing: waits", notst_why[k]);
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && n_tr_clean == tc + 1, "0.6 s later: cleaned");
    }
    elapse(5.0);
    tc = n_tr_clean;
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH);
    elapse(0.6);
    line("swapBuffers: QFlags() QRegion(1394,44 178x32) EPFramebuffer::PresentationMode::Default");
    line(" - ui ........: QRegion(1394,44 100x32)");
    line(" - content ...: QRegion(1494,44 78x32)");
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc, "ui and content together in the top band count as drawing: waits");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1, "0.6 s later: cleaned");
    elapse(5.0);
    tc = n_tr_clean;
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH);
    elapse(0.6);
    partial("ui ........", 1394, 44, 178, 32);
    line(" - merged UI/Content");
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc, "a small update merged with content counts as drawing: waits");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1, "0.6 s later: cleaned");
    printf("     with the pen near, the %.1f s also count from the last update that was not minor\n", TR_INK_QUIET);
    elapse(5.0);
    tc = n_tr_clean;
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH);
    for (int i = 0; i < 3; i++) {
        elapse(0.4);
        status_frame();
        ggq("qml", "GGQ 0 settle pen");
    }
    CHECK(tr_active && n_tr_clean == tc, "pen near, 1.2 s after the view: waits");
    elapse(0.4);
    status_frame();
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!tr_active && n_tr_clean == tc + 1, "1.6 s after the view, the indicator just now: cleaned");
    printf("     never while the on-screen keyboard is up; kept past %.0f s; one flash with the keyboard's own request\n",
           TR_MAX_AGE);
    elapse(5.0);
    tc = n_tr_clean;
    c0 = cleans();
    unsigned long ag0 = n_tr_aged;
    vc("SettingsClosed");
    partial("ui ........", 0, 0, SW, SH);
    kb_frame("overlay ...");
    flush();
    CHECK(kb_on, "keyboard up right after the view change");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc && tr_kb_held, "1 s later, keyboard up: waits");
    for (int i = 0; i < 40; i++) {
        key_press("overlay ...: QRegion(668,1641 139x118)");
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
    }
    CHECK(tr_active && n_tr_clean == tc && n_tr_aged == ag0, "typing for 40 s: still waits, not aged");
    partial("ui ........", 0, 0, SW, SH); // the dialog closes over the keyboard
    flush();
    CHECK(!kb_on && auto_req && auto_kind == AK_KB, "keyboard closed: its own request waits too");
    host_rc = "busy";
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(tr_active && n_tr_clean == tc && n_tr_aged == ag0, "busy: tried again later, not aged");
    host_rc = "ok";
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1, "one full clean after the keyboard closed");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && !auto_req, "the keyboard's request is covered by it: one flash in all");
    elapse(5.0);
    tc = n_tr_clean;
    ag0 = n_tr_aged;
    vc("SettingsOpened");
    partial("ui ........", 0, 0, SW, SH);
    elapse(TR_MAX_AGE + 1);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!tr_active && n_tr_aged == ag0 + 1, "without the keyboard a cycle still ages after %.0f s", TR_MAX_AGE);
    ggq("qml", "GGQ 97 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"minorUpd\":"), "status: %.24s", js ? strstr(js, "\"minorUpd\"") : "");


    printf("T41 1.8.1/1.8.2 keyboard open: the band gets ONE in-place clean before the first key press, never while typing\n");
    elapse(40.0);
    ggq("qml", "GGQ 0 settle");
    ggq("qml", "GGQ 0 settle");
    auto_req = 0;
    tr_active = 0;
    host_rc = "ok";
    if (kb_on) { partial("ui ........", 0, 0, SW, SH); flush(); }
    elapse(5.0);
    ggq("qml", "GGQ 0 settle");
    auto_req = 0;
    CHECK(!tr_active && !kb_on && !kbo_req && !auto_req && !sd_visible, "nothing waits");
    // the user's journal 2026-11-09 08:54:55-56 UTC: the + menu, then dialog + keyboard in one frame, the text cursor
    // 0.445 s later, the first key 45 s later
    printf("     off (default): the stock keyboard as before\n");
    CHECK(cfg.kb_open_clean == 0, "kb_open_clean off by default");
    c0 = cleans();
    unsigned long kn0 = n_kbo;
    partial("ui ........", 581, 1448, 457, 468); // the + menu
    elapse(1.0);
    kb_frame("overlay ...");
    flush();
    elapse(0.445);
    partial("ui ........", 237, 254, 2, 36); // the text cursor
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(kb_on && !kbo_req && n_kbo == kn0 && cleans() == c0, "keyboard up: no request, no clean");
    partial("ui ........", 0, 0, SW, SH); // Cancel
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kb_on && cleans() == c0 + 1, "closed: the keyboard's own full clean (1.8)");
    elapse(5.0);

    ggq("qml", "GGQ 94 set kb_open_clean 1");
    CHECK(cfg.kb_open_clean == 0, "1.8.3: turning it on is refused (retired)");
    // the machinery itself stays tested (and is under the SWTCON gate): switched on directly here, never from text
    cfg.kb_open_clean = 1;
    for (int pass = 0; pass < 2; pass++) {
        const char *cls = pass ? "animation ." : "overlay ...";
        const char *key = pass ? "partial animation" : "overlay ...: QRegion(668,1641 139x118)";
        printf("     [%s] + menu, keyboard, cursor, quiet: one clean of the band; typing: nothing; close: one full\n", cls);
        c0 = cleans();
        unsigned long kc0 = n_kbo_clean, kd0 = n_kbo_drop, wo0 = n_clean_wf[2];
        partial("ui ........", 581, 1448, 457, 468);
        elapse(1.0);
        kb_frame(cls);
        flush();
        CHECK(kb_on && kbo_req && kbo_box.x == 0 && kbo_box.y == 1544 && kbo_box.w == 1620 && kbo_box.h == 616,
              "[%s] request for the band %d,%d %dx%d", cls, kbo_box.x, kbo_box.y, kbo_box.w, kbo_box.h);
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(kbo_req && cleans() == c0, "[%s] 0.25 s: not quiet yet", cls);
        elapse(0.195);
        partial("ui ........", 237, 254, 2, 36);
        flush();
        CHECK(kbo_req, "[%s] the text cursor does not drop it", cls);
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(!kbo_req && cleans() == c0 + 1 && n_kbo_clean == kc0 + 1 && n_clean_wf[2] == wo0 + 1 &&
              t_our_full < kbo_t, "[%s] 0.7 s: one STD in place of the band (not a full clean)", cls);
        for (int i = 0; i < 6; i++) {
            key_press(key);
            elapse(0.3);
            ggq("qml", "GGQ 0 settle");
        }
        kb_frame(cls); // the keyboard redrawn while up (shift state after the first letter)
        flush();
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(!kbo_req && cleans() == c0 + 1 && n_kbo_drop == kd0, "[%s] typing, keyboard redrawn: no request, no clean", cls);
        partial("ui ........", 0, 0, SW, SH); // Cancel
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!kb_on && cleans() == c0 + 2, "[%s] closed: its full clean, two flashes in all", cls);
        elapse(5.0);
    }

    printf("     a key before the quiet time: dropped, no clean (never while typing)\n");
    c0 = cleans();
    unsigned long kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.3);
    key_press("overlay ...: QRegion(668,1641 139x118)");
    ggq("qml", "GGQ 0 settle");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped at the key, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "the close keeps its full clean");
    elapse(5.0);

    printf("     a key in the frame still being assembled at the tick: dropped\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.6);
    line("swapBuffers: QFlags() QRegion(668,1641 139x118) EPFramebuffer::PresentationMode::Default");
    line(" - overlay ...: QRegion(668,1641 139x118)");
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     1.8.2 pen near: dropped at its first tick, settle or heartbeat (no flash under keys tapped with the pen)\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle pen");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "settle with the pen near: dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 hb pen");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "heartbeat with the pen near: dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     1.8.2 pen near at the keyboard, gone before the clock's first tick: cleaned at that tick\n");
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    elapse(0.7); // the clock sent nothing while the pen was near; its leave kicks it
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && cleans() == c0 + 1, "pen gone: cleaned");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     1.8.2 the clock's first pen-free tick after %.1f s (pen held near): dropped, never a late flash\n", KBO_MAX_AGE);
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(1.95); // the measured pen-near start of 1.8.1
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped at +1.95 s, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     1.8.2 a stock full refresh less than stock_gap s before the keyboard: dropped (no second flash)\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    full();
    flush();
    elapse(1.0);
    kb_frame("overlay ...");
    flush();
    CHECK(kbo_req, "armed after the stock flash");
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);
    c0 = cleans();
    full();
    flush();
    elapse(cfg.stock_gap);
    kb_frame("overlay ...");
    flush();
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && cleans() == c0 + 1, "stock_gap after it: cleaned");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     quick Cancel at +1.6 s: the band at +0.7 s, then the close's own full clean (two flashes, both ours)\n");
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    elapse(0.445);
    partial("ui ........", 237, 254, 2, 36);
    flush();
    elapse(0.25);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && cleans() == c0 + 1, "band cleaned");
    elapse(0.9);
    partial("ui ........", 0, 0, SW, SH); // Cancel
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kb_on && cleans() == c0 + 2, "closed: two flashes of ours in all");
    elapse(5.0);

    printf("     quick Create at +1.5 s: the band at +0.7 s, then xochitl's own STD of the new document (one of ours)\n");
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "band cleaned");
    elapse(0.8);
    full(); // the document opens with xochitl's own STD
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kb_on && !auto_req && cleans() == c0 + 1, "no clean of ours after the stock STD");
    elapse(5.0);

    printf("     busy framebuffer: tried again at the next tick, within %.1f s\n", KBO_MAX_AGE);
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    host_rc = "busy";
    elapse(0.45);
    ggq("qml", "GGQ 0 settle");
    CHECK(kbo_req && cleans() == c0, "busy: waits");
    host_rc = "pending";
    elapse(0.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(kbo_req && cleans() == c0, "a stock flash pending: waits");
    host_rc = "ok";
    elapse(0.2);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && cleans() == c0 + 1, "then cleaned once");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     unsupported clean: counted, not retried\n");
    unsigned long kf0 = n_kbo_fail;
    c0 = cleans();
    kb_frame("overlay ...");
    flush();
    host_rc = "unsupported";
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_fail == kf0 + 1 && cleans() == c0, "failed once, not retried");
    host_rc = "ok";
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     no tick for %.0f s (clock stopped): dropped, never a late flash\n", KBO_MAX_AGE);
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(KBO_MAX_AGE + 0.5);
    ggq("qml", "GGQ 0 hb");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     something drawn on the keyboard (even a small ui update): dropped\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.2);
    partial("ui ........", 1448, 1556, 20, 20);
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     a popup or a larger ui update first: dropped\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.2);
    partial("ui ........", 300, 400, 1000, 600);
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped, no clean");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     a stock full refresh first: dropped (never a second flash)\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.1);
    full();
    flush();
    elapse(0.5);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0 && !kb_on, "dropped, no clean");
    elapse(5.0);

    printf("     1.8.2 keyboard still up after a stock full refresh: its redraw while typing (shift state) never arms\n");
    c0 = cleans();
    unsigned long kn1 = n_kbo;
    kb_frame("overlay ...");
    flush();
    elapse(0.1);
    full(); // kb_on cleared under a keyboard that stays up
    flush();
    elapse(4.0); // past stock_gap
    key_press("overlay ...: QRegion(668,1641 139x118)");
    kb_redraw("overlay ...");
    flush();
    CHECK(kb_on && !kbo_req && n_kbo == kn1 + 1, "the redraw shows the keyboard again but arms nothing");
    elapse(0.7);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0, "no clean while typing");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kb_on && cleans() == c0 + 1, "the close keeps its full clean");
    elapse(5.0);
    printf("     1.8.2 the same redraw while the keyboard is down and nothing else: shown, no clean armed\n");
    c0 = cleans();
    kn1 = n_kbo;
    kb_redraw("animation .");
    flush();
    CHECK(kb_on && !kbo_req && n_kbo == kn1, "not an opening: nothing armed");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     the display goes to sleep: dropped; the sleep frame and the wake add nothing\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    pw("Changing display state from Normal to DeepSleep");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1, "dropped at the sleep line");
    line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
    line(" - sleep");
    flush();
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    pw("Changing display state from DeepSleep to Normal");
    elapse(1.0);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 && !kbo_req && n_kbo_drop == kd0 + 1, "no clean");
    elapse(5.0);

    printf("     the keyboard closed before its clean: dropped\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    elapse(0.2);
    partial("ui ........", 0, 0, SW, SH);
    flush();
    CHECK(!kbo_req && !kb_on && n_kbo_drop == kd0 + 1, "dropped at the close");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1, "only the close's own full clean");
    elapse(5.0);

    printf("     turned off while it waits: dropped\n");
    c0 = cleans();
    kd0 = n_kbo_drop;
    kb_frame("overlay ...");
    flush();
    ggq("qml", "GGQ 9 set kb_open_clean 0");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && n_kbo_drop == kd0 + 1 && cleans() == c0, "dropped, no clean");
    cfg.kb_open_clean = 1;
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(5.0);

    printf("     with a view change waiting: the band now, the view's own full clean after the keyboard closed\n");
    c0 = cleans();
    tc = n_tr_clean;
    unsigned long kc1 = n_kbo_clean;
    vc("SettingsClosed");
    partial("ui ........", 0, 0, SW, SH);
    kb_frame("overlay ...");
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(n_kbo_clean == kc1 + 1 && cleans() == c0 + 1 && tr_active && n_tr_clean == tc, "band cleaned; the view waits");
    elapse(0.3);
    ggq("qml", "GGQ 0 settle");
    CHECK(cleans() == c0 + 1 && tr_active, "the next tick: still nothing while the keyboard is up");
    for (int i = 0; i < 3; i++) {
        key_press("overlay ...: QRegion(668,1641 139x118)");
        elapse(0.5);
        ggq("qml", "GGQ 0 settle");
    }
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(!tr_active && n_tr_clean == tc + 1 && cleans() == c0 + 2 && !auto_req, "closed: one full clean, two in all");
    elapse(5.0);

    printf("     a gesture's mono hold in a visible document: waits\n");
    c0 = cleans();
    sd("visibleChanged: true");
    sd("pendingTilesChanged: false");
    kb_frame("overlay ...");
    flush();
    sd(" - updating mono state, full -> mono");
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    CHECK(kbo_req && cleans() == c0, "mono hold: waits");
    sd(" - updating mono state, mono -> full");
    ggq("qml", "GGQ 0 settle");
    CHECK(!kbo_req && cleans() == c0 + 1, "then cleaned");
    partial("ui ........", 0, 0, SW, SH);
    flush();
    elapse(0.6);
    ggq("qml", "GGQ 0 settle");
    sd("visibleChanged: false");
    elapse(5.0);

    ggq("qml", "GGQ 97 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"kbOpen\":[1,") && strstr(js, "\"v\":\"" GG_VERSION "\"") && strstr(js, "\"on\":[1,1,1,1]"),
          "status: %.40s", js ? strstr(js, "\"kbOpen\"") : "");
    ggq("qml", "GGQ 0 set kbo_quiet 9");
    double kq = cfg.kbo_quiet;
    ggq("qml", "GGQ 0 set kbo_quiet 0.01");
    CHECK(kq == 0.7 && cfg.kbo_quiet == 0.2, "kbo_quiet clamped below KBO_MAX_AGE (%.1f %.1f)", kq, cfg.kbo_quiet);
    cfg.kbo_quiet = 0.4;
    ggq("qml", "GGQ 9 set kb_open_clean 0");

    printf("T38 1.8 status and limits\n");
    ggq("qml", "GGQ 0 set page_min 0.01");
    double pm = cfg.page_min;
    ggq("qml", "GGQ 0 set stock_gap 99");
    double sg = cfg.stock_gap;
    ggq("qml", "GGQ 0 set kb_clean 7");
    CHECK(pm == 0.2 && sg == 10.0 && cfg.kb_clean == 1, "clamped (%.2f %.1f %d)", pm, sg, cfg.kb_clean);
    cfg.stock_gap = 3.0;
    cfg.page_min = 0.5;
    ggq("qml", "GGQ 96 status");
    js = slurp(TESTRUN "/status.json");
    CHECK(js && strstr(js, "\"v18\":{\"on\":[1,1,1,1]") && strstr(js, "\"v\":\"" GG_VERSION "\""), "status: %.60s",
          js ? strstr(js, "\"v18\"") : "");
    cfg.sd_close = cfg.gesture_clean = cfg.page_clean = cfg.kb_clean = 0;
    sd("visibleChanged: false");

    printf("T43 1.8.3 SWTCON gate: less than the whole screen only while the panel is idle (IMG_1753)\n");
    {
        cfg.defer = 1;
        cfg.tr_quiet = 0.5; // as clock 12
        cfg.tr_pad = 16;
        cfg.tr_full = 0.35;
        hb_t = now_s();
        xo_state = 0;
        host_rc = "ok";
        host_sw = 1;
        cfg.kb_open_clean = 0;
        elapse(40.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
        tr_active = 0;
        CHECK(!tr_active && !kb_on && !kbo_req && !auto_req, "nothing waits");
        printf("     a small dialog (a box clean) while the panel is still driven: waits, then the box once idle\n");
        unsigned long g0 = cleans(), inf0 = n_inflight, up0 = n_upgrade, tc0 = n_tr_clean, fb0 = n_full_busy;
        vc("SettingsOpened");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        host_sw = 0;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(tr_active && cleans() == g0 && n_inflight == inf0 + 1 && tr_wait_t > 0, "busy: the box waits (inflight %lu)",
              n_inflight - inf0);
        CHECK(tr_box.x == 300 && tr_box.y == 500 && tr_box.w == 400 && tr_box.h == 300, "the waiting area is kept unpadded (%d,%d %dx%d)",
              tr_box.x, tr_box.y, tr_box.w, tr_box.h);
        double w0 = tr_wait_t;
        for (int i = 0; i < 4; i++) {
            elapse(0.25);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(tr_active && cleans() == g0 && n_inflight == inf0 + 5 && fabs(tr_wait_t - (w0 - 1.0)) < 1e-6 && tr_box.w == 400 &&
              tr_box.h == 300,
              "still busy: waits at every tick, same area, same start of the wait");
        host_sw = 1;
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && n_tr_clean == tc0 + 1 && n_upgrade == up0 && host_last.x == 284 &&
              host_last.y == 484 && host_last.w == 432 && host_last.h == 332, "idle: the box padded once goes out (%d,%d %dx%d)",
              host_last.x, host_last.y, host_last.w, host_last.h);
        CHECK(last_sw == 1 && n_full_busy == fb0, "sent with the panel idle");

        printf("     busy for %.1f s: the whole screen instead (one flash for the view change)\n", TR_IDLE_MAX);
        elapse(5.0);
        g0 = cleans();
        vc("SettingsClosed");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        host_sw = 0;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        int waits = 0;
        for (int i = 0; i < 11; i++) {
            elapse(0.25);
            ggq("qml", "GGQ 0 settle");
            waits += tr_active && cleans() == g0;
        }
        CHECK(waits == 11 && n_upgrade == up0, "waits for 2.75 s");
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && n_upgrade == up0 + 1 && rect_full(host_last) && n_full_busy == fb0 + 1,
              "3.0 s: one full in-place clean (%d,%d %dx%d), counted as sent over a busy panel", host_last.x, host_last.y,
              host_last.w, host_last.h);

        printf("     a new view during the wait starts the wait again\n");
        elapse(5.0);
        g0 = cleans();
        vc("SettingsOpened");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        for (int i = 0; i < 8; i++) {
            elapse(0.25);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(tr_active && tr_wait_t > 0 && cleans() == g0, "waited 2 s");
        vc("SettingsClosed");
        CHECK(tr_wait_t == 0, "the joining view change resets the wait");
        partial("ui ........", 320, 520, 300, 200);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(tr_active && cleans() == g0, "0.85 s after the new view: still waiting for the panel, no full clean yet");
        host_sw = 1;
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && !rect_full(host_last), "idle: the box of both views");

        printf("     the panel's state cannot be read: never a box, the whole screen right away\n");
        elapse(5.0);
        g0 = cleans();
        unsigned long up1 = n_upgrade;
        vc("SettingsOpened");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        host_sw = -1;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && rect_full(host_last) && n_upgrade == up1 + 1, "noidle: one full clean");
        host_sw = 1;

        printf("     full-screen cleans keep their timing (a full in-place STD covers what is being driven)\n");
        elapse(5.0);
        g0 = cleans();
        fb0 = n_full_busy;
        vc("SettingsClosed");
        partial("ui ........", 0, 0, SW, SH);
        flush();
        host_sw = 0;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && rect_full(host_last) && n_full_busy == fb0 + 1, "busy panel: the full clean goes out");
        host_sw = 1;

        printf("     an explicit rclean: a box waits for the panel (refused while busy), the whole screen does not\n");
        elapse(5.0);
        g0 = cleans();
        unsigned long rf0 = n_rclean_fail;
        host_sw = 0;
        ggq("qml", "GGQ 71 rclean 10 10 100 100");
        js = slurp(TESTRUN "/status.json");
        CHECK(cleans() == g0 && n_rclean_fail == rf0 + 1 && js && strstr(js, "\"rc\":\"inflight\""), "box: inflight");
        ggq("qml", "GGQ 72 rclean 0 0 1620 2160");
        js = slurp(TESTRUN "/status.json");
        CHECK(cleans() == g0 + 1 && js && strstr(js, "\"rc\":\"ok\""), "whole screen: sent");
        host_sw = 1;
        ggq("qml", "GGQ 73 rclean 10 10 100 100");
        CHECK(cleans() == g0 + 2, "box with the panel idle: sent");

        printf("     the keyboard band (machinery kept, option retired): never while busy, dropped at KBO_MAX_AGE\n");
        elapse(5.0);
        cfg.kb_open_clean = 1;
        g0 = cleans();
        unsigned long kd0 = n_kbo_drop, kc0 = n_kbo_clean;
        kb_frame("overlay ...");
        flush();
        CHECK(kbo_req, "armed");
        host_sw = 0;
        elapse(0.45);
        ggq("qml", "GGQ 0 settle");
        CHECK(kbo_req && cleans() == g0, "0.45 s, panel busy: waits");
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(kbo_req && cleans() == g0, "0.75 s, panel busy: waits");
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(!kbo_req && cleans() == g0 && n_kbo_drop == kd0 + 1 && n_kbo_clean == kc0, "1.05 s: dropped, never sent");
        partial("ui ........", 0, 0, SW, SH);
        flush();
        host_sw = 1;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        elapse(5.0);
        g0 = cleans();
        kb_frame("overlay ...");
        flush();
        host_sw = 0;
        elapse(0.45);
        ggq("qml", "GGQ 0 settle");
        host_sw = 1;
        elapse(0.25);
        ggq("qml", "GGQ 0 settle");
        CHECK(!kbo_req && cleans() == g0 + 1 && n_kbo_clean == kc0 + 1 && !rect_full(host_last), "idle in time: the band once");
        partial("ui ........", 0, 0, SW, SH);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        cfg.kb_open_clean = 0;
        ggq("qml", "GGQ 74 set kb_open_clean 1");
        CHECK(cfg.kb_open_clean == 0, "the option stays refused");

        ggq("qml", "GGQ 75 status");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"sw\":{\"now\":1,") && strstr(js, "\"temp\":21.5") && strstr(js, "\"kbOpen\":[0,") &&
              strstr(js, "\"v\":\"" GG_VERSION "\""), "status: %.80s", js ? strstr(js, "\"sw\"") : "");
        host_temp = NAN;
        ggq("qml", "GGQ 76 status");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"temp\":null"), "unknown temperature: null");
        host_temp = 21.5;
        host_sw = 1;
        cfg.defer = 0;
    }

    printf("T44 1.8.4 every view change cleans the whole screen (a regional STD stretches the updates outside it)\n");
    {
        cfg.defer = 1;
        cfg.tr_quiet = 0.5; // as clock 12
        cfg.tr_pad = 16;
        cfg.tr_full = 0.0; // the 1.8.4 default
        hb_t = now_s();
        xo_state = 0;
        host_rc = "ok";
        host_sw = 1;
        cfg.kb_open_clean = 0;
        elapse(40.0);
        ggq("qml", "GGQ 0 settle");
        auto_req = 0;
        tr_active = 0;
        printf("     a small dialog with the panel idle: the whole screen, not the box\n");
        unsigned long g0 = cleans(), inf0 = n_inflight, up0 = n_upgrade, fb0 = n_full_busy;
        vc("SettingsOpened");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && rect_full(host_last) && n_inflight == inf0 && n_upgrade == up0 &&
              n_full_busy == fb0, "idle: one full clean (%d,%d %dx%d)", host_last.x, host_last.y, host_last.w, host_last.h);
        printf("     the same with the panel busy: the whole screen at once (it covers what is in flight), never held\n");
        elapse(5.0);
        g0 = cleans();
        vc("SettingsClosed");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        host_sw = 0;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && rect_full(host_last) && n_inflight == inf0 && n_full_busy == fb0 + 1,
              "busy: one full clean right away (inflight %lu, fullBusy +%lu)", n_inflight - inf0, n_full_busy - fb0);
        printf("     the panel's state cannot be read: the whole screen, no upgrade needed\n");
        elapse(5.0);
        g0 = cleans();
        vc("SettingsOpened");
        partial("ui ........", 300, 500, 400, 300);
        flush();
        host_sw = -1;
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(!tr_active && cleans() == g0 + 1 && rect_full(host_last) && n_upgrade == up0, "noidle: one full clean");
        host_sw = 1;
        printf("     an \"asleep\" clean never reports the panel state of an earlier clean (review of 1.8.3, CF-3)\n");
        {
            __atomic_store_n(&last_sw, 0, __ATOMIC_RELAXED); // as after a full clean over a busy panel
            __atomic_store_n(&sleeping, 1, __ATOMIC_RELEASE);
            Rect rs = {10, 10, 100, 100};
            const char *rcs = do_clean(&rs);
            __atomic_store_n(&sleeping, 0, __ATOMIC_RELEASE);
            int lsw = __atomic_load_n(&last_sw, __ATOMIC_RELAXED);
            CHECK(!strcmp(rcs, "asleep") && lsw == -2, "asleep: rc %s, panel state %d (-2: not read)", rcs, lsw);
        }
        printf("     a clock cannot bring the box back\n");
        ggq("qml", "GGQ 77 set tr_full 0.35");
        CHECK(cfg.tr_full == 0.0, "set tr_full 0.35: refused");
        ggq("qml", "GGQ 78 status");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"v\":\"" GG_VERSION "\"") && strstr(js, "\"kbOpen\":[0,") && strstr(js, "\"sw\":{"),
              "status: %.40s", js ? strstr(js, "\"v\"") : "");
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        cfg.defer = 0;
        cfg.tr_full = 0.35;
    }
    printf("T45 1.8.5 reading first: page changes are counted; from page_gap on the model decides (page_px), the "
           "page_every-th cleans anyway; leafing waits; a full refresh restarts\n");
    {
        cfg.defer = 1;
        cfg.tr_quiet = 0.5;
        cfg.tr_full = 0.0;
        cfg.page_clean = 1;
        cfg.gesture_clean = 1;
        cfg.page_gap = 2; // the 1.8.5 defaults
        cfg.page_px = cfg.turn_px = 80000;
        cfg.page_every = 6;
        cfg.page_flip = 2.0;
        cfg.page_settle = 1.5;
        cfg.auto_quiet = 0.4;
        cfg.page_quiet = 0.4; // 1.8.6: T45 tests the 1.8.5 rule (turns: page_px, auto_quiet)
        cfg.stock_gap = 3.0;
        xo_state = 0;
        host_rc = "ok";
        host_sw = 1;
        auto_req = 0;
        tr_active = 0;
        promote_live = 0;
        kb_on = 0;
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        sd(" - reset");
        sd("visibleChanged: true");
        full(); // a stock full refresh (the open's STD): the count starts at 0
        flush();
        elapse(5.0);
        CHECK(n_pchg == 0, "after a stock full refresh the page count is 0 (%d)", n_pchg);

        printf("     the model's residue after one to four turns (page cleans off while measuring)\n");
        cfg.page_clean = 0;
        long g[5] = {0, 0, 0, 0, 0};
        for (int i = 1; i <= 4; i++) {
            page_turn(i & 1);
            flush();
            elapse(3.0);
            g[i] = score().ghost;
        }
        printf("     ghost %ld %ld %ld %ld\n", g[1], g[2], g[3], g[4]);
        CHECK(g[2] > 0 && g[4] >= g[2] && g[2] >= g[1], "the residue grows with the turns (%ld %ld %ld %ld)", g[1], g[2],
              g[3], g[4]);
        cfg.page_clean = 1;
        full();
        flush();
        elapse(5.0);
        CHECK(n_pchg == 0 && score().ghost == 0, "a full refresh: count 0, no residue");

        printf("     the gap: the first change after a full refresh never cleans, whatever the model sees\n");
        cfg.page_px = cfg.turn_px = 0; // the model: any residue (hot cells still needed)
        cfg.page_every = 0;
        unsigned long c0 = cleans(), w0 = n_page_wait;
        page_turn(1);
        flush();
        CHECK(!auto_req && n_pchg == 1 && n_page_wait == w0 + 1, "turn 1: counted, no request (count %d)", n_pchg);
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        elapse(2.5);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0, "turn 1: no clean");
        page_turn(0);
        flush();
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_cap && n_pchg == 2, "turn 2: due, the model decides (count %d)",
              n_pchg);
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0 && !auto_req, "turn 2, residue above page_px: one clean, the count starts "
              "again (%d)", n_pchg);

        printf("     the model: below page_px the screen is left alone; above it the next due change cleans\n");
        elapse(5.0);
        cfg.page_px = cfg.turn_px = (int)NS; // the model never asks
        c0 = cleans();
        unsigned long nn0 = n_page_none;
        for (int i = 1; i <= 4; i++) {
            page_turn(i & 1);
            flush();
            elapse(0.6);
            ggq("qml", "GGQ 0 settle");
            elapse(2.5);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 && n_pchg == 4 && n_page_none == nn0 + 3 && !auto_req,
              "four turns, ghost below page_px: no clean (count %d, the model said no %lu times)", n_pchg, n_page_none - nn0);
        cfg.page_px = cfg.turn_px = 1;
        page_turn(1);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0, "the fifth, ghost above page_px: one clean");

        printf("     the cap: the page_every-th change cleans whatever the model says\n");
        elapse(5.0);
        cfg.page_px = cfg.turn_px = (int)NS;
        cfg.page_every = 6;
        c0 = cleans();
        for (int i = 1; i <= 5; i++) {
            page_turn(i & 1);
            flush();
            elapse(0.6);
            ggq("qml", "GGQ 0 settle");
            elapse(2.5);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 && n_pchg == 5, "five turns: no clean (count %d)", n_pchg);
        page_turn(0);
        flush();
        CHECK(auto_req && auto_kind == AK_PAGE && auto_page_cap, "the sixth asks as the cap");
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0 && !auto_req, "the sixth: one clean, the count starts again");

        printf("     leafing: a clean that falls due waits until the turning stopped\n");
        elapse(5.0);
        cfg.page_every = 5;
        c0 = cleans();
        unsigned long f0 = n_page_flip;
        for (int i = 1; i <= 2; i++) { // two turns while reading
            page_turn(i & 1);
            flush();
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
        }
        for (int i = 3; i <= 7; i++) { // then five quick turns, 0.6 s apart (the third comes 3 s after the second)
            page_turn(i & 1);
            flush();
            elapse(0.5);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0, "turn %d (count %d): no clean between the turns", i, n_pchg);
            elapse(0.1);
        }
        CHECK(auto_req && auto_kind == AK_PAGE && auto_page_cap && n_page_flip == f0 + 4 &&
                  auto_quiet_need == cfg.page_settle,
              "due while leafing (turns 4-7): the cap request waits for page_settle (flip %lu, need %.1f)",
              n_page_flip - f0, auto_quiet_need);
        elapse(0.5); // 1.1 s after the last turn
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0, "1.1 s after the last turn: still waits");
        elapse(0.5); // 1.6 s
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0, "1.6 s after the last turn: one clean, on the page the user stopped at");

        printf("     a stock full refresh restarts the count\n");
        elapse(5.0);
        c0 = cleans();
        for (int i = 1; i <= 3; i++) {
            page_turn(i & 1);
            flush();
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(n_pchg == 3, "three turns (count %d)", n_pchg);
        full(); // e.g. a zoom's own STD
        flush();
        CHECK(n_pchg == 0, "stock full refresh: count 0 (%d)", n_pchg);
        elapse(5.0);
        for (int i = 1; i <= 4; i++) {
            page_turn(i & 1);
            flush();
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 && n_pchg == 4, "four more turns: no clean (count %d)", n_pchg);
        page_turn(1);
        flush();
        elapse(0.6);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0, "the fifth after it cleans (the cap)");

        printf("     a gesture's redraw is promoted under the same rule\n");
        elapse(5.0);
        c0 = cleans();
        unsigned long gf0 = n_force, p0 = n_promote;
        for (int i = 1; i <= 4; i++) {
            gesture_skip("0.050000");
            CHECK(n_force == gf0 && n_promote == p0 && !promote_live, "gesture %d: its redraw stays a normal update (count %d)",
                  i, n_pchg);
            sd(" - reset");
            redraw_page(i & 1);
            flush();
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 && n_pchg == 4 && !auto_req, "four gestures, the model says no: no flash (count %d)", n_pchg);
        gesture_skip("0.050000");
        CHECK(n_force == gf0 + 1 && n_promote == p0 + 1, "the fifth gesture (the cap): its redraw goes out as the full STD");
        sd(" - reset");
        line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
        line(" - full update, STD");
        flush();
        promote_live = 0;
        CHECK(n_pchg == 0 && cleans() == c0, "the promoted redraw is the only flash and restarts the count (%d)", n_pchg);
        elapse(1.0);
        page_turn(0); // a quick turn 1 s after a gesture's redraw is not leafing a due clean (count 1)
        flush();
        CHECK(n_pchg == 1 && !auto_req, "a turn after it counts 1");
        elapse(5.0);
        cfg.page_px = cfg.turn_px = 1;
        gf0 = n_force;
        p0 = n_promote;
        unsigned long ps0 = n_promote_skip;
        page_turn(1);
        flush();
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(n_pchg == 0 && cleans() == c0 + 1, "page_px 1: the second turn cleans");
        gesture_skip("0.050000");
        CHECK(n_force == gf0 && n_promote_skip == ps0 + 1, "a gesture right after a full refresh (count 1): not promoted");
        sd(" - reset");
        redraw_page(0);
        flush();
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        gesture_skip("0.050000");
        CHECK(n_force == gf0 + 1 && n_promote == p0 + 1, "the next gesture (count 2), residue above page_px: promoted");
        sd(" - reset");
        line("swapBuffers: QFlags(0x1) QRegion(0,0 1620x2160) EPFramebuffer::PresentationMode::Default");
        line(" - full update, STD");
        flush();
        promote_live = 0;
        CHECK(n_pchg == 0, "its full STD restarts the count (%d)", n_pchg);
        cfg.page_px = cfg.turn_px = (int)NS;
        ps0 = n_promote_skip;
        unsigned long pn0 = n_page_none;
        gf0 = n_force;
        for (int i = 1; i <= 2; i++) {
            elapse(3.0);
            gesture_skip("0.050000");
            sd(" - reset");
            redraw_page(i & 1);
            flush();
        }
        CHECK(n_force == gf0 && n_promote_skip == ps0 + 2 && n_page_none == pn0 + 1,
              "page_px above the screen: the second gesture is left alone by the model (skips %lu, model %lu)",
              n_promote_skip - ps0, n_page_none - pn0);
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");

        printf("     no cap and page_px above the screen: never; page_every 1: every page change\n");
        cfg.page_every = 0;
        elapse(5.0);
        c0 = cleans();
        for (int i = 1; i <= 7; i++) {
            page_turn(i & 1);
            flush();
            elapse(3.0);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 && !auto_req, "seven turns, no clean");
        gf0 = n_force;
        gesture_skip("0.050000");
        CHECK(n_force == gf0, "a gesture's redraw is not promoted");
        sd(" - reset");
        redraw_page(1);
        flush();
        elapse(5.0);
        ggq("qml", "GGQ 79 set page_every 1");
        for (int i = 1; i <= 3; i++) {
            page_turn(i & 1);
            flush();
            elapse(0.6);
            ggq("qml", "GGQ 0 settle");
            CHECK(cleans() == c0 + (unsigned long)i, "page_every 1: turn %d cleans", i);
            elapse(3.0);
        }

        printf("     limits and status\n");
        ggq("qml", "GGQ 79 set page_every 99");
        ggq("qml", "GGQ 79 set page_flip 20");
        ggq("qml", "GGQ 79 set page_settle 0.1");
        ggq("qml", "GGQ 79 set page_gap 0");
        ggq("qml", "GGQ 79 set page_px -5");
        CHECK(cfg.page_every == 50 && cfg.page_flip == 10.0 && cfg.page_settle == 0.4 && cfg.page_gap == 1 &&
                  cfg.page_px == 0,
              "clamped (%d %.1f %.1f %d %d)", cfg.page_every, cfg.page_flip, cfg.page_settle, cfg.page_gap, cfg.page_px);
        ggq("qml", "GGQ 79 set page_every -3");
        ggq("qml", "GGQ 79 set page_settle 9");
        ggq("qml", "GGQ 79 set page_flip -1");
        ggq("qml", "GGQ 79 set page_gap 1e30");
        ggq("qml", "GGQ 79 set page_px 9e99");
        CHECK(cfg.page_every == 0 && cfg.page_settle == 3.0 && cfg.page_flip == 0.0 && cfg.page_gap == 50 &&
                  cfg.page_px == (int)NS,
              "clamped (%d %.1f %.1f %d %d)", cfg.page_every, cfg.page_settle, cfg.page_flip, cfg.page_gap, cfg.page_px);
        ggq("qml", "GGQ 79 set page_gap 2");
        ggq("qml", "GGQ 79 set page_px 80000");
        ggq("qml", "GGQ 79 set page_every 6");
        ggq("qml", "GGQ 79 set page_flip 2.0");
        ggq("qml", "GGQ 79 set page_settle 1.5");
        CHECK(cfg.page_gap == 2 && cfg.page_px == 80000 && cfg.page_every == 6 && cfg.page_flip == 2.0 &&
                  cfg.page_settle == 1.5,
              "the clock's values are taken");
        ggq("qml", "GGQ 78 status");
        js = slurp(TESTRUN "/status.json");
        CHECK(js && strstr(js, "\"page\":[") && strstr(js, ",2,6,80000,") && strstr(js, "\"v\":\"" GG_VERSION "\""),
              "status: %.40s", js ? strstr(js, "\"page\"") : "");
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        cfg.defer = 0;
        cfg.page_clean = 0;
        cfg.gesture_clean = 0;
    }

    printf("T46 1.8.6 every page turn that leaves a trace cleans, page_quiet after its frame (turn_px); a gesture's "
           "redraw keeps page_px; leafing still waits\n");
    {
        cfg.defer = 1;
        cfg.tr_quiet = 0.5;
        cfg.tr_full = 0.0;
        cfg.gesture_clean = 1;
        cfg.page_gap = 1; // the 1.8.6 defaults
        cfg.page_px = 80000;
        cfg.page_every = 6;
        cfg.page_flip = 2.0;
        cfg.page_settle = 1.5;
        cfg.turn_px = 6000;
        cfg.page_quiet = 0.2;
        cfg.auto_quiet = 0.4;
        cfg.stock_gap = 3.0;
        xo_state = 0;
        host_rc = "ok";
        host_sw = 1;
        auto_req = 0;
        tr_active = 0;
        promote_live = 0;
        kb_on = 0;
        elapse(40.0);
        ggq("qml", "GGQ 0 hb");
        sd(" - reset");
        sd("visibleChanged: true");
        cfg.page_clean = 0; // a known page (pattern 0), then a stock full refresh over it
        page_turn(0);
        flush();
        full();
        flush();
        elapse(5.0);
        cfg.page_clean = 1;
        CHECK(n_pchg == 0 && score().ghost == 0, "start: count 0, no residue (%d)", n_pchg);

        printf("     turn 1 after a full refresh: due at once, cleaned page_quiet after its frame\n");
        unsigned long c0 = cleans();
        page_turn(1);
        flush();
        CHECK(auto_req && auto_kind == AK_PAGE && !auto_page_cap && n_pchg == 1 && auto_quiet_need == cfg.page_quiet,
              "turn 1: a request that waits page_quiet (%.2f s, count %d)", auto_quiet_need, n_pchg);
        elapse(0.1);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 && auto_req, "0.1 s after the frame: it waits");
        elapse(0.15);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && !auto_req && n_pchg == 0,
              "0.25 s after the frame: one clean, the model counted the page's own cells (count %d)", n_pchg);

        printf("     a turn that leaves less than turn_px is left alone; the next one cleans\n");
        elapse(5.0);
        cfg.turn_px = (int)NS;
        c0 = cleans();
        unsigned long pn0 = n_page_none;
        page_turn(0);
        flush();
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 && n_page_none == pn0 + 1 && !auto_req && n_pchg == 1,
              "turn_px above the screen: no clean (count %d)", n_pchg);
        cfg.turn_px = 6000;
        elapse(5.0);
        page_turn(1);
        flush();
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1 && n_pchg == 0, "turn_px 6000: the next turn cleans (count %d)", n_pchg);

        printf("     leafing: turns 0.8 s apart wait until the turning stopped, then one clean\n");
        elapse(5.0);
        c0 = cleans();
        unsigned long f0 = n_page_flip;
        page_turn(0); // 5 s after the last change: not leafing, cleaned at once
        flush();
        elapse(0.3);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1, "the first turn cleans at once");
        for (int i = 1; i <= 3; i++) {
            elapse(0.5);
            page_turn(i & 1);
            flush();
            elapse(0.3);
            ggq("qml", "GGQ 0 settle");
        }
        CHECK(cleans() == c0 + 1 && auto_req && auto_quiet_need == cfg.page_settle && n_page_flip == f0 + 3,
              "three quick turns: no clean between them (waits %.1f s, leafing %lu)", auto_quiet_need, n_page_flip - f0);
        elapse(1.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 1, "1.3 s after the last turn: still waits");
        elapse(0.4);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0 + 2 && !auto_req && n_pchg == 0, "1.7 s after the last turn: one clean on the page the "
              "user stopped at (count %d)", n_pchg);

        printf("     a gesture's redraw keeps page_px: a residue between turn_px and page_px does not promote it\n");
        elapse(5.0);
        cfg.page_px = 200000; // above the synthetic residue of one turn (131022); turn_px is below it
        c0 = cleans();
        unsigned long gf0 = n_force, ps0 = n_promote_skip;
        cfg.page_clean = 0; // one turn's residue, no page clean
        page_turn(0);
        flush();
        elapse(3.0);
        cfg.page_clean = 1;
        long gr = score().ghost;
        CHECK(gr >= cfg.turn_px && gr < cfg.page_px, "one turn's residue %ld lies between turn_px and page_px", gr);
        gesture_skip("0.050000");
        CHECK(n_force == gf0 && n_promote_skip == ps0 + 1 && !promote_live, "the gesture's redraw is left alone");
        sd(" - reset");
        redraw_page(0);
        flush();
        elapse(3.0);
        ggq("qml", "GGQ 0 settle");
        CHECK(cleans() == c0, "no flash");
        cfg.page_px = 80000;

        printf("     the clamps, the clock's values and the status\n");
        ggq("qml", "GGQ 74 set turn_px -5");
        ggq("qml", "GGQ 73 set page_quiet 0.05");
        CHECK(cfg.turn_px == 0 && cfg.page_quiet == 0.2, "clamped (%d %.2f)", cfg.turn_px, cfg.page_quiet);
        ggq("qml", "GGQ 74 set turn_px 99999999");
        ggq("qml", "GGQ 73 set page_quiet 9");
        CHECK(cfg.turn_px == (int)NS && cfg.page_quiet == 2.0, "clamped (%d %.2f)", cfg.turn_px, cfg.page_quiet);
        ggq("qml", "GGQ 75 set page_gap 1");
        ggq("qml", "GGQ 74 set turn_px 6000");
        ggq("qml", "GGQ 73 set page_quiet 0.2");
        CHECK(cfg.page_gap == 1 && cfg.turn_px == 6000 && cfg.page_quiet == 0.2, "the clock's values are taken");
        ggq("qml", "GGQ 72 status");
        char *js6 = slurp(TESTRUN "/status.json");
        CHECK(js6 && strstr(js6, ",6000,200],") && strstr(js6, "\"v\":\"" GG_VERSION "\""), "status: %.70s",
              js6 ? strstr(js6, "\"page\"") : "");
        elapse(5.0);
        ggq("qml", "GGQ 0 settle");
        cfg.defer = 0;
        cfg.page_clean = 0;
        cfg.gesture_clean = 0;
    }
    }

    test_codrive();
    test_cd_pure();
    test_turn_full();
    test_second_pass();
    test_white_turn();
    cfg.debug = 0;
    cfg.tr_pad = 16;
    cfg.cell_min = 6;
    cfg.auto_pad = 16;
    cfg.soon_px = 6000;
    cfg.auto_quiet = 0.4;
    cfg.defer = 0;
    hb_t = 0;
    xo_state = -1;
    host_rc = "unsupported";
    cfg.quiet_s = 0;

    printf("T13 speed\n");
    full();
    text(0, 0, SW, SH);
    double t0 = now_s();
    partial("ui ........", 0, 0, SW, SH);
    fill(0, 0, SW, SH, 255);
    partial("ui ........", 0, 0, SW, SH);
    s = score();
    double t1 = now_s();
    printf("  two full-screen partial updates + score: %.1f ms (ghost=%ld)\n", (t1 - t0) * 1000, s.ghost);
    t0 = now_s();
    for (int i = 0; i < 20; i++) s = score();
    t1 = now_s();
    printf("  score: %.2f ms each\n", (t1 - t0) * 1000 / 20);

    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
