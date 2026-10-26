// Memory probing commands for finding game variables (camera FOV etc.):
//   scanf LO HI        log writable addresses holding a float in [LO, HI] (first 300)
//   rescanf LO HI      keep only previous hits still in [LO, HI]
//   peekf ADDR         log the float at ADDR (hex)
//   pokef ADDR VALUE   write a float at ADDR (hex)
//   pokehits VALUE     write VALUE to every current hit
//   scanpair A B [TOL] [WINDOW]   floats near A with a float near B within WINDOW bytes
//   watch ADDR [HITS]  hardware write watch on ADDR, logging the writer (ADDR 0 clears)
#include "common.h"

#include <cmath>
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

void mem_peekd(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        logf("peekd %08x: not mapped", unsigned(addr));
        return;
    }
    uint32_t v;
    memcpy(&v, reinterpret_cast<void*>(addr), 4);
    logf("peekd %08x = %08x", unsigned(addr), v);
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

#include <tlhelp32.h>

#include <atomic>

namespace k7 {

// scanpair A B TOL WINDOW: floats within TOL of A that have a float within TOL of B no more
// than WINDOW bytes away (e.g. a projection's x and y scale). Replaces the hit list.
void mem_scanpair(float a, float b, float tol, int window) {
    g_hits.clear();
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t r = 0x10000; r < 0x7FFF0000; r = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(r), &mbi, sizeof(mbi))) break;
        if (!readable_rw(mbi) || mbi.Type == MEM_MAPPED || in_own_module(r)) continue;
        auto p = static_cast<const float*>(mbi.BaseAddress);
        const ptrdiff_t n = ptrdiff_t(mbi.RegionSize / 4), w = window / 4;
        for (ptrdiff_t i = 0; i < n; ++i) {
            if (!(fabsf(p[i] - a) <= tol)) continue;  // also skips NaN
            for (ptrdiff_t j = (i - w < 0 ? 0 : i - w); j <= i + w && j < n; ++j)
                if (j != i && fabsf(p[j] - b) <= tol) {
                    g_hits.push_back(uintptr_t(p + i));
                    break;
                }
        }
    }
    log_hits("scanpair");
}

// ---- Hardware write watch (debug register DR0) ----
// watch ADDR [HITS]: on every thread, break on 4-byte writes to ADDR; the vectored handler logs
// the writing instruction and return addresses into the exe, then disarms after HITS hits.

static std::atomic<uintptr_t> g_watch_addr;
static std::atomic<int> g_watch_left;
static uintptr_t g_exe_lo, g_exe_hi;

static bool safe_read(const uintptr_t* p, uintptr_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    int left = --g_watch_left;
    if (left >= 0) {
        std::string chain;
        char buf[24];
        auto sp = reinterpret_cast<const uintptr_t*>(c->Esp);
        int found = 0;
        for (int i = 0; i < 256 && found < 10; ++i) {
            uintptr_t v;
            if (!safe_read(sp + i, &v)) break;
            if (v >= g_exe_lo && v < g_exe_hi) {
                snprintf(buf, sizeof(buf), " +%x", unsigned(v - g_exe_lo));
                chain += buf;
                ++found;
            }
        }
        float v = 0;
        memcpy(&v, reinterpret_cast<void*>(g_watch_addr.load()), 4);
        logf("watch %08x = %g by exe+%x (tid %lu) eax=%08lx ecx=%08lx edx=%08lx ebx=%08lx esi=%08lx edi=%08lx "
             "ebp=%08lx stack:%s",
             unsigned(g_watch_addr.load()), v, unsigned(c->Eip - g_exe_lo), GetCurrentThreadId(), c->Eax, c->Ecx,
             c->Edx, c->Ebx, c->Esi, c->Edi, c->Ebp, chain.c_str());
    }
    if (left <= 0) c->Dr7 &= ~uintptr_t(3);  // disarm on this thread
    return EXCEPTION_CONTINUE_EXECUTION;
}

static bool g_watch_rw;  // break on reads too (R/W0 = 11)

static DWORD WINAPI watch_arm_thread(void* param) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(param);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = addr;
                // L0 enable, R/W0 = 01 (write) or 11 (read/write), LEN0 = 11 (4 bytes); addr = 0 clears.
                c.Dr7 = addr ? (c.Dr7 & ~0xF0003ull) | 1 | ((g_watch_rw ? 3u : 1u) << 16) | (3 << 18) : c.Dr7 & ~0xF0003ull;
                if (SetThreadContext(t, &c)) ++armed;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    logf("watch %08x %s on %d threads", unsigned(addr), addr ? "armed" : "cleared", armed);
    return 0;
}

void mem_watch(uintptr_t addr, int hits, bool rw) {
    g_watch_rw = rw;
    static PVOID handler = AddVectoredExceptionHandler(1, watch_handler);
    (void)handler;
    if (!g_exe_lo) {
        auto exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
        g_exe_lo = uintptr_t(exe);
        g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    }
    g_watch_addr = addr;
    g_watch_left = hits;
    // Debug registers of the calling thread can't be set from itself; a helper thread arms all others.
    HANDLE t = CreateThread(nullptr, 0, watch_arm_thread, reinterpret_cast<void*>(addr), 0, nullptr);
    if (t) CloseHandle(t);
}

void mem_watchhit(size_t index, int hits) {
    if (index >= g_hits.size()) {
        logf("watchhit %zu: only %zu hits", index, g_hits.size());
        return;
    }
    mem_watch(g_hits[index], hits);
}

// patch ADDR HEX: overwrite code/data bytes (logs the old bytes so the patch can be undone).
void mem_patch(uintptr_t addr, const std::vector<uint8_t>& bytes) {
    DWORD old;
    if (bytes.empty() || !VirtualProtect(reinterpret_cast<void*>(addr), bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
        logf("patch %08x: failed", unsigned(addr));
        return;
    }
    std::string was;
    char buf[4];
    for (size_t i = 0; i < bytes.size(); ++i) {
        snprintf(buf, sizeof(buf), "%02x", reinterpret_cast<uint8_t*>(addr)[i]);
        was += buf;
    }
    memcpy(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
    VirtualProtect(reinterpret_cast<void*>(addr), bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), bytes.size());
    logf("patch %08x: %zu bytes (was %s)", unsigned(addr), bytes.size(), was.c_str());
}

// dumpf ADDR COUNT [TAG]: log COUNT floats from ADDR, 8 per line as "dump TAG +off: v v v ...".
void mem_dumpf(uintptr_t addr, int count, const std::string& tag) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        logf("dumpf %08x: not mapped", unsigned(addr));
        return;
    }
    const uintptr_t end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
    for (int i = 0; i < count; i += 8) {
        std::string line;
        char buf[24];
        for (int j = i; j < i + 8 && j < count && addr + 4 * j + 4 <= end; ++j) {
            float v;
            memcpy(&v, reinterpret_cast<void*>(addr + 4 * j), 4);
            snprintf(buf, sizeof(buf), " %.6g", v);
            line += buf;
        }
        logf("dump %s +%03x:%s", tag.c_str(), unsigned(i * 4), line.c_str());
    }
}

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
