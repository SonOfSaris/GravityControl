// padtest: checks the PlayStation pad reader (sonypad.h) and shows what it sees.
//   padtest            report-layout checks, then 10 seconds of live reading
//   padtest <seconds>  same, with another duration (0 = checks only)
// Build: tests\build_padtest.bat
#include "../sonypad.h"
#include <cstdarg>
#include <cstdio>

static void logLine(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); fflush(stdout);
}
static bool yes() { return true; }

static int g_fail = 0;
static void expect(const char* what, uint32_t got, uint32_t want) {
    if (got != want) { printf("FAIL %s: got 0x%05X, want 0x%05X\n", what, got, want); g_fail++; }
    else printf("ok   %s (0x%05X)\n", what, got);
}

int main(int argc, char** argv) {
    using namespace sonypad;
    uint8_t r[80];
    // DualSense, USB: 0x01, sticks, triggers, counter, then the button bytes at +8.
    memset(r, 0, sizeof r); r[0] = 0x01; r[8] = 0x08; r[9] = 0;
    expect("DualSense USB idle", buttonsOf(true, 64, r, 64), 0);
    r[8] = 0x00; expect("DualSense USB d-pad up", buttonsOf(true, 64, r, 64), 0x0001);
    r[8] = 0x01; expect("DualSense USB d-pad up+right", buttonsOf(true, 64, r, 64), 0x0009);
    r[8] = 0x24; expect("DualSense USB d-pad down + Cross", buttonsOf(true, 64, r, 64), 0x1002);
    r[8] = 0x08; r[9] = 0x01 | 0x08 | 0x20; expect("DualSense USB L1 + R2 + Options", buttonsOf(true, 64, r, 64), 0x0100 | kRT | 0x0010);
    // DualSense, Bluetooth full report 0x31: one extra byte in front, buttons at +9.
    memset(r, 0, sizeof r); r[0] = 0x31; r[9] = 0x00;
    expect("DualSense BT 0x31 d-pad up", buttonsOf(true, 78, r, 78), 0x0001);
    r[9] = 0x08; r[10] = 0x40; expect("DualSense BT 0x31 L3", buttonsOf(true, 78, r, 78), 0x0040);
    // DualSense, Bluetooth basic report 0x01: buttons at +5, like a DualShock 4.
    memset(r, 0, sizeof r); r[0] = 0x01; r[5] = 0x06; r[8] = 0x08;
    expect("DualSense BT basic d-pad left", buttonsOf(true, 78, r, 78), 0x0004);
    // DualShock 4, USB and Bluetooth basic report: buttons at +5.
    memset(r, 0, sizeof r); r[0] = 0x01; r[5] = 0x00;
    expect("DualShock 4 USB d-pad up", buttonsOf(false, 64, r, 64), 0x0001);
    r[5] = 0x88; expect("DualShock 4 USB Triangle", buttonsOf(false, 64, r, 64), 0x8000);
    r[5] = 0x18; r[6] = 0x02 | 0x10; expect("DualShock 4 USB Square + R1 + Share", buttonsOf(false, 64, r, 64), 0x4000 | 0x0200 | 0x0020);
    // DualShock 4, Bluetooth full report 0x11: buttons at +7.
    memset(r, 0, sizeof r); r[0] = 0x11; r[7] = 0x04;
    expect("DualShock 4 BT 0x11 d-pad down", buttonsOf(false, 547, r, 78), 0x0002);
    r[7] = 0x08; expect("DualShock 4 BT 0x11 idle", buttonsOf(false, 547, r, 78), 0);
    // Reports this reader does not know are rejected, not misread.
    memset(r, 0, sizeof r); r[0] = 0x05;
    expect("unknown report", buttonsOf(true, 64, r, 64), ~0u);
    r[0] = 0x31; expect("short report", buttonsOf(true, 78, r, 4), ~0u);
    printf("%s\n", g_fail ? "LAYOUT CHECKS FAILED" : "layout checks passed");

    {
        std::vector<Pad*> pads; std::vector<std::wstring> refused; int total = 0;
        scan(pads, refused, logLine, &total);
        printf("HID interfaces present: %d, PlayStation pads opened: %zu\n", total, pads.size());
        while (!pads.empty()) drop(pads, 0, 0, nullptr);
    }
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    if (seconds > 0) {
        printf("reading PlayStation pads for %d s (press buttons)...\n", seconds);
        static std::atomic<uint32_t> buttons{ 0 };
        static std::atomic<bool> stop{ false };
        HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { run(buttons, yes, yes, logLine, &stop); return 0; }, nullptr, 0, nullptr);
        Sleep(seconds * 1000);
        stop.store(true); WaitForSingleObject(t, 3000);
        printf("done, last buttons 0x%05X\n", buttons.load());
    }
    return g_fail ? 1 : 0;
}
