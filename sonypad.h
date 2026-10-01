// PlayStation controllers read straight over HID: DualSense, DualSense Edge and DualShock 4, on USB and
// Bluetooth. The game talks to these pads natively, so they never show up in XInput unless Steam Input
// (or a similar wrapper) translates them. Opening the device in shared mode next to the game is fine.
//
// The reader reports buttons in the XInput wButtons layout, plus two bits for the triggers, so one
// button mask works for every kind of pad.
#pragma once
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace sonypad {

const uint32_t kLT = 0x10000, kRT = 0x20000;      // triggers, above the 16 XInput button bits
typedef void (*LogFn)(const char* fmt, ...);

struct Pad {
    HANDLE h = INVALID_HANDLE_VALUE; HANDLE ev = nullptr; OVERLAPPED ov{}; bool pending = false;
    std::wstring path;                 // lower case
    USHORT pid = 0, inLen = 0; bool sense = false;
    DWORD lastReport = 0; uint32_t buttons = 0; bool seen = false, unknownLogged = false;
    uint8_t buf[1024];
};

inline bool knownPid(USHORT pid, bool& sense) {
    switch (pid) {
    case 0x0CE6: case 0x0DF2: sense = true; return true;                  // DualSense, DualSense Edge
    case 0x05C4: case 0x09CC: case 0x0BA0: sense = false; return true;    // DualShock 4 v1, v2, wireless adapter
    }
    return false;
}

// Buttons of one input report, or ~0u when it is not a report this reader knows.
//   DualSense   USB 0x01 (64 bytes): buttons at +8.  Bluetooth 0x31: at +9.  Bluetooth basic 0x01: at +5.
//   DualShock 4 USB 0x01 and Bluetooth basic 0x01: at +5.  Bluetooth 0x11: at +7.
// The three button bytes are the same on all of them: hat and face buttons, then shoulders/sticks.
inline uint32_t buttonsOf(bool sense, USHORT inLen, const uint8_t* r, DWORD n) {
    const uint8_t* b = nullptr;
    if (n < 1) return ~0u;
    if (sense) {
        if (r[0] == 0x31 && n >= 11) b = r + 9;
        else if (r[0] == 0x01 && inLen == 64 && n >= 10) b = r + 8;
        else if (r[0] == 0x01 && inLen != 64 && n >= 7) b = r + 5;
    } else {
        if (r[0] == 0x11 && n >= 9) b = r + 7;
        else if (r[0] == 0x01 && n >= 7) b = r + 5;
    }
    if (!b) return ~0u;
    static const uint32_t hat[8] = { 0x1, 0x1 | 0x8, 0x8, 0x2 | 0x8, 0x2, 0x2 | 0x4, 0x4, 0x1 | 0x4 };   // N NE E SE S SW W NW
    uint32_t m = 0;
    uint8_t h = b[0] & 0x0f; if (h < 8) m |= hat[h];
    if (b[0] & 0x10) m |= 0x4000;      // Square   -> X
    if (b[0] & 0x20) m |= 0x1000;      // Cross    -> A
    if (b[0] & 0x40) m |= 0x2000;      // Circle   -> B
    if (b[0] & 0x80) m |= 0x8000;      // Triangle -> Y
    if (b[1] & 0x01) m |= 0x0100;      // L1 -> LB
    if (b[1] & 0x02) m |= 0x0200;      // R1 -> RB
    if (b[1] & 0x04) m |= kLT;         // L2
    if (b[1] & 0x08) m |= kRT;         // R2
    if (b[1] & 0x10) m |= 0x0020;      // Create / Share -> Back
    if (b[1] & 0x20) m |= 0x0010;      // Options -> Start
    if (b[1] & 0x40) m |= 0x0040;      // L3
    if (b[1] & 0x80) m |= 0x0080;      // R3
    return m;
}

inline void drop(std::vector<Pad*>& pads, size_t i, DWORD err, LogFn log) {
    Pad* p = pads[i];
    if (p->pending) { CancelIo(p->h); DWORD got; GetOverlappedResult(p->h, &p->ov, &got, TRUE); }
    CloseHandle(p->h); CloseHandle(p->ev);
    if (log) log("controller: PlayStation pad 054C:%04X gone (error %lu)", p->pid, err);
    delete p; pads.erase(pads.begin() + i);
}

// Opens every Sony pad that is present and not open yet (at most 4). `total` counts the HID interfaces seen.
inline void scan(std::vector<Pad*>& pads, std::vector<std::wstring>& refused, LogFn log, int* total = nullptr) {
    GUID guid; HidD_GetHidGuid(&guid);
    HDEVINFO di = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; pads.size() < 4 && SetupDiEnumDeviceInterfaces(di, nullptr, &guid, i, &ifd); i++) {
        DWORD need = 0; SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> mem(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)mem.data(); det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, nullptr)) continue;
        std::wstring path = det->DevicePath;
        if (total) (*total)++;
        for (auto& ch : path) ch = (wchar_t)towlower(ch);
        if (path.find(L"054c") == std::wstring::npos) continue;            // Sony's vendor id, in USB and Bluetooth paths
        bool open = false; for (Pad* p : pads) if (p->path == path) open = true;
        if (open) continue;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            bool told = false; for (auto& r : refused) if (r == path) told = true;
            if (!told) { refused.push_back(path); if (log) log("controller: cannot open a Sony HID device (error %lu); trying again later", err); }
            continue;
        }
        HIDD_ATTRIBUTES at; at.Size = sizeof at; bool sense = false;
        PHIDP_PREPARSED_DATA pp = nullptr; HIDP_CAPS caps{};
        if (!HidD_GetAttributes(h, &at) || at.VendorID != 0x054C || !knownPid(at.ProductID, sense) || !HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); continue; }
        NTSTATUS st = HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp);
        if (st != HIDP_STATUS_SUCCESS || caps.InputReportByteLength < 10 || caps.InputReportByteLength > sizeof(Pad::buf)) { CloseHandle(h); continue; }
        Pad* p = new Pad; p->h = h; p->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        p->path = path; p->pid = at.ProductID; p->inLen = caps.InputReportByteLength; p->sense = sense; p->lastReport = GetTickCount();
        pads.push_back(p);
        if (log) log("controller: PlayStation pad 054C:%04X opened (%s, %s, %u-byte reports, version %04X)", p->pid, sense ? "DualSense" : "DualShock 4",
                     path.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos ? "Bluetooth" : "USB", (unsigned)p->inLen, (unsigned)at.VersionNumber);
    }
    SetupDiDestroyDeviceInfoList(di);
}

