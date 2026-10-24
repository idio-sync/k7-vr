// The game's first-person aim mode (M4): detecting it, reading its camera, steering it.
//
// Aim controller (global, exe RVA 0x46bde8): pitch +0xc4, yaw +0xc8 (radians; copies at +0xd0 /
// +0xd4), aim eye +0xdc, look-at target +0xf4 (eye + 100 * direction), angle velocities +0x150 /
// +0x154. The camera manager (pointer at RVA 0x46fa54) holds the render camera's eye +0x0c and
// target +0x24; in aim mode they are the aim controller's. Yaw wraps freely; pitch is clamped to
// +-pi/5 by two functions (RVAs 1b0391/1b0399 for the aim controller, 1b3c89/1b3c91), which read
// the limits from shared .rdata constants. The mod points those four instructions at its own pair
// of limits instead (the constants are used elsewhere too).
#include "common.h"

#include <cmath>
#include <initializer_list>
#include <cstring>

namespace k7 {

namespace {

constexpr uintptr_t kAimRva = 0x46bde8;
constexpr uintptr_t kManagerPtrRva = 0x46fa54;

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_vec(uintptr_t addr, float out[3]) { return guarded_copy(out, reinterpret_cast<void*>(addr), 12); }

bool manager(uintptr_t* mgr) {
    *mgr = 0;
    return guarded_copy(mgr, reinterpret_cast<void*>(exe_base() + kManagerPtrRva), 4) && *mgr;
}

float g_limits[2];  // -limit, +limit: what the patched clamp instructions read

}  // namespace

bool aim_mode_now() {
    uintptr_t mgr;
    float m_eye[3], m_tgt[3], a_eye[3], a_tgt[3];
    const uintptr_t aim = exe_base() + kAimRva;
    if (!manager(&mgr) || !read_vec(mgr + 0x0c, m_eye) || !read_vec(mgr + 0x24, m_tgt) ||
        !read_vec(aim + 0xdc, a_eye) || !read_vec(aim + 0xf4, a_tgt))
        return false;
    for (int i = 0; i < 3; ++i)
        if (fabsf(m_eye[i] - a_eye[i]) > 1e-3f || fabsf(m_tgt[i] - a_tgt[i]) > 1e-3f) return false;
    return true;
}

bool aim_camera(float eye[3], float target[3]) {
    uintptr_t mgr;
    return manager(&mgr) && read_vec(mgr + 0x0c, eye) && read_vec(mgr + 0x24, target);
}

void aim_set(float yaw, float pitch) {
    const uintptr_t aim = exe_base() + kAimRva;
    const float angles[2] = {pitch, yaw};
    const float zero[2] = {};
    guarded_copy(reinterpret_cast<void*>(aim + 0xc4), angles, 8);
    guarded_copy(reinterpret_cast<void*>(aim + 0xd0), angles, 8);
    guarded_copy(reinterpret_cast<void*>(aim + 0x150), zero, 8);
}

bool aim_widen_clamp(float limit_rad) {
    static const struct {
        uintptr_t rva;
        BYTE modrm;          // xmm register in the reg field, rm = disp32
        bool max;            // maxss (lower limit) or minss (upper limit)
    } kSites[] = {{0x1b0391, 0x1d, true}, {0x1b0399, 0x1d, false}, {0x1b3c89, 0x25, true}, {0x1b3c91, 0x25, false}};
    const uintptr_t base = exe_base();
    // Check every site before touching any.
    for (const auto& s : kSites) {
        const BYTE* p = reinterpret_cast<const BYTE*>(base + s.rva);
        uint32_t disp;
        memcpy(&disp, p + 4, 4);
        const uintptr_t want = base + (s.max ? 0x42c214 : 0x42b494);
        if (p[0] != 0xf3 || p[1] != 0x0f || p[2] != (s.max ? 0x5f : 0x5d) || p[3] != s.modrm || disp != want) {
            logf("aim: pitch clamp at exe+%x isn't the expected instruction; limit stays at 36 degrees", unsigned(s.rva));
            return false;
        }
    }
    g_limits[0] = -limit_rad;
    g_limits[1] = limit_rad;
    for (const auto& s : kSites) {
        BYTE* p = reinterpret_cast<BYTE*>(base + s.rva);
        const uint32_t disp = uint32_t(reinterpret_cast<uintptr_t>(s.max ? &g_limits[0] : &g_limits[1]));
        DWORD old;
        VirtualProtect(p + 4, 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(p + 4, &disp, 4);
        VirtualProtect(p + 4, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    logf("aim: pitch limit widened to %.1f degrees", limit_rad * 180 / 3.14159265f);
    return true;
}

// ---- Laser-pointer screens ----
// The silhouette you shoot to start a target: a red dot on a plane in front of the game camera.
// Its state object (heap) starts with x, y (game camera eye space; the dot is drawn at
// (x, y, z + 0x20) moved 50 units forward, so its plane is 50 - z units away), velocities at
// +0x24/+0x28 (the idle sway), screen x/y at +0x0c/+0x10 (what the hit test reads, computed
// later from x, y). Every frame its update ends in a clamp (exe+237700, ecx = the object); a
// stub there records the object and, while the mod steers it, writes x, y first and zeroes the
// sway, so the clamp and everything after it (screen position, hit test) use the hand's aim.

namespace {

constexpr uintptr_t kDotClampRva = 0x237700;  // movss xmm0, [0x82b8bc] (8 bytes), then resume
constexpr float kDotForward = 50;             // the dot draw's model-view translation

uintptr_t g_dot_obj;             // written by the stub
volatile LONG g_dot_calls;       // incremented by the stub
uint32_t g_dot_override;         // nonzero: the stub writes g_dot_xy
float g_dot_xy[2];
LONG g_dot_calls_seen;

void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

}  // namespace

bool pointer_install() {
    const uintptr_t base = exe_base();
    const uintptr_t site = base + kDotClampRva;
    uint8_t want[8] = {0xf3, 0x0f, 0x10, 0x05};
    put32(want + 4, uint32_t(base + 0x42b8bc));
    if (memcmp(reinterpret_cast<void*>(site), want, 8) != 0) {
        logf("pointer: unexpected bytes at exe+%x; laser-pointer screens use the game's controls", unsigned(kDotClampRva));
        return false;
    }
    auto stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t* p = stub;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) *p++ = b; };
    auto emit32 = [&](uint32_t v) { put32(p, v), p += 4; };
    emit({0x89, 0x0d}), emit32(uint32_t(uintptr_t(&g_dot_obj)));        // mov [g_dot_obj], ecx
    emit({0xf0, 0xff, 0x05}), emit32(uint32_t(uintptr_t(&g_dot_calls)));  // lock inc [g_dot_calls]
    emit({0x83, 0x3d}), emit32(uint32_t(uintptr_t(&g_dot_override))), emit({0x00});  // cmp [g_dot_override], 0
    uint8_t* je = p;
    emit({0x74, 0x00});                                                 // je skip
    emit({0x50});                                                       // push eax
    emit({0xa1}), emit32(uint32_t(uintptr_t(&g_dot_xy[0])));            // mov eax, [x]
    emit({0x89, 0x01});                                                 // mov [ecx], eax
    emit({0xa1}), emit32(uint32_t(uintptr_t(&g_dot_xy[1])));            // mov eax, [y]
    emit({0x89, 0x41, 0x04});                                           // mov [ecx+4], eax
    emit({0x31, 0xc0});                                                 // xor eax, eax
    emit({0x89, 0x41, 0x24});                                           // mov [ecx+0x24], eax
    emit({0x89, 0x41, 0x28});                                           // mov [ecx+0x28], eax
    emit({0x58});                                                       // pop eax
    je[1] = uint8_t(p - (je + 2));
    memcpy(p, want, 8), p += 8;                                         // the original instruction
    emit({0xe9}), emit32(uint32_t((site + 8) - uintptr_t(p + 4)));      // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, 128);

    uint8_t jmp[5] = {0xe9};
    put32(jmp + 1, uint32_t(uintptr_t(stub) - (site + 5)));
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(site), jmp, 5);
    VirtualProtect(reinterpret_cast<void*>(site), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    logf("pointer: laser-pointer screens follow the right controller in VR");
    return true;
}

bool pointer_active(float* plane_distance) {
    const LONG calls = g_dot_calls;
    const bool active = calls != g_dot_calls_seen && g_dot_obj;
    g_dot_calls_seen = calls;
    if (!active) return false;
    static uintptr_t logged;
    if (logged != g_dot_obj) logf("pointer: dot object %p", reinterpret_cast<void*>(logged = g_dot_obj));
    float z = 0;
    if (!guarded_copy(&z, reinterpret_cast<void*>(g_dot_obj + 0x20), 4)) return false;
    *plane_distance = kDotForward - z;
    return *plane_distance > 1;
}

void pointer_set(bool on, float x, float y) {
    g_dot_xy[0] = x;
    g_dot_xy[1] = y;
    g_dot_override = on ? 1 : 0;
}

}  // namespace k7
