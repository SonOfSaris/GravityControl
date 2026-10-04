// GravityControl 1.2.1 - arbitrary gravity direction for CONTROL Resonant (Steam build 25472515 up to game version 1.4.1).
//
// How it works (see _modding-research/NOTES.md):
//   The player's "down" is the quaternion in coregame::component::MovementPlane. The game's own
//   gravity_anomaly::movement_plane_interpolation system slerps it to a target held in
//   MovementPlaneInterpolation and re-poses the character controller; one helper ("start")
//   fills that target (and the camera's). We call that helper ourselves with a target built from
//   a sweep along the player's facing: hit -> new up = hit normal (rotate back onto the wall you
//   face); no hit -> new up = facing (rotate forward over the edge you just crested).
//   Row pointers come from two per-entity system bodies we hook (movement_plane_interpolation and
//   update_active_anomaly); the start helper is hooked too, so the game's own calls can be held
//   off while we own the plane.
//
// Loads through crloader (crmods\GravityControl\gravitycontrol.dll). Next to the DLL: the settings in
// gravitycontrol_config.ini, the log in gravitycontrol.log, and gravitycontrol.menu.json, which puts the
// settings on the Options > MODS page of Mod Settings Menu (what the player changes there lands in
// ModMenuConfig\gravitycontrol.ini and wins over the ini).

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <share.h>
#include <Xinput.h>
#include <tlhelp32.h>
#include "MinHook.h"
#include "sonypad.h"

namespace {

const char kVersion[] = "1.2.1";
const wchar_t kTitle[] = L"GravityControl 1.2.1";       // title of the message boxes

// ---------------------------------------------------------------- small math
struct Quat { float x, y, z, w; };
struct Vec  { float x, y, z, w; };

Vec vec(float x, float y, float z) { return Vec{ x, y, z, 0.f }; }
Vec add(Vec a, Vec b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec sub(Vec a, Vec b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec mul(Vec a, float s) { return vec(a.x * s, a.y * s, a.z * s); }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float len(Vec a) { return sqrtf(dot(a, a)); }
Vec norm(Vec a) { float l = len(a); return l > 1e-6f ? mul(a, 1.f / l) : vec(0, 1, 0); }

// v rotated by q (x,y,z,w), standard Hamilton convention.
Vec rotate(const Quat& q, Vec v) {
    Vec u = vec(q.x, q.y, q.z);
    Vec t = mul(cross(u, v), 2.f);
    return add(add(v, mul(t, q.w)), cross(u, t));
}

// Minimal rotation taking unit vector a onto unit vector b (no twist about a).
Quat fromTo(Vec a, Vec b, Vec fallbackAxis) {
    float d = dot(a, b);
    Vec c = cross(a, b);
    Quat q;
    if (d < -0.9999f) {                       // opposite: 180 degrees about any axis perpendicular to a
        Vec ax = cross(a, fallbackAxis);
        if (len(ax) < 1e-4f) ax = cross(a, vec(1, 0, 0));
        ax = norm(ax);
        q = Quat{ ax.x, ax.y, ax.z, 0.f };
    } else {
        q = Quat{ c.x, c.y, c.z, 1.f + d };
    }
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q.x /= l; q.y /= l; q.z /= l; q.w /= l;
    return q;
}

// ---------------------------------------------------------------- settings / log
struct Settings {
    int enabled = 1;            // 0 = the mod does nothing (and gravity goes back to normal)
    int keyShift = VK_RSHIFT;   // tap = shift, hold HoldSeconds = reset (like the pad button)
    float keyHoldSeconds = 1.f;
    float rayLength = 5.f;
    float rayRadius = 0.15f;
    int diagnostics = 0;
    int versionWarning = 1;     // 1 = message box at start when the mod could not hook the game, fully or partly
    int requireUnlock = 1;      // 1 = shifting needs the game's Gravity Anomaly ability unlocked
    // edge probe (used when nothing is ahead)
    float edgeAhead = 1.2f;     // metres ahead of the chest to probe for the surface past the edge
    float edgeDepth = 2.5f;     // how far below floor level the down probe reaches
    float floorTol = 0.35f;     // a down hit within the ray height + this is "floor continues"
    float snapDeg = 15.f;       // snap the new up to a world axis when within this angle (0 = off)
    int axisOnly = 1;           // 1 = the new up is always the nearest world axis
    // controller
    int padEnabled = 1;
    int padButton = XINPUT_GAMEPAD_DPAD_UP;   // XInput wButtons mask; 0x10000 = LT / L2, 0x20000 = RT / R2; 0 = none
    float padHoldSeconds = 1.f;
    // blockers: collision-matrix bits (PlayerLayer, L) cleared so the player's capsule ignores layer L
    int ignoreLayers[8] = {}; int nIgnore = 0;
} g_cfg;

// Fixed engine facts (build 25472515), confirmed in play: the capsule clearance sweep's query flags, the
// facing axis (+Z of the world transform) and the layout of the engine's sweep hit record.
const unsigned kQueryFlags = 0x45;
const float kForwardSign = 1.f;
const int kOffHit = 0x70, kOffEntity = 0x14, kOffDistance = 0x20, kOffPosition = 0x30, kOffNormal = 0x50;   // entity: u32 index, u32 generation
// Transition: the game's own angle-based duration (-1), its eased blend (not an instant snap) and its
// default pivot (packed pivot argument 0 = capsule middle).
const float kDurationOverride = -1.f;
const uint8_t kLinear = 0;
const uint64_t kPivot = 0;
// Settings until 1.1, fixed since: the ray starts 1.2 m above the feet, Gravity Anomaly is ability 7 in the
// ability database (the id the game's own anomaly scan checks) and the player's capsule is physics layer 8.
// A second ray always looks straight up, and inputs are always ignored while a game menu is open.
const float kChestHeight = 1.2f;
const int kAbilityId = 7;
const int kPlayerLayer = 8;

std::wstring g_dir;      // folder of this DLL, with trailing backslash
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;
DWORD g_t0 = 0;

void logf(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%7.3f] ", (GetTickCount() - g_t0) / 1000.0);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

int iniInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& file) {
    wchar_t buf[64]; GetPrivateProfileStringW(sec, key, L"", buf, 64, file.c_str());
    if (!buf[0]) return def;
    return (int)wcstol(buf, nullptr, 0);
}
float iniFloat(const wchar_t* sec, const wchar_t* key, float def, const std::wstring& file) {
    wchar_t buf[64]; GetPrivateProfileStringW(sec, key, L"", buf, 64, file.c_str());
    if (!buf[0]) return def;
    return (float)wcstod(buf, nullptr);
}

void iniList(const wchar_t* sec, const wchar_t* key, int* out, int& n, int cap, const std::wstring& file) {
    wchar_t buf[128]; GetPrivateProfileStringW(sec, key, L"", buf, 128, file.c_str());
    n = 0; const wchar_t* q = buf;
    while (*q && n < cap) { wchar_t* e; long v = wcstol(q, &e, 0); if (e == q) { q++; continue; } out[n++] = (int)v; q = e; }
}

// Mod Settings Menu pad codes are 256 + n, n in this order: A B X Y LB RB LT RT Back Start L3 R3 Up Down
// Left Right. The same buttons as masks (XInput bits, the triggers above them):
const uint32_t kMenuPad[16] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, sonypad::kLT, sonypad::kRT,
                                0x0020, 0x0010, 0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008 };
int g_menuValues = 0;    // how many settings the MODS menu supplied on the last load

// Mod Settings Menu saves what the player changes on Options > MODS as numbers under [Settings] in
// ModMenuConfig\<id>.ini, in the folder of the descriptor (gravitycontrol.menu.json, next to this DLL).
bool menuNum(const wchar_t* key, double& v, const std::wstring& file) {
    wchar_t buf[64]; GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, file.c_str());
    if (!buf[0]) return false;
    if (!_wcsicmp(buf, L"true")) { v = 1; return true; }
    if (!_wcsicmp(buf, L"false")) { v = 0; return true; }
    wchar_t* e = nullptr; v = wcstod(buf, &e);
    return e != buf;
}
float clampf(double v, double lo, double hi) { return (float)(v < lo ? lo : v > hi ? hi : v); }

// A value saved by the menu wins over the ini; a setting the menu never saved keeps its ini value.
int applyMenu(Settings& c) {
    std::wstring f = g_dir + L"ModMenuConfig\\gravitycontrol.ini";
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    int n = 0; double v;
    if (menuNum(L"enabled", v, f))            { c.enabled = v != 0; n++; }
    if (menuNum(L"key", v, f))                { int k = (int)v; if (k == 0 || (k >= 3 && k <= 254)) { c.keyShift = k; n++; } }
    if (menuNum(L"pad_button", v, f))         { int k = (int)v; if (k == 0) { c.padButton = 0; n++; } else if (k >= 256 && k < 272) { c.padButton = (int)kMenuPad[k - 256]; n++; } }
    if (menuNum(L"hold_ms", v, f))            { c.keyHoldSeconds = c.padHoldSeconds = clampf(v, 100, 5000) / 1000.f; n++; }
    if (menuNum(L"ray_length", v, f))         { c.rayLength = clampf(v, 1, 100); n++; }
    if (menuNum(L"axis_only", v, f))          { c.axisOnly = v != 0; n++; }
    if (menuNum(L"require_unlock", v, f))     { c.requireUnlock = v != 0; n++; }
    if (menuNum(L"snap_degrees", v, f))       { c.snapDeg = clampf(v, 0, 90); n++; }
    if (menuNum(L"ray_radius_cm", v, f))      { c.rayRadius = clampf(v, 1, 100) / 100.f; n++; }
    if (menuNum(L"edge_ahead_cm", v, f))      { c.edgeAhead = clampf(v, 10, 1000) / 100.f; n++; }
    if (menuNum(L"edge_depth_cm", v, f))      { c.edgeDepth = clampf(v, 10, 2000) / 100.f; n++; }
    if (menuNum(L"floor_tolerance_cm", v, f)) { c.floorTol = clampf(v, 0, 300) / 100.f; n++; }
    if (menuNum(L"version_warning", v, f))    { c.versionWarning = v != 0; n++; }
    if (menuNum(L"diagnostics", v, f))        { c.diagnostics = v != 0; n++; }
    return n;
}

