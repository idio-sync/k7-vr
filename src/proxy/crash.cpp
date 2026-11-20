// Crash reports in k7vr.log: where the game (or the mod) crashed, so a player's log alone says
// which module and code address failed.
//
// A vectored handler sees every fatal-kind exception first, before any handler of the game's
// (the game has its own crash filter, which may replace ours), and writes the report at once:
// the exception, the faulting module+offset, the registers and the code addresses found on the
// stack (likely callers). An exception can still be handled after that (the mod's guarded reads
// of game memory catch their own faults), so the report says "if the game closes now"; the
// unhandled-exception filter then confirms it. A normal exit logs "the game is exiting", so a
// log that just stops means the process was killed or failed fast (no handler runs then).
#include "common.h"

#include <atomic>
#include <cstdio>

namespace k7 {

namespace {

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
        case 0xC0000374: return "heap corruption";
        default: return nullptr;  // not a crash kind (C++ exceptions, breakpoints, thread names...)
    }
}

// "Module.dll+0x1234" for a code address, or the bare address outside any module.
void name_address(uintptr_t addr, char* out, size_t size) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        GetModuleFileNameW(mod, path, MAX_PATH)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls+0x%x", name ? name + 1 : path, unsigned(addr - reinterpret_cast<uintptr_t>(mod)));
    } else {
        snprintf(out, size, "0x%08x (no module)", unsigned(addr));
    }
}

// Whether addr is in executable memory of a loaded module (a likely return address).
bool is_module_code(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && (mbi.Protect & exec);
}

// One stack slot, or false when the stack can't be read there (no C++ objects: __try).
bool read_slot(const uintptr_t* p, uintptr_t* out) {
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<int> g_reports{0};
std::atomic<bool> g_reporting{false};
uintptr_t g_last_addr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;

constexpr int kMaxReports = 4;      // a crash loop or a run of handled faults stays readable
constexpr int kStackSlots = 1024;   // 4 KB of stack scanned for callers
constexpr int kMaxCallers = 16;

// Static buffers: a stack overflow leaves the handler little stack of its own.
char g_name[MAX_PATH + 32];
char g_line[512];

// A stack overflow is usually a recursion: count the code addresses over the whole stack (the
// ones that repeat are the loop) and list the oldest callers, near the stack's base (what
// started it).
void report_recursion(const uintptr_t* sp) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(sp, &mbi, sizeof(mbi))) return;
    const auto* top = reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
    constexpr int kDistinct = 128;
    static uintptr_t addr[kDistinct];
    static int count[kDistinct];
    int distinct = 0;
    for (const uintptr_t* p = sp; p < top; ++p) {
        uintptr_t v;
        if (!read_slot(p, &v) || !is_module_code(v)) continue;
        int i = 0;
        while (i < distinct && addr[i] != v) ++i;
        if (i == distinct) {
            if (distinct == kDistinct) continue;
            addr[distinct] = v;
            count[distinct++] = 0;
        }
        ++count[i];
    }
    logf("  stack %u KB; code addresses seen most often (a recursion repeats them):",
         unsigned((top - sp) * sizeof(uintptr_t) / 1024));
    for (int shown = 0; shown < 8; ++shown) {
        int best = -1;
        for (int i = 0; i < distinct; ++i)
            if (count[i] > 1 && (best < 0 || count[i] > count[best])) best = i;
        if (best < 0) break;
        name_address(addr[best], g_name, sizeof(g_name));
        logf("    %5d x %s", count[best], g_name);
        count[best] = -count[best];  // shown
    }
    logf("  oldest code addresses on the stack (what started it):");
    int found = 0;
    for (const uintptr_t* p = top - 1; p >= sp && found < kMaxCallers; --p) {
        uintptr_t v;
        if (!read_slot(p, &v) || !is_module_code(v)) continue;
        name_address(v, g_name, sizeof(g_name));
        logf("    base-0x%04x: %s", unsigned((top - p) * sizeof(uintptr_t)), g_name);
        ++found;
    }
}

