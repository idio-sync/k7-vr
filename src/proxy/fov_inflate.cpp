// FOV inflation: the game frustum-culls against its camera's FOV, so looking around in VR
// would show unsubmitted geometry. The camera manager copies the FOV into the render camera
// at exe+10301a (`movss [eax+0x40], xmm0`); a stub there records the intended FOV and writes
// an inflated one instead. The patched vertex shader then projects perspective draws with
// the intended FOV (flat) or the headset's projection (VR), so framing is unaffected.
#include "common.h"

#include <cmath>

namespace k7 {

constexpr uintptr_t kCopyFovRva = 0x10301a;  // movss dword ptr [eax+0x40], xmm0
constexpr uintptr_t kResumeRva = 0x10301f;
static const uint8_t kOriginal[5] = {0xF3, 0x0F, 0x11, 0x40, 0x40};

static float g_intended = 0;  // written by the stub every frame
static float g_inflated = 0;  // the FOV the game culls and projects with
static uint8_t* g_stub;
static bool g_patched;

static bool write_code(uintptr_t addr, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(addr), bytes, n);
    VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

void set_fov_inflate(float degrees) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t site = base + kCopyFovRva;
    if (degrees <= 0) {
        if (g_patched && write_code(site, kOriginal, sizeof(kOriginal))) g_patched = false;
        g_inflated = 0;
        logf("fov_inflate off");
        return;
    }
    if (!g_patched) {
        if (memcmp(reinterpret_cast<void*>(site), kOriginal, sizeof(kOriginal)) != 0) {
            logf("fov_inflate: unexpected bytes at exe+%x, not patching", unsigned(kCopyFovRva));
            return;
        }
        if (!g_stub) {
            g_stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            uint8_t* p = g_stub;
            // movss [g_intended], xmm0
            p[0] = 0xF3, p[1] = 0x0F, p[2] = 0x11, p[3] = 0x05;
            put32(p + 4, uint32_t(uintptr_t(&g_intended)));
            p += 8;
            // movss xmm0, [g_inflated]
            p[0] = 0xF3, p[1] = 0x0F, p[2] = 0x10, p[3] = 0x05;
            put32(p + 4, uint32_t(uintptr_t(&g_inflated)));
            p += 8;
            // movss [eax+0x40], xmm0 (the original instruction)
            memcpy(p, kOriginal, sizeof(kOriginal));
            p += sizeof(kOriginal);
            // jmp back
            p[0] = 0xE9;
            put32(p + 1, uint32_t((base + kResumeRva) - uintptr_t(p + 5)));
            FlushInstructionCache(GetCurrentProcess(), g_stub, 64);
        }
        uint8_t jmp[5] = {0xE9};
        put32(jmp + 1, uint32_t(uintptr_t(g_stub) - (site + 5)));
        if (!write_code(site, jmp, sizeof(jmp))) {
            logf("fov_inflate: patch failed");
            return;
        }
        g_patched = true;
    }
    g_inflated = degrees;
    logf("fov_inflate %g", degrees);
}

// The FOV the game meant to use this frame (degrees), or 0 when not inflating.
float fov_inflate_intended() { return (g_patched && g_inflated > 0) ? g_intended : 0; }

}  // namespace k7