void loadSettings() {
    Settings c;                               // from the defaults every time: a line removed from a file goes back to its default
    std::wstring f = g_dir + L"gravitycontrol_config.ini";
    c.enabled    = iniInt(L"General", L"Enabled", c.enabled, f);
    c.keyShift   = iniInt(L"Keys", L"Shift", c.keyShift, f);
    c.keyHoldSeconds = iniFloat(L"Keys", L"HoldSeconds", c.keyHoldSeconds, f);
    c.rayLength  = iniFloat(L"Ray", L"Length", c.rayLength, f);
    c.rayRadius  = iniFloat(L"Ray", L"Radius", c.rayRadius, f);
    c.diagnostics= iniInt(L"General", L"Diagnostics", c.diagnostics, f);
    c.requireUnlock = iniInt(L"General", L"RequireAnomalyUnlocked", c.requireUnlock, f);
    c.versionWarning = iniInt(L"General", L"VersionWarning", c.versionWarning, f);
    c.edgeAhead  = iniFloat(L"Edge", L"Ahead", c.edgeAhead, f);
    c.edgeDepth  = iniFloat(L"Edge", L"Depth", c.edgeDepth, f);
    c.floorTol   = iniFloat(L"Edge", L"FloorTolerance", c.floorTol, f);
    c.snapDeg    = iniFloat(L"Edge", L"SnapDegrees", c.snapDeg, f);
    c.axisOnly   = iniInt(L"Edge", L"AxisOnly", c.axisOnly, f);
    c.padEnabled = iniInt(L"Controller", L"Enabled", c.padEnabled, f);
    c.padButton  = iniInt(L"Controller", L"Button", c.padButton, f);
    c.padHoldSeconds = iniFloat(L"Controller", L"HoldSeconds", c.padHoldSeconds, f);
    iniList(L"Blockers", L"IgnoreLayers", c.ignoreLayers, c.nIgnore, 8, f);
    g_menuValues = applyMenu(c);
    g_cfg = c;
}

void logSettings() {
    logf("settings: Enabled=%d Key=0x%X PadButton=0x%X Hold=%.1f/%.1f Length=%.1f Radius=%.2f AxisOnly=%d RequireUnlock=%d Diag=%d (%d from the MODS menu)",
         g_cfg.enabled, g_cfg.keyShift, g_cfg.padEnabled ? g_cfg.padButton : 0, g_cfg.keyHoldSeconds, g_cfg.padHoldSeconds,
         g_cfg.rayLength, g_cfg.rayRadius, g_cfg.axisOnly, g_cfg.requireUnlock, g_cfg.diagnostics, g_menuValues);
    if (g_cfg.nIgnore) logf("blockers: IgnoreLayers count=%d (player layer %d)", g_cfg.nIgnore, kPlayerLayer);
}

// ---------------------------------------------------------------- game functions
// Byte patterns of function prologues; each must be found exactly once in .text. An anchor may carry the
// pattern of more than one game build (the first that matches wins). The mod cannot run without the
// required ones; a missing optional one only switches its part of the mod off. A function that another mod
// hooked first no longer starts with its pattern; it is found by the rest of it (findHooked).
struct Anchor {
    const char* name; bool required; const char* patterns[2]; uintptr_t found;
    std::string sharedWith;     // set when another mod's hook was already on the function: that mod's file name
    bool changed;               // the function is there, but its start was rewritten in a way this mod cannot share
};
Anchor g_anchors[] = {
    { "mpi_body", true,
                    { "4c 8b dc 4d 89 4b 20 4d 89 43 18 49 89 53 10 53 56 57 41 54 41 55 41 56 41 57 48 81 ec e0 02 00",
                      nullptr }, 0 },
    { "start_helper", true,
                    { "4c 8b dc 49 89 5b 20 c5 fa 11 54 24 18 49 89 53 10 55 56 41 56 48 81 ec",
                      nullptr }, 0 },
    { "sweep", true,
                    { "4c 8b dc 49 89 5b 10 49 89 73 18 49 89 7b 20 41 54 41 56 41 57 48 81 ec 20 02 00 00 c4 c1 78 29",
                      nullptr }, 0 },
    { "type_lookup", false,
                    { "8b c2 48 8d 14 40 48 8b 81 98 01 00 00 0f b7 04 90 c3 cc cc cc cc cc cc",
                      nullptr }, 0 },
    { "uaa_body", true,
                    { "48 8b c4 4c 89 48 20 4c 89 40 18 48 89 50 10 48 89 48 08 53 56 57 41 54 41 55 41 56 41 57 48 81 ec 00 03 00 00 "
                      "c5 f8 29 70 b8 c5 f8 29 78 a8 c5 78 29 40 98 c5 78 29 48 88 c5 78 29 90 78 ff ff ff c5 78 29 98 68 ff ff ff "
                      "c5 78 29 a0 58 ff ff ff c5 78 29 a8 48 ff ff ff c5 78 29 b0 38 ff ff ff c5 78 29 b8 28 ff ff ff 4d 8b f8 48 8b d9 "
                      "c5 fc 10 09 c5 fc 11 88 08 fe ff ff c5 fc 10 51 20",
                      nullptr }, 0 },
    { "ability_unlocked", false,
                    { "40 57 48 83 ec 30 4c 8b 09 48 8b fa 8b 41 08 48 8d 04 40 48 c1 e0 05 49 03 c1 4c 3b c8 74 10 90 45",
                      "48 83 ec 28 8b 41 08 4c 8b ca 48 8b 11 48 8d 04 40 48 c1 e0 05 48 03 c2 48 3b d0 74 12 0f 1f 00 44 38 42 05 74 10 48 83 c2 60" }, 0 },      // build 25472515, then the 2026-10-01 update
    { "menu_broadcast", false,
                    { "48 89 5c 24 10 56 48 83 ec 30 80 7a 38 00 49 8b f0 48 8b da 0f 84 a2 00 00 00 48 8d 4c 24 20",
                      nullptr }, 0 },
    // bool layersCollide(a, b): ends in "lea rdx, [rip + matrix]"; the pattern stops at that lea (+59)
    { "layer_check", false,
                    { "8b 41 04 85 c0 74 1d 3b 42 04 75 18 8b 41 08 85 c0 74 0e 44 8b 42 08 45 85 c0 74 05 44 85 c0 74 03 32 c0 c3 "
                      "44 8b 02 8b 11 41 f7 d0 f7 d2 41 8b c8 41 3b d0 44 0f 47 c2 0f 42 ca 48 8d 15",
                      nullptr }, 0 },
};
enum { A_MPI = 0, A_START, A_SWEEP, A_TYPE, A_UAA, A_UNLOCK, A_MENU, A_LAYERS };

// void start(const Quat* target, uint64 entity, float durationOverride, uint8 linear,
//            void* playerAnomalyComponentRow, Quat* movementPlane, uint8* mpiRow, uint8* cameraRow)
typedef void (*StartFn)(const Quat*, uint64_t, float, uint8_t, void*, Quat*, uint8_t*, uint8_t*);
// per-entity body of gravity_anomaly::movement_plane_interpolation
typedef void (*BodyFn)(uint8_t (*blocks)[32], void* p2, void** scenePtr, float* dt, void* p5);
// per-entity body of gravity_anomaly::update_active_anomaly (many args; all forwarded untouched)
typedef void (*UaaFn)(uint8_t (*blocks)[32], uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
// PhysicsSweepHit* sweep(PhysicsSweepHit* out, PhysicsScene* scene, float radius, const Vec* from, const Vec* to, const void* opts)
typedef void* (*SweepFn)(void*, void*, float, const Vec*, const Vec*, const void*);
typedef uint16_t (*TypeLookupFn)(void* scene, uint64_t entity);
// bool isAbilityUnlocked(const AbilityDatabase* env, const WorldStateDatabase* env, uint8 abilityId): looks the ability
// up by id, then its unlock world fact; missing entries count as unlocked. Result in al.
typedef uint64_t (*UnlockFn)(void*, void*, uint8_t);
// per-player body of heron::ui_broadcast_current_menu::broadcast (runs every variable update):
// (entity ref, BroadcastState* env, UI world). The state keeps the current menu name at +0x3c with a
// "has current menu" flag at +0x54; menus that leave the game world visible (wardrobe, shop, talents,
// ability/melee select) set it when they open and clear it when they close.
typedef void (*MenuFn)(void*, uint8_t*, void*);

StartFn g_origStart = nullptr;
BodyFn g_origBody = nullptr;
UaaFn g_origUaa = nullptr;
SweepFn g_sweep = nullptr;
TypeLookupFn g_typeLookup = nullptr;
UnlockFn g_isUnlocked = nullptr;
MenuFn g_origMenu = nullptr;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputGetState = nullptr;
std::atomic<uint32_t> g_hidButtons{ 0 };   // PlayStation pads read over HID (sonypad.h), in the XInput button layout
std::atomic<uint32_t> g_xiSlots{ 0 };      // XInput slots that have a pad, found by xinputThread
HMODULE g_self = nullptr;
HANDLE g_instanceMutex = nullptr;            // held while this copy is the one in charge

// ---------------------------------------------------------------- state (game thread only)
struct State {
    bool engaged = false;      // we own the plane; the game's start calls are dropped
    bool releasing = false;    // reset transition in flight
    int traceFrames = 0;       // log the next N body calls in detail
    Quat ourTarget{ 0, 0, 0, 1 };
    // rows captured from update_active_anomaly's view
    uint8_t* cam = nullptr; uint8_t* pad = nullptr; uint8_t* rules = nullptr; int64_t uaaIdx = -1;
    void* worldDb = nullptr; void* abilityDb = nullptr; DWORD uaaTick = 0;   // env pointers seen by update_active_anomaly
    uint8_t* menuState = nullptr; DWORD menuTick = 0; int menuOpenLast = -1; DWORD menuIgnoreLog = 0;
    unsigned startCalls = 0, bodyCalls = 0, uaaCalls = 0, dropped = 0;
    bool keyShiftDown = false, keyConsumed = false; DWORD keyDownAt = 0;
    bool padDown = false, padConsumed = false; DWORD padDownAt = 0;
    DWORD lastCfgCheck = 0; FILETIME cfgTime{}, menuCfgTime{}; int enabledLast = -1;
} g_s;

const Quat kIdentity{ 0, 0, 0, 1 };

bool sameQuat(const Quat& a, const Quat& b) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;      // same rotation when |dot| is 1 (q and -q are equal)
    return fabsf(d) > 0.99999f;
}

