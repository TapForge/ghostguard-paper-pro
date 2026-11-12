// ggtool: test helper for the reMarkable Paper Pro ghosting work (runs on the tablet, aarch64).
//   Touch injection into the Elan touch device (multitouch protocol B), raw device coordinates:
//     ggtool abs   <dev>                                  print the MT axis ranges
//     ggtool tap   <dev> <x> <y> [hold_ms]                 one finger down/up
//     ggtool taps  <dev> <gap_ms> <x,y> [<x,y> ...]        several taps, gap_ms between them
//     ggtool swipe <dev> <x1> <y1> <x2> <y2> <ms> [steps]  one finger drag
//     ggtool pinch <dev> <cx> <cy> <r1> <r2> <ms> [steps] [v] two fingers on a horizontal (or vertical, "v")
//                                                         line through cx,cy, from distance r1 to r2 each
//   Key injection (any evdev device; KEY_POWER = 116 on the snvs-powerkey device = a short power-button press):
//     ggtool key   <dev> <code> [hold_ms]                 one key down/up, hold_ms 1..500 (default 100)
//   Input watch (reads events like any other reader; xochitl still gets every event):
//     ggtool quiet <quiet_s> <max_s> <dev> [dev ...]        exit 0 once no device sent an event for quiet_s
//                                                         seconds of awake time, 1 when max_s passed first
//                                                         (max_s 0 = no limit), 2 on an error
//   Read-only memory access to a process (through /proc/<pid>/mem; nothing is written, nothing is stopped):
//     ggtool u64   <pid> <addr>                            print the 64-bit value at addr (hex)
//     ggtool u32   <pid> <addr>                            print the 32-bit value at addr (hex)
//     ggtool peek  <pid> <addr> <len>                      raw bytes to stdout
//     ggtool fb    <pid> <addr> <w> <h> <bpl> <step> [c]   RGB32 image -> PGM (luma) or PPM ("c"), every step-th px
//     ggtool rows  <pid> <addr> <stride> <rows> <rowlen> <ystep>  rowlen bytes of every ystep-th row (raw)
// Build: python3 -m ziglang cc -target aarch64-linux-gnu.2.31 -O2 -Wall -Wextra -o ggtool ggtool.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static double now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static int ev(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    e.input_event_sec = ts.tv_sec;
    e.input_event_usec = ts.tv_nsec / 1000;
    e.type = (unsigned short)type;
    e.code = (unsigned short)code;
    e.value = value;
    if (write(fd, &e, sizeof e) != (ssize_t)sizeof e) { perror("write"); return -1; }
    return 0;
}
static void syn(int fd) { ev(fd, EV_SYN, SYN_REPORT, 0); }

static int tracking = 900;  // ids for our touches; the kernel only needs them to differ from -1