// Runs until *stop is set (never, in the mod). `wanted` says whether anyone listens: while it returns
// false the pads are closed. `out` gets the buttons of all pads, OR-ed. `verbose` logs button changes.
inline void run(std::atomic<uint32_t>& out, bool (*wanted)(), bool (*verbose)(), LogFn log, std::atomic<bool>* stop = nullptr) {
    std::vector<Pad*> pads; std::vector<std::wstring> refused;
    DWORD lastScan = GetTickCount() - 60000; uint32_t lastAll = 0;
    while (!stop || !stop->load()) {
        DWORD now = GetTickCount();
        if (wanted && !wanted()) {
            while (!pads.empty()) drop(pads, 0, 0, nullptr);
            out.store(0); lastAll = 0; lastScan = now - 60000;
            Sleep(500); continue;
        }
        if (now - lastScan >= 3000) { scan(pads, refused, log); lastScan = now; }
        HANDLE evs[4]; DWORD n = 0;
        for (size_t i = 0; i < pads.size(); ) {
            Pad* p = pads[i];
            if (!p->pending) {
                ResetEvent(p->ev); p->ov = OVERLAPPED{}; p->ov.hEvent = p->ev;
                DWORD got = 0;
                if (ReadFile(p->h, p->buf, p->inLen, &got, &p->ov) || GetLastError() == ERROR_IO_PENDING) p->pending = true;
                else { drop(pads, i, GetLastError(), log); continue; }
            }
            if (n < 4) evs[n++] = p->ev;
            i++;
        }
        if (!n) { out.store(0); lastAll = 0; Sleep(500); continue; }
        WaitForMultipleObjects(n, evs, FALSE, 250);
        now = GetTickCount(); uint32_t all = 0;
        for (size_t i = 0; i < pads.size(); ) {
            Pad* p = pads[i];
            if (p->pending && WaitForSingleObject(p->ev, 0) == WAIT_OBJECT_0) {
                DWORD got = 0;
                if (!GetOverlappedResult(p->h, &p->ov, &got, FALSE)) { p->pending = false; drop(pads, i, GetLastError(), log); continue; }
                p->pending = false;
                uint32_t b = buttonsOf(p->sense, p->inLen, p->buf, got);
                if (b != ~0u) {
                    if (!p->seen) {
                        p->seen = true; const uint8_t* r = p->buf;
                        if (log) log("controller: PlayStation pad 054C:%04X sends report 0x%02X (%lu bytes): %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                     p->pid, r[0], got, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
                    }
                    p->buttons = b; p->lastReport = now;
                } else if (!p->unknownLogged) {
                    p->unknownLogged = true; if (log) log("controller: PlayStation pad 054C:%04X sent an unknown report 0x%02X (%lu bytes); ignored", p->pid, got ? p->buf[0] : 0, got);
                }
            }
            if (now - p->lastReport > 1000) p->buttons = 0;       // a silent pad holds nothing
            all |= p->buttons; i++;
        }
        if (all != lastAll) { if (log && verbose && verbose()) log("controller: PlayStation buttons 0x%05X", all); lastAll = all; }
        out.store(all);
    }
    while (!pads.empty()) drop(pads, 0, 0, nullptr);
    out.store(0);
}

} // namespace sonypad