// ---------------------------------------------------------------- hooks
void hkStart(const Quat* target, uint64_t entity, float duration, uint8_t linear,
             void* pac, Quat* plane, uint8_t* mpi, uint8_t* cam) {
    g_s.startCalls++;
    logf("game start call #%u: target (%.3f %.3f %.3f %.3f) entity %llx duration %.2f linear %d cam %p%s",
         g_s.startCalls, target->x, target->y, target->z, target->w, (unsigned long long)entity, duration, linear, cam,
         g_s.engaged ? " (engaged)" : "");
    if (g_s.engaged && linear) {
        // An instant snap from the game (fall respawn, teleport, game-flow transition): it must win, and it
        // ends our ownership of the plane. Reset still works afterwards if the plane is left turned.
        g_s.engaged = false; g_s.releasing = false;
        logf("game snapped the plane (respawn/teleport): releasing it (dropped %u default-plane calls while engaged)", g_s.dropped);
        g_s.dropped = 0;
    } else if (g_s.engaged && !sameQuat(*target, g_s.ourTarget)) {
        if (!sameQuat(*target, kIdentity)) {
            // The game wants a real anomaly's plane (the player entered one): hand the plane back and let
            // its own animated transition run. Leaving the anomaly then returns to normal gravity as usual.
            g_s.engaged = false; g_s.releasing = false;
            logf("handing the plane to the game's anomaly (dropped %u default-plane calls while engaged)", g_s.dropped);
            g_s.dropped = 0;
        } else {
            g_s.dropped++; return;   // the game's "back to default" while we own the plane
        }
    }
    g_origStart(target, entity, duration, linear, pac, plane, mpi, cam);
}