static void finger(int fd, int slot, int id, int x, int y, int first) {
    ev(fd, EV_ABS, ABS_MT_SLOT, slot);
    if (first) {
        ev(fd, EV_ABS, ABS_MT_TRACKING_ID, id);
        ev(fd, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        ev(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
        ev(fd, EV_ABS, ABS_MT_PRESSURE, 60);
    }
    ev(fd, EV_ABS, ABS_MT_POSITION_X, x);
    ev(fd, EV_ABS, ABS_MT_POSITION_Y, y);
}
static void lift(int fd, int slot) {
    ev(fd, EV_ABS, ABS_MT_SLOT, slot);
    ev(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
}

static int opendev(const char *p) {
    int fd = open(p, O_WRONLY);
    if (fd < 0) { perror(p); exit(2); }
    return fd;
}

static void do_tap(int fd, int x, int y, int hold) {
    finger(fd, 0, ++tracking, x, y, 1);
    ev(fd, EV_KEY, BTN_TOUCH, 1);
    syn(fd);
    int n = hold / 20;
    for (int i = 0; i < n; i++) { msleep(20); finger(fd, 0, tracking, x, y, 0); syn(fd); }
    msleep(hold - n * 20 > 0 ? hold - n * 20 : 1);
    lift(fd, 0);
    ev(fd, EV_KEY, BTN_TOUCH, 0);
    syn(fd);
}

static int abs_info(const char *dev) {
    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror(dev); return 2; }
    static const struct { int code; const char *name; } ax[] = {
        {ABS_MT_SLOT, "slot"}, {ABS_MT_TOUCH_MAJOR, "major"}, {ABS_MT_POSITION_X, "x"}, {ABS_MT_POSITION_Y, "y"},
        {ABS_MT_TOOL_TYPE, "tool"}, {ABS_MT_TRACKING_ID, "id"}, {ABS_MT_PRESSURE, "pressure"}, {ABS_MT_DISTANCE, "distance"}};
    for (size_t i = 0; i < sizeof ax / sizeof ax[0]; i++) {
        struct input_absinfo a;
        if (ioctl(fd, EVIOCGABS(ax[i].code), &a) == 0)
            printf("%s: min %d max %d fuzz %d flat %d res %d value %d\n", ax[i].name, a.minimum, a.maximum, a.fuzz, a.flat,
                   a.resolution, a.value);
    }
    char name[128] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    printf("name: %s\n", name);
    close(fd);
    return 0;
}

static int memfd(const char *pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%s/mem", pid);
    int fd = open(p, O_RDONLY);
    if (fd < 0) { perror(p); exit(2); }
    return fd;
}
static int rd(int fd, unsigned long long addr, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, (char *)buf + got, len - got, (off_t)(addr + got));
        if (r <= 0) { fprintf(stderr, "read failed at 0x%llx: %s\n", addr + got, r < 0 ? strerror(errno) : "eof"); return -1; }
        got += (size_t)r;
    }
    return 0;
}
static unsigned long long hex(const char *s) { return strtoull(s, NULL, 0); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see the header of ggtool.c\n"); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "abs")) return abs_info(argv[2]);
    if (!strcmp(cmd, "quiet") && argc >= 5) {
        double quiet = atof(argv[2]), maxs = atof(argv[3]);
        int n = argc - 4;
        if (quiet <= 0 || maxs < 0 || n > 8) { fprintf(stderr, "bad quiet/max or more than 8 devices\n"); return 2; }
        struct pollfd pf[8];
        for (int i = 0; i < n; i++) {
            pf[i].fd = open(argv[4 + i], O_RDONLY | O_NONBLOCK);
            if (pf[i].fd < 0) { perror(argv[4 + i]); return 2; }
            pf[i].events = POLLIN;
        }
        // CLOCK_MONOTONIC stops while the tablet is suspended: only awake time counts
        double t0 = now_mono(), last = t0;
        for (;;) {
            double t = now_mono();
            if (t - last >= quiet) return 0;
            if (maxs > 0 && t - t0 >= maxs) return 1;
            double w = quiet - (t - last);
            if (maxs > 0 && t0 + maxs - t < w) w = t0 + maxs - t;
            int r = poll(pf, (nfds_t)n, (int)(w * 1000.0) + 1);
            if (r < 0) {
                if (errno == EINTR) continue;
                perror("poll");
                return 2;
            }
            if (r == 0) continue;
            for (int i = 0; i < n; i++) {
                if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { fprintf(stderr, "%s: device error\n", argv[4 + i]); return 2; }
                if (pf[i].revents & POLLIN) {
                    struct input_event e[64];
                    while (read(pf[i].fd, e, sizeof e) > 0) {}
                }
            }
            last = now_mono();
        }
    }
    if (!strcmp(cmd, "key") && argc >= 4) {
        int code = atoi(argv[3]), hold = argc > 4 ? atoi(argv[4]) : 100;
        // short presses only: a long power-button press is a different gesture
        if (code <= 0 || code >= KEY_MAX || hold < 1 || hold > 500) { fprintf(stderr, "bad key code or hold\n"); return 1; }
        int fd = opendev(argv[2]);
        ev(fd, EV_KEY, code, 1);
        syn(fd);
        msleep(hold);
        ev(fd, EV_KEY, code, 0);
        syn(fd);
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "tap") && argc >= 5) {
        int fd = opendev(argv[2]);
        do_tap(fd, atoi(argv[3]), atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 80);
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "taps") && argc >= 5) {
        int fd = opendev(argv[2]);
        int gap = atoi(argv[3]);
        for (int i = 4; i < argc; i++) {
            int x, y;
            if (sscanf(argv[i], "%d,%d", &x, &y) != 2) { fprintf(stderr, "bad point %s\n", argv[i]); return 1; }
            do_tap(fd, x, y, 70);
            if (i + 1 < argc) msleep(gap);
        }
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "swipe") && argc >= 8) {
        int fd = opendev(argv[2]);
        int x1 = atoi(argv[3]), y1 = atoi(argv[4]), x2 = atoi(argv[5]), y2 = atoi(argv[6]), ms = atoi(argv[7]);
        int steps = argc > 8 ? atoi(argv[8]) : 16;
        if (steps < 2) steps = 2;
        finger(fd, 0, ++tracking, x1, y1, 1);
        ev(fd, EV_KEY, BTN_TOUCH, 1);
        syn(fd);
        for (int i = 1; i <= steps; i++) {
            msleep(ms / steps);
            finger(fd, 0, tracking, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0);
            syn(fd);
        }
        msleep(30);
        lift(fd, 0);
        ev(fd, EV_KEY, BTN_TOUCH, 0);
        syn(fd);
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "pinch") && argc >= 8) {
        int fd = opendev(argv[2]);
        int cx = atoi(argv[3]), cy = atoi(argv[4]), r1 = atoi(argv[5]), r2 = atoi(argv[6]), ms = atoi(argv[7]);
        int steps = argc > 8 ? atoi(argv[8]) : 20;
        int vert = argc > 9 && argv[9][0] == 'v';
        if (steps < 2) steps = 2;
        int ida = ++tracking, idb = ++tracking;
#define PT(r, s) (vert ? cx : cx + (s) * (r)), (vert ? cy + (s) * (r) : cy)
        finger(fd, 0, ida, PT(r1, -1), 1);
        ev(fd, EV_KEY, BTN_TOUCH, 1);
        syn(fd);
        msleep(15);
        finger(fd, 1, idb, PT(r1, 1), 1);
        syn(fd);
        for (int i = 1; i <= steps; i++) {
            msleep(ms / steps);
            int r = r1 + (r2 - r1) * i / steps;
            finger(fd, 0, ida, PT(r, -1), 0);
            finger(fd, 1, idb, PT(r, 1), 0);
            syn(fd);
        }
#undef PT
        msleep(40);
        lift(fd, 1);
        syn(fd);
        msleep(15);
        lift(fd, 0);
        ev(fd, EV_KEY, BTN_TOUCH, 0);
        syn(fd);
        close(fd);
        return 0;
    }
    if ((!strcmp(cmd, "u64") || !strcmp(cmd, "u32")) && argc >= 4) {
        int fd = memfd(argv[2]);
        unsigned long long v = 0;
        if (rd(fd, hex(argv[3]), &v, cmd[1] == '6' ? 8 : 4)) return 3;
        printf("0x%llx\n", v);
        return 0;
    }
    if (!strcmp(cmd, "peek") && argc >= 5) {
        int fd = memfd(argv[2]);
        unsigned long long a = hex(argv[3]);
        size_t len = (size_t)hex(argv[4]);
        char buf[65536];
        while (len) {
            size_t n = len < sizeof buf ? len : sizeof buf;
            if (rd(fd, a, buf, n)) return 3;
            fwrite(buf, 1, n, stdout);
            a += n;
            len -= n;
        }
        return 0;
    }
    if (!strcmp(cmd, "rows") && argc >= 8) {
        int fd = memfd(argv[2]);
        unsigned long long a = hex(argv[3]);
        long stride = atol(argv[4]), rows = atol(argv[5]), rowlen = atol(argv[6]), ystep = atol(argv[7]);
        if (rowlen <= 0 || rowlen > (1 << 20) || ystep <= 0) return 1;
        char *buf = malloc((size_t)rowlen);
        for (long y = 0; y < rows; y += ystep) {
            if (rd(fd, a + (unsigned long long)(y * stride), buf, (size_t)rowlen)) return 3;
            fwrite(buf, 1, (size_t)rowlen, stdout);
        }
        return 0;
    }
    if (!strcmp(cmd, "fb") && argc >= 8) {
        int fd = memfd(argv[2]);
        unsigned long long a = hex(argv[3]);
        int w = atoi(argv[4]), h = atoi(argv[5]), bpl = atoi(argv[6]), step = atoi(argv[7]);
        int color = argc > 8 && argv[8][0] == 'c';
        if (w <= 0 || h <= 0 || bpl < w * 4 || step <= 0) return 1;
        int ow = (w + step - 1) / step, oh = (h + step - 1) / step;
        printf("%s\n%d %d\n255\n", color ? "P6" : "P5", ow, oh);
        uint8_t *row = malloc((size_t)bpl), *out = malloc((size_t)ow * 3);
        for (int y = 0; y < h; y += step) {
            if (rd(fd, a + (unsigned long long)y * (unsigned long long)bpl, row, (size_t)w * 4)) return 3;
            int k = 0;
            for (int x = 0; x < w; x += step) {
                uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                if (color) { out[k++] = r; out[k++] = g; out[k++] = b; }
                else out[k++] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
            }
            fwrite(out, 1, (size_t)k, stdout);
        }
        return 0;
    }
    fprintf(stderr, "unknown or incomplete command %s\n", cmd);
    return 1;
}
