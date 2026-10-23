#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace k7 {

std::wstring g_game_dir;
std::wstring g_dir;

static FILE* g_log;
static std::mutex g_log_mx;
static ULONGLONG g_start;

void init_paths() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_game_dir = buf;
    g_game_dir.resize(g_game_dir.find_last_of(L"\\/"));
    g_dir = g_game_dir + L"\\k7vr";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_start = GetTickCount64();
}

std::wstring path_in(const wchar_t* name) { return g_dir + L"\\" + name; }

void log_open() { g_log = _wfopen(path_in(L"k7vr.log").c_str(), L"w"); }

void logf(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard lock(g_log_mx);
    fprintf(g_log, "[%8.3f] ", (GetTickCount64() - g_start) / 1000.0);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

}  // namespace k7