// -1 = cannot tell (the broadcast system has not run lately), 0 = no menu, 1 = a menu is open
int menuOpen() {
    if (!g_s.menuState || GetTickCount() - g_s.menuTick > 3000) return -1;
    return g_s.menuState[0x54] ? 1 : 0;
}
void menuName(const uint8_t* st, char* out, size_t n) {
    // the name is a small engine string at +0x3c; show its printable bytes
    size_t k = 0;
    for (int i = 0x3c; i < 0x58 && k + 1 < n; i++) { uint8_t c = st[i]; out[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
    out[k] = 0;
}
void hkMenu(void* ent, uint8_t* state, void* world) {
    g_s.menuState = state; g_s.menuTick = GetTickCount();
    g_origMenu(ent, state, world);
    int open = state[0x54] ? 1 : 0;
    if (open != g_s.menuOpenLast) {
        char nm[40]; menuName(state, nm, sizeof nm);
        if (open) logf("menu opened: [%s] (shift/reset blocked)", nm);
        else logf("menu closed");
        g_s.menuOpenLast = open;
    }
}

void hkUaa(uint8_t (*blocks)[32], uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
           uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12) {
    g_s.uaaCalls++;
    // view: block0 = PlayerAnomalyData(0x50), PlayerAnomalyRequest(0x24), MPI(0x50), CameraPlaneInterpolation(0xf0);
    //       block1 = AnomalyRules(4), PlayerAnomalyComponent(0xf0), ...; block3 +0x10 = entity index
    int64_t idx = *(int64_t*)(blocks[3] + 0x10);
    g_s.uaaIdx = idx;
    g_s.pad = *(uint8_t**)(blocks[0] + 0) + idx * 0x50;
    g_s.cam = *(uint8_t**)(blocks[0] + 24) + idx * 0xf0;
    g_s.rules = *(uint8_t**)(blocks[1] + 0) + idx * 4;
    // the body hands a6 (world_state::env::Database) and a7 (ability_database::env::AbilityDatabase) to its
    // anomaly scan, which starts with isAbilityUnlocked(a7, a6, 7)
    g_s.worldDb = (void*)a6; g_s.abilityDb = (void*)a7; g_s.uaaTick = GetTickCount();
    g_origUaa(blocks, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
}

struct SweepResult {
    bool hit = false; float distance = 0; Vec position{}, normal{}; uint64_t entity = ~0ull; uint16_t type = 0;
};

// The sweep options embed an MSVC std::function<bool(const PhysicsSweepHit&)> whose impl pointer sits at
// +0x40. The engine only uses the impl through its vtable: slot 0 _Copy(where) clones it into a 56-byte
// buffer of the caller, slot 1 _Move(where) moves it again into the filter wrapper, slot 2 _Do_call(hit)
// is called per candidate hit (a pointer to the hit; entity id at +0x14; return true = IGNORE it), slot 4
// _Delete_this(bool dealloc) destroys the copies. This object provides exactly that.
struct SelfFilter {
    const void* const* vtable;
    void* scene; uint64_t self; int* skipped;
};
SelfFilter* selfFilterCopy(const SelfFilter* f, void* where) { SelfFilter* c = (SelfFilter*)where; *c = *f; return c; }
SelfFilter* selfFilterMove(SelfFilter* f, void* where)       { SelfFilter* c = (SelfFilter*)where; *c = *f; return c; }
bool selfFilterCall(SelfFilter* f, const uint8_t* hit) {
    // true = ignore this hit (physics::RaycastIgnoreCallback): only our own capsule is ignored
    uint64_t id; memcpy(&id, hit + 0x14, 8);
    if ((uint32_t)id == 0xffffffffu) return false;
    bool self = id == f->self || (g_typeLookup && f->scene && g_typeLookup(f->scene, id) == (uint16_t)kPlayerLayer);
    if (self) (*f->skipped)++;
    return self;
}
const void* selfFilterTargetType(const SelfFilter*) { return nullptr; }   // never called (std::function::target_type)
void selfFilterDelete(SelfFilter*, bool) {}                                // copies live in the engine's buffers; nothing to free
const void* selfFilterGet(const SelfFilter* f) { return f; }
const void* const g_selfFilterVtable[6] = {
    (const void*)&selfFilterCopy, (const void*)&selfFilterMove, (const void*)&selfFilterCall,
    (const void*)&selfFilterTargetType, (const void*)&selfFilterDelete, (const void*)&selfFilterGet,
};

// selfEntity: the player's entity; hits on it (or on anything in the player capsule's physics layer) are
// skipped through the engine's own hit filter, so the sweep can start inside the capsule.
SweepResult doSweep(void* scene, uint32_t group, Vec from, Vec to, float radius, uint64_t selfEntity = ~0ull) {
    alignas(16) uint8_t out[0x80]; memset(out, 0, sizeof out);
    alignas(16) uint8_t opts[0x60]; memset(opts, 0, sizeof opts);
    *(uint32_t*)(opts + 0x00) = kQueryFlags;
    *(uint32_t*)(opts + 0x04) = group;
    *(int32_t*)(opts + 0x48) = 1;
    int skipped = 0;
    SelfFilter keep{ g_selfFilterVtable, scene, selfEntity, &skipped };
    *(void**)(opts + 0x40) = selfEntity != ~0ull ? (void*)&keep : nullptr;
    g_sweep(out, scene, radius, &from, &to, opts);
    if (skipped) logf("sweep: skipped %d hit(s) on our own capsule", skipped);
    SweepResult r;
    r.hit = out[kOffHit] != 0;
    memcpy(&r.entity, out + kOffEntity, 8);
    r.distance = *(float*)(out + kOffDistance) * len(sub(to, from));   // the record holds a fraction of the sweep
    memcpy(&r.position, out + kOffPosition, 16);
    memcpy(&r.normal, out + kOffNormal, 16);
    if (r.hit && g_typeLookup && scene && r.entity != ~0ull) r.type = g_typeLookup(scene, r.entity);   // physics layer of the hit entity
    return r;
}

void applyTarget(const Quat& q, void* pac, Quat* plane, uint8_t* mpi, const char* why) {
    if (!g_s.cam) { logf("cannot apply: camera row not captured yet (update_active_anomaly never ran: %u calls)", g_s.uaaCalls); return; }
    g_s.ourTarget = q;
    // 2nd argument: packed pivot. Byte 4 set = use the float in the low 32 bits as the pivot height
    // fraction along the capsule (0 = feet, 1 = head); byte 4 clear = the game's default 0.5.
    g_origStart(&q, kPivot, kDurationOverride, kLinear, pac, plane, mpi, g_s.cam);
    logf("start (%s): target (%.3f %.3f %.3f %.3f) -> mpi pending=%d active=%d target (%.3f %.3f %.3f %.3f) duration-override=%.2f cam pending=%d",
         why, q.x, q.y, q.z, q.w, mpi[0x4b], mpi[0x49],
         *(float*)(mpi + 0x10), *(float*)(mpi + 0x14), *(float*)(mpi + 0x18), *(float*)(mpi + 0x1c), *(float*)(mpi + 0x40), g_s.cam[0xe2]);
    const uint64_t* mq = (const uint64_t*)mpi; const uint64_t* pq = (const uint64_t*)pac;
    logf("rows: mpi+20 %llx %llx %llx | mpi+38..4f %llx %llx %llx | pac+0 %llx %llx %llx %llx %llx %llx",
         mq[4], mq[5], mq[6], mq[7], mq[8], mq[9], pq[0], pq[1], pq[2], pq[3], pq[4], pq[5]);
}

// ---------------------------------------------------------------- blockers (layer matrix)
uint64_t* g_matrix = nullptr;        // physics layer collision matrix: 64 rows of 64 bits; found through the layer_check anchor
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// Length of a jump written over a function start, in the forms hook libraries use (0 = none of them), and
// where it goes.
size_t jumpAt(const uint8_t* p, uintptr_t& target) {
    if (p[0] == 0xE9) { int32_t rel; memcpy(&rel, p + 1, 4); target = (uintptr_t)p + 5 + rel; return 5; }             // jmp rel32
    if (p[0] == 0xFF && p[1] == 0x25 && !p[2] && !p[3] && !p[4] && !p[5]) { memcpy(&target, p + 6, 8); return 14; }   // jmp [rip+0], address
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) { memcpy(&target, p + 2, 8); return 12; }     // mov rax, address; jmp rax
    if (p[0] == 0x49 && p[1] == 0xBB && p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3) { memcpy(&target, p + 2, 8); return 13; }   // mov r11, address; jmp r11
    return 0;
}

// True while the function at `site` still starts with this mod's own jump to `hook` (MinHook: jmp rel32 to
// a relay that holds "jmp [rip+0], hook").
bool ownHook(const uint8_t* site, const void* hook) {
    uintptr_t relay = 0, dest = 0;
    return site && jumpAt(site, relay) == 5 && readable((const void*)relay, 14) &&
           jumpAt((const uint8_t*)relay, dest) == 14 && dest == (uintptr_t)hook;
}
std::vector<std::pair<int, int>> g_cleared;   // (hi, lo) matrix bits we cleared, for restoring
void setMatrixBit(int a, int b, bool on) {
    int hi = a > b ? a : b, lo = a > b ? b : a;
    if (on) { g_matrix[hi] |= 1ull << lo; g_matrix[lo] |= 1ull << hi; }
    else    { g_matrix[hi] &= ~(1ull << lo); g_matrix[lo] &= ~(1ull << hi); }
}
void applyBlockerLayers() {
    if (!g_matrix || !readable(g_matrix, 64 * 8)) return;
    int P = kPlayerLayer;
    if (P < 0 || P >= 64) return;
    int nIgnore = g_cfg.enabled ? g_cfg.nIgnore : 0;          // switched off: every bit goes back
    for (size_t i = 0; i < g_cleared.size(); ) {
        bool wanted = false;
        for (int k = 0; k < nIgnore; k++) {
            int L = g_cfg.ignoreLayers[k]; int hi = P > L ? P : L, lo = P > L ? L : P;
            if (g_cleared[i].first == hi && g_cleared[i].second == lo) wanted = true;
        }
        if (wanted) { i++; continue; }
        setMatrixBit(g_cleared[i].first, g_cleared[i].second, true);
        logf("blockers: restored collision between layers %d and %d", g_cleared[i].first, g_cleared[i].second);
        g_cleared.erase(g_cleared.begin() + i);
    }
    for (int k = 0; k < nIgnore; k++) {
        int L = g_cfg.ignoreLayers[k];
        if (L < 0 || L >= 64 || L == P) continue;
        int hi = P > L ? P : L, lo = P > L ? L : P;
        if (!((g_matrix[hi] >> lo) & 1)) continue;          // already clear (by us, or never set)
        setMatrixBit(hi, lo, false);
        bool known = false; for (auto& c : g_cleared) if (c.first == hi && c.second == lo) known = true;
        if (!known) g_cleared.push_back({ hi, lo });
        logf("blockers: cleared collision between player layer %d and layer %d (the player now passes through layer-%d blockers)", P, L, L);
    }
}

// -1 = cannot tell yet, 0 = locked, 1 = unlocked
int anomalyUnlocked() {
    if (!g_isUnlocked || !g_s.worldDb || !g_s.abilityDb || GetTickCount() - g_s.uaaTick > 3000) return -1;
    return (uint8_t)g_isUnlocked(g_s.abilityDb, g_s.worldDb, (uint8_t)kAbilityId) ? 1 : 0;
}

void requestShift(uint8_t (*b)[32], void* scene) {
    if (g_cfg.requireUnlock && g_isUnlocked) {
        int u = anomalyUnlocked();
        if (u < 0) {
            logf("shift refused: cannot check the Gravity Anomaly unlock yet (the game's anomaly update has not run; this mod's hook on it is %s)",
                 ownHook((const uint8_t*)g_anchors[A_UAA].found, (const void*)hkUaa) ? "in place" : "no longer first on the function: another mod hooked or restored it");
            return;
        }
        if (u == 0) { logf("shift refused: the Gravity Anomaly ability (id %d) is not unlocked in this save", kAbilityId); return; }
    }
    // Column pointers of the interpolation view, in the system's signature order (4 per 32-byte block).
    int64_t idx = *(int64_t*)(b[3] + 8);
    uint8_t* entTable = *(uint8_t**)(b[3] + 0);
    uint64_t entity = entTable ? *(uint64_t*)(entTable + 0x10 + idx * 8) : ~0ull;
    Quat* plane = (Quat*)(*(uint8_t**)(b[0] + 16) + idx * 0x10);
    uint8_t* mpi = *(uint8_t**)(b[1] + 0) + idx * 0x50;
    uint8_t* wtf = *(uint8_t**)(b[1] + 16) + idx * 0x20;          // WorldTransformReadOnly: quat, position
    uint32_t group = *(uint32_t*)(*(uint8_t**)(b[2] + 8) + idx * 4);
    void* pac = *(uint8_t**)(b[2] + 16) + idx * 0xf0;             // PlayerAnomalyComponent row

    Quat rot; memcpy(&rot, wtf, 16);
    Vec pos; memcpy(&pos, wtf + 16, 16);
    Vec up = norm(rotate(*plane, vec(0, 1, 0)));
    Vec fwd = norm(rotate(rot, vec(0, 0, kForwardSign)));
    // keep the facing in the current plane
    fwd = norm(sub(fwd, mul(up, dot(fwd, up))));

    Vec chest = add(pos, mul(up, kChestHeight));
    Vec from = chest;                                            // from the player's centre; our capsule is filtered out
    Vec to = add(chest, mul(fwd, g_cfg.rayLength));
    SweepResult r = doSweep(scene, group, from, to, g_cfg.rayRadius, entity);
    if (r.hit && r.entity == entity) {
        logf("sweep hit our own entity at %.2f m, treating as no hit", r.distance);
        r.hit = false;
    }
    // Straight up as well: standing under a surface, the player can flip upside down onto it, as the
    // game's own anomaly zones allow. Whichever surface is closer wins.
    SweepResult u;
    {
        Vec upTo = add(chest, mul(up, g_cfg.rayLength));
        u = doSweep(scene, group, chest, upTo, g_cfg.rayRadius, entity);
        if (u.hit && u.entity == entity) u.hit = false;
        logf("up sweep: hit=%d dist=%.2f normal (%.2f %.2f %.2f) | forward hit=%d dist=%.2f", (int)u.hit, u.distance, u.normal.x, u.normal.y, u.normal.z, (int)r.hit, r.distance);
    }

    Vec newUp;
    const char* why;
    if (u.hit && (!r.hit || u.distance <= r.distance)) { newUp = norm(u.normal); why = "surface above"; r = u; }
    else if (r.hit) { newUp = norm(r.normal); why = "wall ahead"; }
    else {
        // Nothing ahead: look for the surface past the edge the way the game's own scan does.
        // Down from a point ahead, then back toward the ledge face from below floor level.
        newUp = fwd; why = "nothing ahead (facing)";
        Vec aheadTop = add(chest, mul(fwd, g_cfg.edgeAhead));
        float depth = kChestHeight + g_cfg.edgeDepth;
        Vec aheadBottom = sub(aheadTop, mul(up, depth));
        SweepResult d = doSweep(scene, group, aheadTop, aheadBottom, g_cfg.rayRadius, entity);
        logf("edge probe down: hit=%d dist=%.2f (floor level %.2f) normal (%.2f %.2f %.2f)", (int)d.hit, d.distance, kChestHeight, d.normal.x, d.normal.y, d.normal.z);
        if (d.hit && d.distance <= kChestHeight + g_cfg.floorTol) {
            why = "floor continues ahead (facing)";
        } else {
            float backDepth = d.hit ? (d.distance - 0.15f) : depth;      // stay just above whatever is below
            Vec backStart = sub(aheadTop, mul(up, backDepth));
            Vec backEnd = sub(backStart, mul(fwd, g_cfg.edgeAhead + 1.0f));
            SweepResult bk = doSweep(scene, group, backStart, backEnd, g_cfg.rayRadius, entity);
            logf("edge probe back: from (%.2f %.2f %.2f) hit=%d dist=%.2f normal (%.2f %.2f %.2f)", backStart.x, backStart.y, backStart.z, (int)bk.hit, bk.distance, bk.normal.x, bk.normal.y, bk.normal.z);
            if (bk.hit && dot(norm(bk.normal), fwd) > 0.3f) { newUp = norm(bk.normal); why = "edge: ledge face"; }
        }
    }
    if (g_cfg.axisOnly || g_cfg.snapDeg > 0.f) {
        const Vec axes[6] = { vec(1,0,0), vec(-1,0,0), vec(0,1,0), vec(0,-1,0), vec(0,0,1), vec(0,0,-1) };
        float best = -2.f; Vec bestAxis = newUp;
        for (const Vec& a : axes) { float dd = dot(newUp, a); if (dd > best) { best = dd; bestAxis = a; } }
        bool within = g_cfg.axisOnly || best >= cosf(g_cfg.snapDeg * 0.017453292f);
        if (within && best < 0.99999f) {
            logf("snap: new up (%.3f %.3f %.3f) -> axis (%.0f %.0f %.0f), %.1f deg", newUp.x, newUp.y, newUp.z, bestAxis.x, bestAxis.y, bestAxis.z, acosf(best) * 57.29578f);
            newUp = bestAxis;
        }
    }

    Quat q = fromTo(vec(0, 1, 0), newUp, fwd);
    logf("shift (%s): entity %llx pos (%.2f %.2f %.2f) up (%.2f %.2f %.2f) fwd (%.2f %.2f %.2f) from (%.2f %.2f %.2f) to (%.2f %.2f %.2f) hit=%d dist=%.3g hitpos (%.2f %.2f %.2f) normal (%.2f %.2f %.2f) entity %llx type %u -> new up (%.2f %.2f %.2f) quat (%.3f %.3f %.3f %.3f)",
         why, (unsigned long long)entity, pos.x, pos.y, pos.z, up.x, up.y, up.z, fwd.x, fwd.y, fwd.z, from.x, from.y, from.z, to.x, to.y, to.z,
         (int)r.hit, r.distance, r.position.x, r.position.y, r.position.z, r.normal.x, r.normal.y, r.normal.z,
         (unsigned long long)r.entity, (unsigned)r.type, newUp.x, newUp.y, newUp.z, q.x, q.y, q.z, q.w);
    if (g_s.pad) {
        uint64_t cur; memcpy(&cur, g_s.pad, 8);
        if (cur != ~0ull) {
            // Shifting from inside one of the game's anomalies: tell its bookkeeping we left, so it stops
            // asking for that anomaly's plane. Its prompt still works to go back in.
            uint64_t none = ~0ull; memcpy(g_s.pad, &none, 8); memcpy(g_s.pad + 8, &none, 8); g_s.pad[0x48] = 0;
            logf("shift inside anomaly %llx: cleared the game's current-anomaly state", (unsigned long long)cur);
        }
    }
    g_s.engaged = true;
    applyTarget(q, pac, plane, mpi, why);
}

void requestReset(uint8_t (*b)[32]) {
    int64_t idx = *(int64_t*)(b[3] + 8);
    Quat* plane = (Quat*)(*(uint8_t**)(b[0] + 16) + idx * 0x10);
    uint8_t* mpi = *(uint8_t**)(b[1] + 0) + idx * 0x50;
    void* pac = *(uint8_t**)(b[2] + 16) + idx * 0xf0;
    logf("reset: back to normal gravity (dropped %u game calls while engaged)", g_s.dropped);
    g_s.dropped = 0;
    g_s.engaged = false;
    g_s.releasing = true;
    applyTarget(kIdentity, pac, plane, mpi, "reset");
}

void fileTime(const std::wstring& f, FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    t = GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad) ? fad.ftLastWriteTime : FILETIME{};
}

void maybeReloadSettings() {
    DWORD now = GetTickCount();
    if (now - g_s.lastCfgCheck < 1000) return;
    g_s.lastCfgCheck = now;
    FILETIME ini, menu;
    fileTime(g_dir + L"gravitycontrol_config.ini", ini);
    fileTime(g_dir + L"ModMenuConfig\\gravitycontrol.ini", menu);
    if (CompareFileTime(&ini, &g_s.cfgTime) != 0 || CompareFileTime(&menu, &g_s.menuCfgTime) != 0) {
        g_s.cfgTime = ini; g_s.menuCfgTime = menu;
        loadSettings();
        logSettings();
    }
    if (g_cfg.enabled != g_s.enabledLast) {
        if (g_s.enabledLast != -1 || !g_cfg.enabled) logf(g_cfg.enabled ? "enabled" : "disabled (Enable is off in the MODS menu or the ini): inputs are ignored");
        g_s.enabledLast = g_cfg.enabled;
    }
    applyBlockerLayers();
}

// Buttons of every connected XInput pad, in the shared mask layout. Only slots known to hold a pad are
// asked here: asking an empty slot can stall for milliseconds, so xinputThread looks for new pads.
uint32_t xinputButtons() {
    if (!g_xinputGetState) return 0;
    uint32_t slots = g_xiSlots.load(std::memory_order_relaxed), m = 0;
    for (DWORD i = 0; i < 4; i++) {
        if (!(slots & (1u << i))) continue;
        XINPUT_STATE st{};
        if (g_xinputGetState(i, &st) != ERROR_SUCCESS) {
            g_xiSlots.fetch_and(~(1u << i)); logf("controller: XInput pad in slot %lu gone", i);
            continue;
        }
        m |= st.Gamepad.wButtons;
        if (st.Gamepad.bLeftTrigger > 60) m |= sonypad::kLT;
        if (st.Gamepad.bRightTrigger > 60) m |= sonypad::kRT;
    }
    return m;
}

bool gameHasFocus() {
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void hkBody(uint8_t (*blocks)[32], void* p2, void** scenePtr, float* dt, void* p5) {
    g_s.bodyCalls++;
    maybeReloadSettings();
    if (!g_cfg.enabled) {
        // Switched off: give the plane back to the game and forget half-pressed inputs.
        if (g_s.engaged) { logf("disabled while shifted: returning to normal gravity"); requestReset(blocks); }
        g_s.keyShiftDown = g_s.padDown = false;
    }
    if (g_s.engaged || g_s.releasing) {
        // The interpolation waits for the game's transition animation while MPI+0x48 is set; we play no
        // animation, so clear the wait and let it start on this frame.
        int64_t idx = *(int64_t*)(blocks[3] + 8);
        uint8_t* mpi = *(uint8_t**)(blocks[1] + 0) + idx * 0x50;
        if (mpi[0x4b] && mpi[0x48]) { mpi[0x48] = 0; logf("pending transition: cleared animation wait, starting now"); }
        if (mpi[0x4b] && !mpi[0x4a] && *(void**)(mpi + 0x20) == nullptr) {
            mpi[0x4a] = 1; logf("pending transition: no easing curve handle at MPI+0x20, forcing linear");
        }
        if (mpi[0x4b] || mpi[0x49]) {
            g_s.traceFrames = 6;
        }
        if (g_s.releasing && !mpi[0x4b] && !mpi[0x49]) { g_s.releasing = false; logf("reset transition finished"); }
    }
    if (g_cfg.enabled && gameHasFocus()) {
        bool doShift = false, doReset = false;
        DWORD know = GetTickCount();
        bool shift = g_cfg.keyShift && (GetAsyncKeyState(g_cfg.keyShift) & 0x8000) != 0;
        if (shift && !g_s.keyShiftDown) { g_s.keyDownAt = know; g_s.keyConsumed = false; }
        if (shift && g_s.keyShiftDown && !g_s.keyConsumed && know - g_s.keyDownAt >= (DWORD)(g_cfg.keyHoldSeconds * 1000.f)) {
            g_s.keyConsumed = true; doReset = true; logf("key: hold -> reset");
        }
        if (!shift && g_s.keyShiftDown && !g_s.keyConsumed) { doShift = true; logf("key: tap -> shift"); }
        g_s.keyShiftDown = shift;
        // Controller: tap = shift, hold for HoldSeconds = reset. XInput pads and PlayStation pads read over
        // HID feed one button mask, so a pad that shows up both ways (Steam Input) still counts once.
        if (g_cfg.padEnabled && g_cfg.padButton) {
            uint32_t buttons = xinputButtons() | g_hidButtons.load(std::memory_order_relaxed);
            bool down = (buttons & (uint32_t)g_cfg.padButton) != 0;
            if (down && !g_s.padDown) { g_s.padDownAt = know; g_s.padConsumed = false; }
            if (down && g_s.padDown && !g_s.padConsumed && know - g_s.padDownAt >= (DWORD)(g_cfg.padHoldSeconds * 1000.f)) {
                g_s.padConsumed = true; doReset = true; logf("pad: hold -> reset");
            }
            if (!down && g_s.padDown && !g_s.padConsumed) { doShift = true; logf("pad: tap -> shift"); }
            g_s.padDown = down;
        } else g_s.padDown = false;
        if ((doShift || doReset) && menuOpen() == 1) {
            DWORD now2 = GetTickCount();
            if (now2 - g_s.menuIgnoreLog > 2000) { char nm[40]; menuName(g_s.menuState, nm, sizeof nm); logf("input ignored: a menu is open [%s]", nm); g_s.menuIgnoreLog = now2; }
            doShift = doReset = false;
        }
        if (doShift) {
            void* scene = scenePtr ? *scenePtr : nullptr;
            if (scene && g_sweep) requestShift(blocks, scene);
            else logf("shift: no physics scene captured yet");
        }
        if (doReset) {
            int64_t ridx = *(int64_t*)(blocks[3] + 8);
            Quat* rplane = (Quat*)(*(uint8_t**)(blocks[0] + 16) + ridx * 0x10);
            Vec rup = rotate(*rplane, vec(0, 1, 0));
            bool turned = rup.y < 0.999f;                                          // up is not world up
            bool inRealAnomaly = g_s.pad && *(uint64_t*)g_s.pad != ~0ull;
            if (g_s.engaged || (turned && !inRealAnomaly)) requestReset(blocks);
            else if (turned) logf("reset ignored: inside one of the game's anomalies (leave it to return to normal)");
        }
    }
    if (g_cfg.diagnostics && (g_s.bodyCalls % 300) == 1) {
        int64_t idx = *(int64_t*)(blocks[3] + 8);
        Quat* plane = (Quat*)(*(uint8_t**)(blocks[0] + 16) + idx * 0x10);
        uint8_t* mpi = *(uint8_t**)(blocks[1] + 0) + idx * 0x50;
        logf("diag: anomaly ability %d unlocked=%d (require=%d) | menu open=%d", kAbilityId, anomalyUnlocked(), g_cfg.requireUnlock, menuOpen());
        logf("diag: plane (%.3f %.3f %.3f %.3f) mpi active=%d pending=%d elapsed=%.2f duration=%.2f target (%.3f %.3f %.3f %.3f) | uaaCalls=%u startCalls=%u rules=%s pad+0=%llx pad+0x48=%d",
             plane->x, plane->y, plane->z, plane->w, mpi[0x49], mpi[0x4b], *(float*)(mpi + 0x38), *(float*)(mpi + 0x3c),
             *(float*)(mpi + 0x10), *(float*)(mpi + 0x14), *(float*)(mpi + 0x18), *(float*)(mpi + 0x1c),
             g_s.uaaCalls, g_s.startCalls,
             g_s.rules ? (std::to_string(g_s.rules[0]) + "," + std::to_string(g_s.rules[1]) + "," + std::to_string(g_s.rules[2]) + "," + std::to_string(g_s.rules[3])).c_str() : "?",
             g_s.pad ? (unsigned long long)*(uint64_t*)g_s.pad : 0ull, g_s.pad ? g_s.pad[0x48] : -1);
    }
    if (g_s.traceFrames > 0) {
        int64_t idx = *(int64_t*)(blocks[3] + 8);
        uint8_t* mpi = *(uint8_t**)(blocks[1] + 0) + idx * 0x50;
        Quat* plane = (Quat*)(*(uint8_t**)(blocks[0] + 16) + idx * 0x10);
        logf("trace: enter body pending=%d active=%d linear=%d wait=%d elapsed=%.3f duration=%.3f dt=%.4f plane (%.3f %.3f %.3f %.3f) curve %p",
             mpi[0x4b], mpi[0x49], mpi[0x4a], mpi[0x48], *(float*)(mpi + 0x38), *(float*)(mpi + 0x3c), dt ? *dt : -1.f,
             plane->x, plane->y, plane->z, plane->w, *(void**)(mpi + 0x20));
        g_origBody(blocks, p2, scenePtr, dt, p5);
        logf("trace: exit body pending=%d active=%d elapsed=%.3f duration=%.3f plane (%.3f %.3f %.3f %.3f)",
             mpi[0x4b], mpi[0x49], *(float*)(mpi + 0x38), *(float*)(mpi + 0x3c), plane->x, plane->y, plane->z, plane->w);
        g_s.traceFrames--;
        return;
    }
    g_origBody(blocks, p2, scenePtr, dt, p5);
}

// ---------------------------------------------------------------- anchors
bool parsePattern(const char* s, std::string& bytes, std::string& mask) {
    bytes.clear(); mask.clear();
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { bytes.push_back(0); mask.push_back('?'); s += (s[1] == '?') ? 2 : 1; continue; }
        unsigned v; if (sscanf_s(s, "%2x", &v) != 1) return false;
        bytes.push_back((char)v); mask.push_back('x'); s += 2;
    }
    return !bytes.empty();
}

uintptr_t findPattern(const uint8_t* base, size_t size, const std::string& bytes, const std::string& mask, int& count) {
    count = 0; uintptr_t first = 0;
    size_t n = bytes.size();
    for (size_t i = 0; i + n <= size; i++) {
        if (base[i] != (uint8_t)bytes[0]) continue;
        size_t k = 1;
        for (; k < n; k++) if (mask[k] == 'x' && base[i + k] != (uint8_t)bytes[k]) break;
        if (k == n) { if (!count) first = i; count++; if (count > 1) break; }
    }
    return first;
}

// Game builds this version was checked on, by PE timestamp. Any other build is tried all the same: the
// patterns decide whether the mod can run, not the timestamp.
const DWORD kKnownBuilds[] = { 0x6AB107A0,      // Steam build 25472515
                               0x6ABA5BB8,      // game 1.4.0, the update of 2026-10-01 (changelist 5642085)
                               0x6ABEF018 };    // game 1.4.1, Steam build 25673981 (changelist 5644780)
bool g_buildKnown = false;

// The image the anchors are looked up in: this process's exe. A test can name a game exe to map as data
// instead (GRAVITYCONTROL_TEST_IMAGE), which checks the patterns and the hook creation against real code.
HMODULE gameImage() {
    static HMODULE img = nullptr;
    if (!img) {
        wchar_t path[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"GRAVITYCONTROL_TEST_IMAGE", path, MAX_PATH);
        if (n && n < MAX_PATH) img = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!img) img = GetModuleHandleW(nullptr);
    }
    return img;
}

