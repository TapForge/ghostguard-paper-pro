// tabset: EXPERIMENT TOOL. tools/tabpoke.c without the hold: the cells of a patch file STAY written in one of
// xochitl's waveform tables (RAM only) until "rev" puts the old values back, or until xochitl restarts (it loads the
// table afresh from the waveform file: nothing here outlives a restart). Runs on the tablet (aarch64).
//   tabset <pid> <table object> <nfr> <patch file> <check|fwd|rev>
// Table object and patch file as for tabpoke: one line per cell, <cell index, decimal> <old value, hex> <new value, hex>.
//   check: prints how many cells hold the old value, the new value or anything else; writes nothing.
//   fwd:   writes the new value into every cell that holds the old one.
//   rev:   writes the old value into every cell that holds the new one.
// Nothing is written unless the header fits and EVERY cell holds its old or its new value.
// Exit: 0 done (check: all old), 10 check: all new, 11 check: mixed, 1 a write failed, 2 usage, 3 not the table / unknown cell.
// Build: python3 -m ziglang cc -target aarch64-linux-gnu.2.31 -O2 -Wall -Wextra -o tabset tabset.c
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXP 4096

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: tabset <pid> <table object> <nfr> <patch file> <check|fwd|rev>\n"); return 2; }
    int pid = atoi(argv[1]);
    uint64_t tobj = strtoull(argv[2], NULL, 16);
    long nfr = atol(argv[3]);
    int mode = !strcmp(argv[5], "check") ? 0 : !strcmp(argv[5], "fwd") ? 1 : !strcmp(argv[5], "rev") ? 2 : -1;
    if (pid <= 0 || tobj < 0x10000 || (tobj & 7) || nfr < 8 || nfr > 400 || mode < 0) { fprintf(stderr, "bad arguments\n"); return 2; }
    static long idx[MAXP];
    static uint16_t oldv[MAXP], newv[MAXP], cur[MAXP];
    int n = 0;
    FILE *f = fopen(argv[4], "r");
    if (!f) { perror(argv[4]); return 2; }
    long i;
    unsigned o, v;
    long ncell = ((nfr + 4) / 5) * 1025;
    while (fscanf(f, "%ld %x %x", &i, &o, &v) == 3) {
        if (n >= MAXP || i < 0 || i >= ncell || o > 0xffff || v > 0xffff || o == v) { fprintf(stderr, "bad patch line %d\n", n + 1); return 2; }
        idx[n] = i; oldv[n] = (uint16_t)o; newv[n] = (uint16_t)v; n++;
    }
    fclose(f);
    if (!n) { fprintf(stderr, "empty patch\n"); return 2; }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/mem", pid);
    int fd = open(path, mode ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror(path); return 2; }
    uint8_t hb[0x20];
    uint32_t h0, h4, hc;
    uint64_t tab;
    if (pread(fd, hb, sizeof hb, (off_t)tobj) != (ssize_t)sizeof hb) { fprintf(stderr, "header unreadable\n"); return 3; }
    memcpy(&h0, hb, 4); memcpy(&h4, hb + 4, 4); memcpy(&hc, hb + 0xc, 4); memcpy(&tab, hb + 0x18, 8);
    if ((long)h0 != nfr || h4 != 32 || hc != 3 || tab < 0x10000 || (tab & 1)) { fprintf(stderr, "not the table (nfr %u, %u, %u)\n", h0, h4, hc); return 3; }
    int no = 0, nn = 0, nx = 0;
    for (int k = 0; k < n; k++) {
        if (pread(fd, &cur[k], 2, (off_t)(tab + (uint64_t)idx[k] * 2)) != 2) { fprintf(stderr, "cell %ld unreadable\n", idx[k]); return 3; }
        if (cur[k] == oldv[k]) no++;
        else if (cur[k] == newv[k]) nn++;
        else {
            if (!nx) fprintf(stderr, "cell %ld holds %04x (old %04x, new %04x)\n", idx[k], cur[k], oldv[k], newv[k]);
            nx++;
        }
    }
    printf("cells %d: old %d, new %d, other %d (table cells at %llx)\n", n, no, nn, nx, (unsigned long long)tab);
    if (nx) { fprintf(stderr, "unknown cells: nothing written\n"); return 3; }
    if (!mode) return no == n ? 0 : nn == n ? 10 : 11;
    int wr = 0, bad = 0;
    for (int k = 0; k < n; k++) {
        uint16_t want = mode == 1 ? newv[k] : oldv[k], back = 0;
        off_t a = (off_t)(tab + (uint64_t)idx[k] * 2);
        if (cur[k] != want) {
            if (pwrite(fd, &want, 2, a) != 2) { bad++; continue; }
            wr++;
        }
        if (pread(fd, &back, 2, a) != 2 || back != want) bad++;
    }
    printf("%s: written %d, bad %d\n", argv[5], wr, bad);
    return bad ? 1 : 0;
}
