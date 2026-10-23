// Memory probing commands for finding game variables (camera FOV etc.):
//   scanf LO HI        log writable addresses holding a float in [LO, HI] (first 300)
//   rescanf LO HI      keep only previous hits still in [LO, HI]
//   peekf ADDR         log the float at ADDR (hex)
//   pokef ADDR VALUE   write a float at ADDR (hex)
//   pokehits VALUE     write VALUE to every current hit
#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace k7 {

static std::vector<uintptr_t> g_hits;

static bool readable_rw(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;
}

static bool in_own_module(uintptr_t addr) {
    static uintptr_t base, end;
    if (!base) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&in_own_module), &self);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(self) + dos->e_lfanew);
        base = reinterpret_cast<uintptr_t>(self);
        end = base + nt->OptionalHeader.SizeOfImage;
    }
    return addr >= base && addr < end;
}

static void log_hits(const char* what) {
    std::string line;
    char buf[64];
    size_t shown = 0;
    for (uintptr_t a : g_hits) {
        if (shown++ == 300) break;
        float v;
        memcpy(&v, reinterpret_cast<void*>(a), 4);
        snprintf(buf, sizeof(buf), " %08x=%g", unsigned(a), v);
        line += buf;
        if (line.size() > 900) {
            logf("%s:%s", what, line.c_str());
            line.clear();
        }
    }
    logf("%s: %zu hits%s", what, g_hits.size(), line.empty() ? "" : (std::string(" ...") + line).c_str());
}

void mem_scanf(float lo, float hi) {
    g_hits.clear();
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000; a = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
        if (!readable_rw(mbi) || mbi.Type == MEM_MAPPED || in_own_module(a)) continue;
        auto p = static_cast<const float*>(mbi.BaseAddress);
        size_t n = mbi.RegionSize / 4;
        for (size_t i = 0; i < n; ++i)
            if (p[i] >= lo && p[i] <= hi) g_hits.push_back(uintptr_t(p + i));
    }
    log_hits("scanf");
}

void mem_rescanf(float lo, float hi) {
    std::vector<uintptr_t> keep;
    for (uintptr_t a : g_hits) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || !readable_rw(mbi)) continue;
        float v;
        memcpy(&v, reinterpret_cast<void*>(a), 4);
        if (v >= lo && v <= hi) keep.push_back(a);
    }
    g_hits.swap(keep);
    log_hits("rescanf");
}

void mem_peekf(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        logf("peekf %08x: not mapped", unsigned(addr));
        return;
    }
    float v;
    memcpy(&v, reinterpret_cast<void*>(addr), 4);
    logf("peekf %08x = %g", unsigned(addr), v);
}

void mem_pokef(uintptr_t addr, float value) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || !readable_rw(mbi)) {
        logf("pokef %08x: not writable", unsigned(addr));
        return;
    }
    memcpy(reinterpret_cast<void*>(addr), &value, 4);
    logf("pokef %08x = %g", unsigned(addr), value);
}

}  // namespace k7

namespace k7 {

void mem_pokehits(float value) {
    size_t n = 0;
    for (uintptr_t a : g_hits) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || !readable_rw(mbi)) continue;
        memcpy(reinterpret_cast<void*>(a), &value, 4);
        ++n;
    }
    logf("pokehits %g: wrote %zu of %zu", value, n, g_hits.size());
}

}  // namespace k7