bool textSection(const uint8_t*& text, size_t& size) {
    HMODULE exe = gameImage();
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".text", 5) == 0) { text = (uint8_t*)exe + sec[i].VirtualAddress; size = sec[i].Misc.VirtualSize; return true; }
    return false;
}

// Another mod may have hooked a function before this one looked for it. Its first bytes are then a jump out
// of the game image, so the pattern does not match there any more. Such a function is found by its pattern
// from kHookHead on, and MinHook chains onto the jump that is there: this mod runs first, then the other.
const size_t kHookHead = 16;      // bytes at a function start that another mod's jump may have replaced
bool g_otherMod = false;          // a required function is there, but rewritten in a way this mod cannot share

// Counts the places where the pattern matches from kHookHead on and the start is a jump that leaves the game
// image [lo, hi); site and target describe the first. `tails` counts every place the rest of the pattern
// matches, and `odd` is one of them whose start is no such jump.
int findHooked(const uint8_t* text, size_t size, const std::string& bytes, const std::string& mask, uintptr_t lo, uintptr_t hi,
               uintptr_t& site, uintptr_t& target, const uint8_t*& odd, int& tails) {
    int count = 0; tails = 0; odd = nullptr;
    size_t n = bytes.size();
    if (n < kHookHead + 8) return 0;
    for (size_t i = 0; i + n <= size; i++) {
        size_t k = kHookHead;
        for (; k < n; k++) if (mask[k] == 'x' && text[i + k] != (uint8_t)bytes[k]) break;
        if (k < n) continue;
        tails++;
        const uint8_t* p = text + i; uintptr_t t = 0;
        size_t len = jumpAt(p, t);
        bool hooked = len && (t < lo || t >= hi);
        // what the jump left of the function start is still the pattern, or the filler some hook libraries write
        for (size_t j = len; hooked && j < kHookHead; j++)
            if (mask[j] == 'x' && p[j] != (uint8_t)bytes[j] && p[j] != 0x90 && p[j] != 0xCC) hooked = false;
        if (!hooked) { odd = p; continue; }
        if (!count) { site = (uintptr_t)p; target = t; }
        count++;
    }
    return count;
}

