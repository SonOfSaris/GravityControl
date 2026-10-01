// GravityControl 1.0 - arbitrary gravity direction for CONTROL Resonant (Steam build 25472515).
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
// Loads through crloader (crmods\gravitycontrol.dll). Settings: gravitycontrol_config.ini next to the
// DLL. Log: gravitycontrol.log next to the DLL.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <Xinput.h>
#include "MinHook.h"

namespace {

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
    int keyShift = VK_RSHIFT;   // tap = shift, hold HoldSeconds = reset (like the pad button)
    float keyHoldSeconds = 1.f;
    float rayLength = 10.f;
    float rayRadius = 0.15f;
    float chestHeight = 1.2f;
    int upRay = 1;              // 1 = also look straight up; the closer of the two hits wins
    int diagnostics = 0;
    int versionWarning = 1;     // 1 = message box at start when the game build differs from the tested one
    int blockInMenus = 1;       // 1 = the keys/pad do nothing while one of the game's menus is open
    int requireUnlock = 1;      // 1 = shifting needs the game's Gravity Anomaly ability unlocked
    int abilityId = 7;          // Gravity Anomaly in the ability database (the game's own anomaly scan checks id 7)
    // edge probe (used when nothing is ahead)
    float edgeAhead = 1.2f;     // metres ahead of the chest to probe for the surface past the edge
    float edgeDepth = 2.5f;     // how far below floor level the down probe reaches
    float floorTol = 0.35f;     // a down hit within chestHeight + this is "floor continues"
    float snapDeg = 15.f;       // snap the new up to a world axis when within this angle (0 = off)
    int axisOnly = 1;           // 1 = the new up is always the nearest world axis
    // controller
    int padEnabled = 1;
    int padButton = XINPUT_GAMEPAD_DPAD_UP;
    float padHoldSeconds = 1.f;
    // blockers: collision-matrix bits (PlayerLayer, L) cleared so the player's capsule ignores layer L
    int playerLayer = 8;
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

void loadSettings() {
    std::wstring f = g_dir + L"gravitycontrol_config.ini";
    g_cfg.keyShift   = iniInt(L"Keys", L"Shift", g_cfg.keyShift, f);
    g_cfg.keyHoldSeconds = iniFloat(L"Keys", L"HoldSeconds", g_cfg.keyHoldSeconds, f);
    g_cfg.rayLength  = iniFloat(L"Ray", L"Length", g_cfg.rayLength, f);
    g_cfg.rayRadius  = iniFloat(L"Ray", L"Radius", g_cfg.rayRadius, f);
    g_cfg.chestHeight= iniFloat(L"Ray", L"ChestHeight", g_cfg.chestHeight, f);
    g_cfg.upRay      = iniInt(L"Ray", L"UpwardRay", g_cfg.upRay, f);
    g_cfg.diagnostics= iniInt(L"General", L"Diagnostics", g_cfg.diagnostics, f);
    g_cfg.requireUnlock = iniInt(L"General", L"RequireAnomalyUnlocked", g_cfg.requireUnlock, f);
    g_cfg.blockInMenus = iniInt(L"General", L"BlockInMenus", g_cfg.blockInMenus, f);
    g_cfg.versionWarning = iniInt(L"General", L"VersionWarning", g_cfg.versionWarning, f);
    g_cfg.abilityId  = iniInt(L"General", L"AnomalyAbilityId", g_cfg.abilityId, f);
    g_cfg.edgeAhead  = iniFloat(L"Edge", L"Ahead", g_cfg.edgeAhead, f);
    g_cfg.edgeDepth  = iniFloat(L"Edge", L"Depth", g_cfg.edgeDepth, f);
    g_cfg.floorTol   = iniFloat(L"Edge", L"FloorTolerance", g_cfg.floorTol, f);
    g_cfg.snapDeg    = iniFloat(L"Edge", L"SnapDegrees", g_cfg.snapDeg, f);
    g_cfg.axisOnly   = iniInt(L"Edge", L"AxisOnly", g_cfg.axisOnly, f);
    g_cfg.padEnabled = iniInt(L"Controller", L"Enabled", g_cfg.padEnabled, f);
    g_cfg.padButton  = iniInt(L"Controller", L"Button", g_cfg.padButton, f);
    g_cfg.padHoldSeconds = iniFloat(L"Controller", L"HoldSeconds", g_cfg.padHoldSeconds, f);
    g_cfg.playerLayer = iniInt(L"Blockers", L"PlayerLayer", g_cfg.playerLayer, f);
    iniList(L"Blockers", L"IgnoreLayers", g_cfg.ignoreLayers, g_cfg.nIgnore, 8, f);
}

// ---------------------------------------------------------------- game functions
// Byte patterns of the function prologues on build 25472515; every one is unique in .text.
struct Anchor { const char* name; const char* pattern; uintptr_t rva; uintptr_t found; };
Anchor g_anchors[] = {
    { "mpi_body",     "4c 8b dc 4d 89 4b 20 4d 89 43 18 49 89 53 10 53 56 57 41 54 41 55 41 56 41 57 48 81 ec e0 02 00", 0x24b28e0, 0 },
    { "start_helper", "4c 8b dc 49 89 5b 20 c5 fa 11 54 24 18 49 89 53 10 55 56 41 56 48 81 ec",                         0x24aa360, 0 },
    { "sweep",        "4c 8b dc 49 89 5b 10 49 89 73 18 49 89 7b 20 41 54 41 56 41 57 48 81 ec 20 02 00 00 c4 c1 78 29", 0x18f82c0, 0 },
    { "type_lookup",  "8b c2 48 8d 14 40 48 8b 81 98 01 00 00 0f b7 04 90 c3 cc cc cc cc cc cc",                         0x2ccc3b0, 0 },
    { "uaa_body",     "48 8b c4 4c 89 48 20 4c 89 40 18 48 89 50 10 48 89 48 08 53 56 57 41 54 41 55 41 56 41 57 48 81 ec 00 03 00 00 "
                      "c5 f8 29 70 b8 c5 f8 29 78 a8 c5 78 29 40 98 c5 78 29 48 88 c5 78 29 90 78 ff ff ff c5 78 29 98 68 ff ff ff "
                      "c5 78 29 a0 58 ff ff ff c5 78 29 a8 48 ff ff ff c5 78 29 b0 38 ff ff ff c5 78 29 b8 28 ff ff ff 4d 8b f8 48 8b d9 "
                      "c5 fc 10 09 c5 fc 11 88 08 fe ff ff c5 fc 10 51 20",                                                       0x24ae0c0, 0 },
    { "ability_unlocked", "40 57 48 83 ec 30 4c 8b 09 48 8b fa 8b 41 08 48 8d 04 40 48 c1 e0 05 49 03 c1 4c 3b c8 74 10 90 45",     0x2939e10, 0 },
    { "menu_broadcast", "48 89 5c 24 10 56 48 83 ec 30 80 7a 38 00 49 8b f0 48 8b da 0f 84 a2 00 00 00 48 8d 4c 24 20",           0x1ff6470, 0 },
};
enum { A_MPI = 0, A_START, A_SWEEP, A_TYPE, A_UAA, A_UNLOCK, A_MENU };

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
    bool padDown = false, padConsumed = false; DWORD padDownAt = 0, padRetryAt = 0;
    DWORD lastCfgCheck = 0; FILETIME cfgTime{};
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
        if (open) logf("menu opened: [%s] (shift/reset %s)", nm, g_cfg.blockInMenus ? "blocked" : "still allowed, BlockInMenus=0");
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
    bool self = id == f->self || (g_typeLookup && f->scene && g_typeLookup(f->scene, id) == (uint16_t)g_cfg.playerLayer);
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
uint64_t* g_matrix = nullptr;        // physics layer collision matrix: 64 rows of 64 bits (exe + 0x5d1fd50, build-specific)
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & PAGE_NOACCESS)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}
std::vector<std::pair<int, int>> g_cleared;   // (hi, lo) matrix bits we cleared, for restoring
void setMatrixBit(int a, int b, bool on) {
    int hi = a > b ? a : b, lo = a > b ? b : a;
    if (on) { g_matrix[hi] |= 1ull << lo; g_matrix[lo] |= 1ull << hi; }
    else    { g_matrix[hi] &= ~(1ull << lo); g_matrix[lo] &= ~(1ull << hi); }
}
void applyBlockerLayers() {
    if (!g_matrix || !readable(g_matrix, 64 * 8)) return;
    int P = g_cfg.playerLayer;
    if (P < 0 || P >= 64) return;
    for (size_t i = 0; i < g_cleared.size(); ) {
        bool wanted = false;
        for (int k = 0; k < g_cfg.nIgnore; k++) {
            int L = g_cfg.ignoreLayers[k]; int hi = P > L ? P : L, lo = P > L ? L : P;
            if (g_cleared[i].first == hi && g_cleared[i].second == lo) wanted = true;
        }
        if (wanted) { i++; continue; }
        setMatrixBit(g_cleared[i].first, g_cleared[i].second, true);
        logf("blockers: restored collision between layers %d and %d", g_cleared[i].first, g_cleared[i].second);
        g_cleared.erase(g_cleared.begin() + i);
    }
    for (int k = 0; k < g_cfg.nIgnore; k++) {
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
    return (uint8_t)g_isUnlocked(g_s.abilityDb, g_s.worldDb, (uint8_t)g_cfg.abilityId) ? 1 : 0;
}

void requestShift(uint8_t (*b)[32], void* scene) {
    if (g_cfg.requireUnlock) {
        int u = anomalyUnlocked();
        if (u < 0) { logf("shift refused: cannot check the Gravity Anomaly unlock yet (the game's anomaly update has not run)"); return; }
        if (u == 0) { logf("shift refused: the Gravity Anomaly ability (id %d) is not unlocked in this save", g_cfg.abilityId); return; }
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

    Vec chest = add(pos, mul(up, g_cfg.chestHeight));
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
    if (g_cfg.upRay) {
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
        float depth = g_cfg.chestHeight + g_cfg.edgeDepth;
        Vec aheadBottom = sub(aheadTop, mul(up, depth));
        SweepResult d = doSweep(scene, group, aheadTop, aheadBottom, g_cfg.rayRadius, entity);
        logf("edge probe down: hit=%d dist=%.2f (floor level %.2f) normal (%.2f %.2f %.2f)", (int)d.hit, d.distance, g_cfg.chestHeight, d.normal.x, d.normal.y, d.normal.z);
        if (d.hit && d.distance <= g_cfg.chestHeight + g_cfg.floorTol) {
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

void maybeReloadSettings() {
    DWORD now = GetTickCount();
    if (now - g_s.lastCfgCheck < 1000) return;
    g_s.lastCfgCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW((g_dir + L"gravitycontrol_config.ini").c_str(), GetFileExInfoStandard, &fad)) {
        if (CompareFileTime(&fad.ftLastWriteTime, &g_s.cfgTime) != 0) {
            g_s.cfgTime = fad.ftLastWriteTime;
            loadSettings();
            logf("blockers: PlayerLayer=%d IgnoreLayers count=%d", g_cfg.playerLayer, g_cfg.nIgnore);
            logf("settings: Shift=0x%X Hold=%.1f Length=%.1f Radius=%.2f Chest=%.2f UpwardRay=%d AxisOnly=%d RequireUnlock=%d BlockInMenus=%d Diag=%d",
                 g_cfg.keyShift, g_cfg.keyHoldSeconds, g_cfg.rayLength, g_cfg.rayRadius, g_cfg.chestHeight,
                 g_cfg.upRay, g_cfg.axisOnly, g_cfg.requireUnlock, g_cfg.blockInMenus, g_cfg.diagnostics);
        }
    }
    applyBlockerLayers();
}

bool gameHasFocus() {
    HWND fg = GetForegroundWindow(); if (!fg) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void hkBody(uint8_t (*blocks)[32], void* p2, void** scenePtr, float* dt, void* p5) {
    g_s.bodyCalls++;
    maybeReloadSettings();
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
    if (gameHasFocus()) {
        bool doShift = false, doReset = false;
        DWORD know = GetTickCount();
        bool shift = g_cfg.keyShift && (GetAsyncKeyState(g_cfg.keyShift) & 0x8000) != 0;
        if (shift && !g_s.keyShiftDown) { g_s.keyDownAt = know; g_s.keyConsumed = false; }
        if (shift && g_s.keyShiftDown && !g_s.keyConsumed && know - g_s.keyDownAt >= (DWORD)(g_cfg.keyHoldSeconds * 1000.f)) {
            g_s.keyConsumed = true; doReset = true; logf("key: hold -> reset");
        }
        if (!shift && g_s.keyShiftDown && !g_s.keyConsumed) { doShift = true; logf("key: tap -> shift"); }
        g_s.keyShiftDown = shift;
        // Controller: tap = shift, hold for HoldSeconds = reset.
        if (g_cfg.padEnabled && g_xinputGetState) {
            DWORD now = GetTickCount();
            if (now >= g_s.padRetryAt) {
                XINPUT_STATE st{};
                DWORD rc = g_xinputGetState(0, &st);
                if (rc == ERROR_SUCCESS) {
                    bool down = (st.Gamepad.wButtons & (WORD)g_cfg.padButton) != 0;
                    if (down && !g_s.padDown) { g_s.padDownAt = now; g_s.padConsumed = false; }
                    if (down && g_s.padDown && !g_s.padConsumed && now - g_s.padDownAt >= (DWORD)(g_cfg.padHoldSeconds * 1000.f)) {
                        g_s.padConsumed = true; doReset = true; logf("pad: hold -> reset");
                    }
                    if (!down && g_s.padDown && !g_s.padConsumed) { doShift = true; logf("pad: tap -> shift"); }
                    g_s.padDown = down;
                } else {
                    g_s.padDown = false; g_s.padRetryAt = now + 2000;   // no pad: ask again in 2 s
                }
            }
        }
        if ((doShift || doReset) && g_cfg.blockInMenus && menuOpen() == 1) {
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
        logf("diag: anomaly ability %d unlocked=%d (require=%d) | menu open=%d (block=%d)", g_cfg.abilityId, anomalyUnlocked(), g_cfg.requireUnlock, menuOpen(), g_cfg.blockInMenus);
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

const DWORD kTestedBuildStamp = 0x6AB107A0;   // Steam build 25472515
bool g_buildMatch = false;
bool resolveAnchors() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    g_buildMatch = nt->FileHeader.TimeDateStamp == kTestedBuildStamp;
    logf("exe %p timestamp 0x%08X (tested build: 0x%08X) %s", exe, nt->FileHeader.TimeDateStamp, kTestedBuildStamp, g_buildMatch ? "match" : "DIFFERENT: trying anyway");
    g_matrix = (uint64_t*)((uint8_t*)exe + 0x5d1fd50);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t* text = nullptr; size_t textSize = 0; uintptr_t textRva = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) { text = (uint8_t*)exe + sec[i].VirtualAddress; textSize = sec[i].Misc.VirtualSize; textRva = sec[i].VirtualAddress; break; }
    }
    if (!text) { logf("FAIL: no .text section"); return false; }
    bool ok = true;
    for (auto& a : g_anchors) {
        std::string b, m; parsePattern(a.pattern, b, m);
        int count = 0; uintptr_t off = findPattern(text, textSize, b, m, count);
        if (count != 1) { logf("FAIL: anchor %s found %d times (game build changed?)", a.name, count); ok = false; continue; }
        a.found = (uintptr_t)text + off;
        logf("anchor %s at rva 0x%llx (expected 0x%llx) %s", a.name, (unsigned long long)(textRva + off), (unsigned long long)a.rva,
             (textRva + off) == a.rva ? "match" : "MOVED");
    }
    return ok;
}

DWORD WINAPI initThread(LPVOID) {
    loadSettings();
    bool anchorsOk = resolveAnchors();
    if (!g_buildMatch && g_cfg.versionWarning) {
        MessageBoxW(nullptr,
            anchorsOk ? L"Your game version is different from the one this version of GravityControl was made for, so there might be issues.\n\n"
                        L"To turn this warning off, set VersionWarning=0 in crmods\\gravitycontrol_config.ini."
                      : L"Your game version is different from the one this version of GravityControl was made for, and the mod could not find its hook points in it.\n\n"
                        L"GravityControl stays inactive until it is updated. Details are in crmods\\gravitycontrol.log.",
            L"GravityControl", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    if (!anchorsOk) { logf("GravityControl inactive: anchors missing, the game runs unmodified"); return 0; }
    if (MH_Initialize() != MH_OK) { logf("FAIL: MinHook init"); return 0; }
    {
        HMODULE xi = LoadLibraryW(L"xinput1_4.dll");
        if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
        if (xi) g_xinputGetState = (XInputGetStateFn)GetProcAddress(xi, "XInputGetState");
        logf("controller: %s", g_xinputGetState ? "XInputGetState ready (d-pad up: tap = shift, hold = reset)" : "XInput not available");
    }
    g_sweep = (SweepFn)g_anchors[A_SWEEP].found;
    g_typeLookup = (TypeLookupFn)g_anchors[A_TYPE].found;
    g_isUnlocked = (UnlockFn)g_anchors[A_UNLOCK].found;
    if (MH_CreateHook((void*)g_anchors[A_START].found, (void*)hkStart, (void**)&g_origStart) != MH_OK) { logf("FAIL: hook start_helper"); return 0; }
    if (MH_CreateHook((void*)g_anchors[A_MPI].found, (void*)hkBody, (void**)&g_origBody) != MH_OK) { logf("FAIL: hook mpi_body"); return 0; }
    if (MH_CreateHook((void*)g_anchors[A_UAA].found, (void*)hkUaa, (void**)&g_origUaa) != MH_OK) { logf("FAIL: hook uaa_body"); return 0; }
    if (MH_CreateHook((void*)g_anchors[A_MENU].found, (void*)hkMenu, (void**)&g_origMenu) != MH_OK) { logf("FAIL: hook menu_broadcast"); return 0; }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { logf("FAIL: enable hooks"); return 0; }
    logf("active: GravityControl 1.0 - tap the Shift key (0x%X) or d-pad up to shift gravity, hold it to reset", g_cfg.keyShift);
    return 0;
}

} // namespace

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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(inst, path, MAX_PATH);
        g_dir = path; size_t p = g_dir.find_last_of(L"\\/"); g_dir = g_dir.substr(0, p + 1);
        InitializeCriticalSection(&g_logLock);
        g_t0 = GetTickCount();
        _wfopen_s(&g_log, (g_dir + L"gravitycontrol.log").c_str(), L"w");
        logf("GravityControl 1.0 loading from %ls", g_dir.c_str());
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf("game exiting");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
