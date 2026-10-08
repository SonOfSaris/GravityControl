// chordtest: checks the timing rules of chorded activation (chord.h) on scripted input.
// Each script is a list of polls: time in ms, modifier held, key held, and whether the chord should be held.
// Build: tests\build_chordtest.bat
#include "../chord.h"
#include <cstdio>

struct Poll { uint32_t t; bool mod, key, want; };

static int g_fail = 0;
static void run(const char* name, const Poll* p, int n) {
    chord::State s;
    for (int i = 0; i < n; i++) {
        bool got = chord::down(s, p[i].mod, p[i].key, p[i].t);
        if (got != p[i].want) { printf("FAIL %s: poll %d (t=%u mod=%d key=%d) gave %d, want %d\n", name, i, p[i].t, p[i].mod, p[i].key, got, p[i].want); g_fail++; return; }
    }
    printf("ok   %s\n", name);
}
#define RUN(name, ...) { static const Poll p[] = { __VA_ARGS__ }; run(name, p, (int)(sizeof p / sizeof p[0])); }

int main() {
    RUN("modifier first, then the key; ends when the key is released",
        {0, false, false, false}, {16, true, false, false}, {33, true, true, true}, {50, true, true, true}, {66, true, false, false});
    RUN("both in the same poll",
        {0, false, false, false}, {16, true, true, true}, {33, false, false, false});
    RUN("key one frame early, modifier follows within the grace time",
        {0, false, true, false}, {33, true, true, true}, {66, true, true, true}, {100, true, false, false});
    RUN("key held for a while, then the modifier: no chord",
        {0, false, true, false}, {100, false, true, false}, {400, true, true, false}, {450, true, true, false}, {500, false, false, false});
    RUN("releasing the modifier ends the chord; pressing it again with the key still held does not restart it",
        {0, true, false, false}, {16, true, true, true}, {33, false, true, false}, {50, true, true, false}, {66, true, false, false});
    RUN("modifier held throughout, key tapped twice: two chords",
        {0, true, false, false}, {16, true, true, true}, {33, true, false, false}, {50, true, true, true}, {66, true, false, false});
    RUN("no modifier bound (always held): the key alone",
        {0, true, false, false}, {16, true, true, true}, {600, true, true, true}, {616, true, false, false});
    RUN("modifier alone never counts",
        {0, true, false, false}, {500, true, false, false}, {1500, true, false, false}, {1516, false, false, false});
    RUN("key alone never counts",
        {0, false, true, false}, {500, false, true, false}, {516, false, false, false});
    RUN("grace time is measured from the key press: 150 ms still counts, 151 ms does not",
        {1000, false, true, false}, {1150, true, true, true}, {1200, false, false, false}, {2000, false, true, false}, {2151, true, true, false});
    RUN("timer wrap-around inside the grace time",
        {0xFFFFFFF0u, false, true, false}, {0x00000020u, true, true, true});
    printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