// File name of the module a hook jump leads to. A hook library's jump goes to a relay outside any module
// first, which jumps on to the hook function.
std::string hookOwner(uintptr_t target) {
    for (int hop = 0; hop < 3; hop++) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &mod) && mod) {
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(mod, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            char out[MAX_PATH * 3] = "";
            WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof out, nullptr, nullptr);
            if (out[0]) return out;
            break;
        }
        uintptr_t next = 0;
        if (!readable((const void*)target, 16) || !jumpAt((const uint8_t*)target, next)) break;
        target = next;
    }
    return "another mod";
}

// Looks every anchor up. Returns false when a required one is missing; a missing optional one just stays 0.
bool resolveAnchors() {
    HMODULE exe = gameImage();
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    DWORD stamp = nt->FileHeader.TimeDateStamp;
    for (DWORD b : kKnownBuilds) if (b == stamp) g_buildKnown = true;
    logf("game image %p, build timestamp 0x%08X: %s", exe, stamp,
         g_buildKnown ? "a build this version was checked on" : "not a build this version knows, trying anyway");
    const uint8_t* text = nullptr; size_t textSize = 0;
    if (!textSection(text, textSize)) { logf("FAIL: no .text section"); return false; }
    uintptr_t lo = (uintptr_t)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    bool ok = true;
    for (auto& a : g_anchors) {
        int most = 0, which = 0;
        for (const char* pat : a.patterns) {
            if (!pat) continue;
            which++;
            std::string b, m; parsePattern(pat, b, m);
            int count = 0; uintptr_t off = findPattern(text, textSize, b, m, count);
            if (count == 1) { a.found = (uintptr_t)text + off; break; }
            if (count > most) most = count;
        }
        const uint8_t* odd = nullptr;
        if (!a.found && !most) {
            // not there as the game ships it: look for it behind another mod's hook
            which = 0;
            for (const char* pat : a.patterns) {
                if (!pat) continue;
                which++;
                std::string b, m; parsePattern(pat, b, m);
                uintptr_t site = 0, target = 0; const uint8_t* o = nullptr; int tails = 0;
                int n = findHooked(text, textSize, b, m, lo, hi, site, target, o, tails);
                if (n == 1) { a.found = site; a.sharedWith = hookOwner(target); break; }
                if (n == 0 && tails == 1 && !odd) odd = o;
            }
        }
        if (a.found && a.sharedWith.empty()) logf("anchor %s at rva 0x%llx (pattern %d)", a.name, (unsigned long long)(a.found - (uintptr_t)exe), which);
        else if (a.found) logf("anchor %s at rva 0x%llx (pattern %d), behind a hook of %s", a.name, (unsigned long long)(a.found - (uintptr_t)exe), which, a.sharedWith.c_str());
        else if (odd) {
            char hex[kHookHead * 3 + 1] = "";
            for (size_t i = 0; i < kHookHead; i++) sprintf_s(hex + i * 3, 4, "%02x ", odd[i]);
            hex[kHookHead * 3 - 1] = 0;
            logf("FAIL: %s anchor %s is at rva 0x%llx, but it starts with \"%s\": rewritten by another mod or tool in a way this mod cannot share",
                 a.required ? "required" : "optional", a.name, (unsigned long long)((uintptr_t)odd - (uintptr_t)exe), hex);
            a.changed = true;
            if (a.required) { ok = false; g_otherMod = true; }
        } else {
            logf("FAIL: %s anchor %s not found (%d matches); the game code changed", a.required ? "required" : "optional", a.name, most);
            if (a.required) ok = false;
        }
    }
    if (g_anchors[A_LAYERS].found) {
        const uint8_t* lea = (const uint8_t*)g_anchors[A_LAYERS].found + 59;     // lea rdx, [rip + matrix]
        int32_t disp; memcpy(&disp, lea + 3, 4);
        g_matrix = (uint64_t*)(lea + 7 + disp);
        if ((uintptr_t)g_matrix < lo || (uintptr_t)g_matrix + 64 * 8 > hi) g_matrix = nullptr;      // must lie inside the game image
        else logf("layer matrix at rva 0x%llx", (unsigned long long)((uintptr_t)g_matrix - lo));
    }
    return ok;
}