void report(EXCEPTION_POINTERS* ep, const char* name) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
    name_address(at, g_name, sizeof(g_name));
    logf("CRASH? %s (0x%08x) at %s, thread %lu; if the game closes now, this is why", name,
         unsigned(er->ExceptionCode), g_name, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        logf("  %s address 0x%08x", kind == 1 ? "writing" : kind == 8 ? "executing" : "reading",
             unsigned(er->ExceptionInformation[1]));
    }
#ifdef _M_IX86
    logf("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x", unsigned(c->Eax), unsigned(c->Ebx),
         unsigned(c->Ecx), unsigned(c->Edx), unsigned(c->Esi), unsigned(c->Edi));
    logf("  eip=%08x esp=%08x ebp=%08x", unsigned(c->Eip), unsigned(c->Esp), unsigned(c->Ebp));
    const auto* sp = reinterpret_cast<const uintptr_t*>(c->Esp);
#else
    const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
#endif
    int found = 0;
    for (int i = 0; i < kStackSlots && found < kMaxCallers; ++i) {
        uintptr_t v;
        if (!read_slot(sp + i, &v)) break;
        if (!is_module_code(v)) continue;
        name_address(v, g_name, sizeof(g_name));
        snprintf(g_line, sizeof(g_line), "  stack+0x%03x: %s", unsigned(i * sizeof(uintptr_t)), g_name);
        logf("%s", g_line);
        ++found;
    }
    if (!found) logf("  (no code addresses on the stack)");
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) report_recursion(sp);
}

struct ReportJob {
    EXCEPTION_POINTERS* ep;
    const char* name;
};

DWORD WINAPI report_thread(void* p) {
    auto* job = static_cast<ReportJob*>(p);
    report(job->ep, job->name);
    return 0;
}

// Reports on a fresh thread, so a stack overflow (or a nearly full stack) can't take the report
// down with it. If no thread starts, the report runs here, unless the stack is the problem.
// (A thread held at the loader lock the crashing thread owns reports nothing within the wait.)
void report_safely(EXCEPTION_POINTERS* ep, const char* name) {
    static ReportJob job;  // outlives a wait that times out
    job = {ep, name};
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, report_thread, &job, 0, nullptr)) {
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
        return;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) report(ep, name);
    else logf("CRASH? stack overflow at %p; no thread to report it from", ep->ExceptionRecord->ExceptionAddress);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const char* name = exception_name(ep->ExceptionRecord->ExceptionCode);
    if (!name) return EXCEPTION_CONTINUE_SEARCH;
    const int n = g_reports.fetch_add(1);
    if (n < kMaxReports && !g_reporting.exchange(true)) {
        report_safely(ep, name);
        g_last_addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        g_reporting = false;
    } else if (n == kMaxReports) {
        logf("CRASH? more exceptions; not reported");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    name_address(at, g_name, sizeof(g_name));
    logf("CRASH: unhandled exception 0x%08x at %s: the game closes%s", unsigned(ep->ExceptionRecord->ExceptionCode),
         g_name, at == g_last_addr ? " (details above)" : "");
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// Killer7Win.exe's link timestamp in the Steam build the mod's addresses come from.
constexpr DWORD kGameStamp = 0x672916ec;

void install_crash_log() {
    AddVectoredExceptionHandler(1, on_exception);
    g_prev_filter = SetUnhandledExceptionFilter(on_unhandled);
    // The game's build: the mod patches fixed addresses in it, so another build would crash.
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp;
    logf("game exe: timestamp %08x, image size %x (%s)", unsigned(stamp), unsigned(nt->OptionalHeader.SizeOfImage),
         stamp == kGameStamp ? "the build the mod is made for"
                             : "NOT the build the mod is made for: expect crashes or wrong pictures");
}

void log_exit(bool process_exit) {
    if (!process_exit)
        logf("the mod is unloading");
    else if (g_reports > 0)
        logf("the game is exiting, after the exception reported above (its own crash handler)");
    else
        logf("the game is exiting");
}

void crash_test() {
    logf("crashtest: writing to address 0 on purpose");
    *static_cast<volatile int*>(nullptr) = 7;
}

}  // namespace k7
