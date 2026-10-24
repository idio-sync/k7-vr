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

}  // namespace k7