bool dialogs() { return GetEnvironmentVariableW(L"GRAVITYCONTROL_NO_DIALOGS", nullptr, 0) == 0; }   // tests switch the pop-ups off

// ---------------------------------------------------------------- other copies of the mod
// 1.0 was installed flat (crmods\gravitycontrol.dll) and has no way to be told to stop. A copy from 1.1
// on exports GravityControl_InstanceGuard and takes a mutex: the first one in the process is in charge.
struct OtherCopy { HMODULE mod = nullptr; uintptr_t base = 0; size_t size = 0; std::wstring path; };
struct Copies { bool second = false; std::wstring inCharge; std::vector<OtherCopy> legacy; };

Copies findCopies() {
    Copies c;
    wchar_t name[64]; swprintf_s(name, L"Local\\GravityControl.Instance.%lu", GetCurrentProcessId());
    HANDLE m = CreateMutexW(nullptr, FALSE, name);
    c.second = m && GetLastError() == ERROR_ALREADY_EXISTS;      // a 1.1+ copy got here first
    if (c.second) CloseHandle(m); else g_instanceMutex = m;
    for (int attempt = 0; attempt < 5; attempt++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_BAD_LENGTH) continue; break; }
        MODULEENTRY32W me; me.dwSize = sizeof me;
        if (Module32FirstW(snap, &me)) do {
            if (me.hModule == g_self) continue;
            if (_wcsicmp(me.szModule, L"gravitycontrol.dll") && _wcsicmp(me.szModule, L"gravityshift.dll")) continue;
            if (GetProcAddress(me.hModule, "GravityControl_InstanceGuard")) { c.inCharge = me.szExePath; continue; }
            OtherCopy o; o.mod = me.hModule; o.base = (uintptr_t)me.modBaseAddr; o.size = me.modBaseSize; o.path = me.szExePath;
            c.legacy.push_back(o);
        } while (Module32NextW(snap, &me));
        CloseHandle(snap);
        break;
    }
    return c;
}

// True while a thread that was started inside [base, base + size) is alive: an older copy's init thread.
bool threadStartedIn(uintptr_t base, size_t size) {
    typedef LONG (NTAPI *QueryFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    if (!query) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te; te.dwSize = sizeof te; bool any = false; DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        uintptr_t start = 0;
        if (query(t, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) == 0 && start >= base && start < base + size) any = true;
        CloseHandle(t);
    } while (!any && Thread32Next(snap, &te));
    CloseHandle(snap);
    return any;
}

// A function start as MinHook leaves it: "E9 rel32" to a relay "FF 25 00000000 <address>", here with the
// address inside the module [base, base + size).
bool hookedBy(const uint8_t* site, uintptr_t base, size_t size) {
    if (site[0] != 0xE9) return false;
    int32_t rel; memcpy(&rel, site + 1, 4);
    const uint8_t* relay = site + 5 + rel;
    if (!readable(relay, 14) || relay[0] != 0xFF || relay[1] != 0x25) return false;
    int32_t disp; memcpy(&disp, relay + 2, 4);
    uintptr_t target; memcpy(&target, relay + 6, 8);
    return disp == 0 && target >= base && target < base + size;
}

// An older copy is loaded: wait until it has finished starting, then put the original bytes back where it
// hooked the game. Its hook functions are never reached again, so it stays loaded but does nothing, and
// this copy hooks the untouched functions as usual. This runs while the game is still starting up.
void takeOver(const OtherCopy& old) {
    logf("an older copy is loaded (%ls): taking over from it", old.path.c_str());
    DWORD t0 = GetTickCount();
    while (threadStartedIn(old.base, old.size) && GetTickCount() - t0 < 120000) Sleep(100);
    const uint8_t* text = nullptr; size_t textSize = 0;
    if (!textSection(text, textSize) || textSize < 64) return;
    static const int hooked[] = { A_START, A_MPI, A_UAA, A_MENU };
    int removed = 0;
    for (int id : hooked) {
        const Anchor& a = g_anchors[id];
        std::string b, m; parsePattern(a.patterns[0], b, m);
        // the hook overwrote the first 5 bytes: find the function by the rest of its pattern
        int count = 0; uintptr_t off = findPattern(text + 5, textSize - 5, b.substr(5), m.substr(5), count);
        if (count != 1) { logf("takeover: %s not found (%d matches)", a.name, count); continue; }
        uint8_t* site = (uint8_t*)text + off;
        if (memcmp(site, b.data(), 5) == 0) continue;                       // not hooked
        if (!hookedBy(site, old.base, old.size)) { logf("takeover: %s is hooked by something else; left alone", a.name); continue; }
        DWORD prot = 0;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &prot)) { logf("takeover: cannot write at %s (error %lu)", a.name, GetLastError()); continue; }
        memcpy(site, b.data(), 5);
        VirtualProtect(site, 5, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        removed++;
    }
    logf("takeover: removed %d hook(s) of the older copy after %lu ms; it stays loaded but does nothing", removed, GetTickCount() - t0);
}

