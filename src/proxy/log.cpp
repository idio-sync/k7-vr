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
    // A first run (the mod's two DLLs just dropped into the game folder): a settings file with
    // the main settings, all commented out, so it documents them without pinning any default.
    const std::wstring ini = g_dir + L"\\k7vr.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        if (FILE* f = _wfopen(ini.c_str(), L"w")) {
            fputs("; Killer7 VR settings. Remove the ; in front of a line to change it.\n"
                  "; The full list is in the mod's README (docs/development.md).\n"
                  "[vr]\n"
                  "; auto = VR when a headset is connected, openvr = always VR, off = play flat\n"
                  ";mode=auto\n"
                  "; eye image size as a multiple of SteamVR's recommended size (try 0.8 if it stutters)\n"
                  ";eye_scale=1.0\n"
                  "; metres per game unit (world size)\n"
                  ";unit_m=0.25\n"
                  "; controller rumble strength, 0..1 (0 = off)\n"
                  ";rumble=1\n"
                  "; 1 draws a laser from the gun while aiming\n"
                  ";aim_laser=0\n",
                  f);
            fclose(f);
        }
}

std::wstring path_in(const wchar_t* name) { return g_dir + L"\\" + name; }

#ifndef K7VR_VERSION
#define K7VR_VERSION "unknown"
#endif

void log_open() {
    g_log = _wfopen(path_in(L"k7vr.log").c_str(), L"w");
    logf("killer7-vr %s", K7VR_VERSION);
}

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