bool fileExists(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Renames a file in place by appending ".old" (".old2" ... when that name is taken). Nothing is deleted or
// overwritten. A loaded DLL can be renamed, and the loader only picks up names ending in .dll.
bool renameOld(const std::wstring& f, const char* what) {
    if (!fileExists(f)) return true;
    DWORD err = 0;
    for (int i = 1; i <= 9; i++) {
        std::wstring to = f + (i == 1 ? std::wstring(L".old") : L".old" + std::to_wstring(i));
        if (fileExists(to)) continue;
        if (MoveFileExW(f.c_str(), to.c_str(), 0)) { logf("upgrade: renamed the old %s to %ls", what, to.c_str()); return true; }
        err = GetLastError();
        break;
    }
    logf("upgrade: could not rename the old %s %ls (error %lu); it is tried again on the next start", what, f.c_str(), err);
    return false;
}

// This copy lives in crmods\<folder>\. The files a flat 1.0 install left in crmods\ are renamed with
// ".old" on the end, so the next start loads this copy only. The player deletes them when they like.
void retireFlatInstall() {
    std::wstring here = g_dir.substr(0, g_dir.size() - 1);
    size_t p = here.find_last_of(L"\\/"); if (p == std::wstring::npos) return;
    std::wstring crmods = here.substr(0, p);                                  // ...\crmods
    size_t q = crmods.find_last_of(L"\\/"); if (q == std::wstring::npos) return;
    if (_wcsicmp(crmods.substr(q + 1).c_str(), L"crmods") != 0) return;       // not installed as crmods\<folder>\: nothing to do
    std::wstring game = crmods.substr(0, q + 1); crmods += L"\\";
    renameOld(crmods + L"gravitycontrol.dll", "DLL");
    renameOld(crmods + L"gravitycontrol_config.ini", "settings file");
    renameOld(crmods + L"gravitycontrol.log", "log");                         // held open while the old copy is loaded
    std::wstring readme = game + L"README-GravityControl.txt";               // the 1.0 archive put its readme in the game folder
    FILE* fp = nullptr;
    if (!_wfopen_s(&fp, readme.c_str(), L"rb") && fp) {
        char head[20] = {}; fread(head, 1, 18, fp); fclose(fp);
        if (!strcmp(head, "GravityControl 1.0")) renameOld(readme, "readme");
    }
}

// Nothing of ours is hooked yet: leave the process, so Mod Settings Menu shows the mod as not loaded
// instead of "Running".
DWORD unloadSelf() {
    logf("unloading this copy");
    if (g_instanceMutex) { CloseHandle(g_instanceMutex); g_instanceMutex = nullptr; }
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

bool padWanted() { return g_cfg.enabled && g_cfg.padEnabled && g_cfg.padButton; }
bool padVerbose() { return g_cfg.diagnostics != 0; }
DWORD WINAPI hidThread(LPVOID) { sonypad::run(g_hidButtons, padWanted, padVerbose, logf); return 0; }
DWORD WINAPI xinputThread(LPVOID) {
    for (;;) {
        if (g_xinputGetState && padWanted()) {
            for (DWORD i = 0; i < 4; i++) {
                if (g_xiSlots.load() & (1u << i)) continue;
                XINPUT_STATE st{};
                if (g_xinputGetState(i, &st) == ERROR_SUCCESS) { g_xiSlots.fetch_or(1u << i); logf("controller: XInput pad in slot %lu", i); }
            }
        }
        Sleep(2000);
    }
}

const wchar_t kStopMessage[] = L"To stop this message, set VersionWarning=0 in gravitycontrol_config.ini, or switch it off under Options > MODS.";

// The mod cannot run here: say so (unless told not to) and leave the process. otherMod: the game code is
// there, but another mod rewrote it; otherwise a game update is the likely reason.
DWORD standDown(const char* why, bool otherMod = false) {
    logf("GravityControl inactive: %s; the game runs unmodified", why);
    if (g_cfg.versionWarning && dialogs()) {
        std::wstring msg = otherMod
            ? L"Another mod has changed game code that GravityControl needs, in a way GravityControl cannot work with, so GravityControl is "
              L"switched off. The game itself is not affected.\n\n"
              L"Try without your other mods to find out which one it is.\n\n"
            : L"GravityControl could not hook this version of the game, so it is switched off. The game itself is not affected.\n\n"
              L"A game update probably changed the code the mod relies on. Look for a newer version of GravityControl. "
              L"Another mod that changes the same game code can cause this too.\n\n";
        msg += L"Details are in gravitycontrol.log, next to the mod. ";
        MessageBoxW(nullptr, (msg + kStopMessage).c_str(), kTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    return unloadSelf();
}

// After hooking a function that another mod had hooked first: the "original" this mod calls is then that
// mod's hook, so it starts with a jump instead of the function's own first instructions. Says so in the log.
void logShared(int id, const void* orig) {
    const Anchor& a = g_anchors[id];
    uintptr_t next = 0;
    if (orig && jumpAt((const uint8_t*)orig, next)) logf("hook %s is shared: this mod runs first, then %s", a.name, hookOwner(next).c_str());
    else if (!a.sharedWith.empty()) logf("hook %s is shared with %s", a.name, a.sharedWith.c_str());
}

DWORD WINAPI initThread(LPVOID) {
    loadSettings();
    logSettings();
    Copies copies = findCopies();
    if (copies.second) {
        // Another 1.1+ copy is in charge (it loaded first). This one only tidies up and leaves.
        retireFlatInstall();
        logf("inactive: another copy of GravityControl is already in charge (%ls)", copies.inCharge.c_str());
        return unloadSelf();
    }
    for (const OtherCopy& old : copies.legacy) takeOver(old);
    retireFlatInstall();
    if (!resolveAnchors())
        return g_otherMod ? standDown("another mod rewrote a function it needs", true)
                          : standDown("a hook point it needs was not found in this game build");
    if (MH_Initialize() != MH_OK) return standDown("the hooking library did not start");
    {
        HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
        if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
        if (xi) g_xinputGetState = (XInputGetStateFn)GetProcAddress(xi, "XInputGetState");
        logf("controller: %s; PlayStation pads are read over HID", g_xinputGetState ? "XInput ready" : "XInput not available");
    }
    g_sweep = (SweepFn)g_anchors[A_SWEEP].found;
    g_typeLookup = (TypeLookupFn)g_anchors[A_TYPE].found;          // optional: without it only the player's own entity id is filtered
    g_isUnlocked = (UnlockFn)g_anchors[A_UNLOCK].found;            // optional: without it the unlock gate cannot be checked
    const char* failed = nullptr;
    if (MH_CreateHook((void*)g_anchors[A_START].found, (void*)hkStart, (void**)&g_origStart) != MH_OK) failed = "start_helper";
    else if (MH_CreateHook((void*)g_anchors[A_MPI].found, (void*)hkBody, (void**)&g_origBody) != MH_OK) failed = "mpi_body";
    else if (MH_CreateHook((void*)g_anchors[A_UAA].found, (void*)hkUaa, (void**)&g_origUaa) != MH_OK) failed = "uaa_body";
    if (failed) { logf("FAIL: hook %s", failed); MH_Uninitialize(); return standDown("a hook could not be created"); }
    bool menuHook = g_anchors[A_MENU].found &&
                    MH_CreateHook((void*)g_anchors[A_MENU].found, (void*)hkMenu, (void**)&g_origMenu) == MH_OK;   // optional
    logShared(A_START, (const void*)g_origStart); logShared(A_MPI, (const void*)g_origBody); logShared(A_UAA, (const void*)g_origUaa);
    if (menuHook) logShared(A_MENU, (const void*)g_origMenu);
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        // some hooks may already be live, so this copy stays loaded
        logf("FAIL: enable hooks; GravityControl may not work in this session");
        if (g_cfg.versionWarning && dialogs())
            MessageBoxW(nullptr, (std::wstring(L"GravityControl could not switch its hooks on in this version of the game and may not work.\n\n"
                                               L"Details are in gravitycontrol.log, next to the mod. ") + kStopMessage).c_str(),
                        kTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        return 0;
    }
    CreateThread(nullptr, 0, hidThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, xinputThread, nullptr, 0, nullptr);
    logf("active: GravityControl %s - tap the key (0x%X) or the pad button (mask 0x%X) to shift gravity, hold it to reset",
         kVersion, g_cfg.keyShift, g_cfg.padEnabled ? g_cfg.padButton : 0);
    // Parts that had to be left out in this game build. Only the ones a player would notice get a message.
    std::wstring off;
    if (!g_isUnlocked) {
        logf("reduced: the Gravity Anomaly unlock check is unavailable; RequireAnomalyUnlocked cannot be enforced, shifting is allowed");
        off += L"  - the Gravity Anomaly unlock check: gravity can be shifted before the ability is unlocked\n";
    }
    if (!menuHook) {
        logf("reduced: the game-menu state is unavailable; the key and button are not ignored while a menu is open");
        off += L"  - ignoring the key and button while a game menu is open\n";
    }
    if (!g_typeLookup) logf("reduced: the physics layer lookup is unavailable; the ray filters the player by entity id only");
    if (!g_matrix) logf("reduced: the layer matrix was not found; [Blockers] IgnoreLayers does nothing");
    if (off.empty()) logf(g_buildKnown ? "all hook points found" : "all hook points found in a game build this version does not know: no message is shown");
    else if (g_cfg.versionWarning && dialogs()) {
        std::wstring msg = L"GravityControl is running, but this version of the game changed some of the code it relies on. Switched off:\n\n" + off +
                           L"\nEverything else works. Look for a newer version of GravityControl.\n\n";
        MessageBoxW(nullptr, (msg + kStopMessage).c_str(), kTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    return 0;
}

} // namespace

// Marks a copy that guards against a second instance (1.1 and later); see findCopies().
extern "C" __declspec(dllexport) const int GravityControl_InstanceGuard = 1;

// Third-party notices, embedded in the binary (BSD-2-Clause requires them with binary distributions).
extern "C" __declspec(dllexport) const char GravityControl_ThirdPartyNotices[] =
    "GravityControl uses MinHook. Notices follow.\n\n"
    "MinHook - The Minimalistic API Hooking Library for x64/x86\n"
    "Copyright (C) 2009-2017 Tsuda Kageyu.\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "\n"
    " 1. Redistributions of source code must retain the above copyright\n"
    "    notice, this list of conditions and the following disclaimer.\n"
    " 2. Redistributions in binary form must reproduce the above copyright\n"
    "    notice, this list of conditions and the following disclaimer in the\n"
    "    documentation and/or other materials provided with the distribution.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED\n"
    "TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A\n"
    "PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER\n"
    "OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,\n"
    "EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,\n"
    "PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR\n"
    "PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF\n"
    "LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING\n"
    "NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS\n"
    "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "================================================================================\n"
    "Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.\n"
    "================================================================================\n"
    "Hacker Disassembler Engine 32 C\n"
    "Copyright (c) 2008-2009, Vyacheslav Patkov.\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "\n"
    " 1. Redistributions of source code must retain the above copyright\n"
    "    notice, this list of conditions and the following disclaimer.\n"
    " 2. Redistributions in binary form must reproduce the above copyright\n"
    "    notice, this list of conditions and the following disclaimer in the\n"
    "    documentation and/or other materials provided with the distribution.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED\n"
    "TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A\n"
    "PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR\n"
    "CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,\n"
    "EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,\n"
    "PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR\n"
    "PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF\n"
    "LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING\n"
    "NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS\n"
    "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "-------------------------------------------------------------------------------\n"
    "Hacker Disassembler Engine 64 C\n"
    "Copyright (c) 2008-2009, Vyacheslav Patkov.\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "\n"
    " 1. Redistributions of source code must retain the above copyright\n"
    "    notice, this list of conditions and the following disclaimer.\n"
    " 2. Redistributions in binary form must reproduce the above copyright\n"
    "    notice, this list of conditions and the following disclaimer in the\n"
    "    documentation and/or other materials provided with the distribution.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED\n"
    "TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A\n"
    "PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR\n"
    "CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,\n"
    "EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,\n"
    "PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR\n"
    "PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF\n"
    "LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING\n"
    "NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS\n"
    "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n";

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(inst, path, MAX_PATH);
        g_dir = path; size_t p = g_dir.find_last_of(L"\\/"); g_dir = g_dir.substr(0, p + 1);
        InitializeCriticalSection(&g_logLock);
        g_t0 = GetTickCount();
        g_log = _wfsopen((g_dir + L"gravitycontrol.log").c_str(), L"w", _SH_DENYWR);   // readable while the game runs
        logf("GravityControl %s loading from %ls", kVersion, g_dir.c_str());
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf(reserved ? "game exiting" : "unloaded");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
