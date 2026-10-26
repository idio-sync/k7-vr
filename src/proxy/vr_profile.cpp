// The VR profile: in VR the game reads and writes k7vr\killer7.ini instead of the player's
// Saved Games\killer7\killer7.ini. The copy is seeded from the player's file (again whenever
// theirs is newer), and on every open its keyboard bindings are set to the keys the VR
// controllers send and fullscreen is turned off (ini_bindings.cpp). The player's own file is
// never written, so playing flat (mode=off, or Shift held at start) keeps their settings.
//
// Why: by default keyboard fire is unbound (KEY_NONE) and exit_game is Escape, the same key as
// decline/skip, so the controllers' fire wouldn't work and B could quit to the title.
// Not used in the test copy (its Saved Games is already private: hooks_folder.cpp), except for
// testing: [probe] vr_profile=1 forces it on, vr_profile_source=PATH replaces the player's file.
//
// Also, always: the game's shader cache (Shaders\pipeline.cache) is never read, so every shader
// goes through D3DCompile and gets the stereo patch, and the game's writes of it go to
// k7vr\pipeline.cache, so its own cache stays as shipped (flat play after uninstalling).
#include "common.h"

#include <shlobj.h>

#include <cstdio>
#include <string>

#include "ini_bindings.h"

namespace k7 {

namespace {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileW_t o_CreateFileW;
CreateFileA_t o_CreateFileA;

std::wstring g_real;  // the player's killer7.ini (looked up on first use)
std::wstring g_copy;  // k7vr\killer7.ini
SRWLOCK g_lock = SRWLOCK_INIT;

bool read_all(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[4096];
    out.clear();
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) out.append(buf, n);
    fclose(f);
    return true;
}

bool write_all(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

bool newer(const std::wstring& a, const std::wstring& b) {  // a modified after b (or b missing)
    WIN32_FILE_ATTRIBUTE_DATA da, db;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &da)) return false;
    if (!GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &db)) return true;
    return CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime) > 0;
}

// The copy, made current: seeded from the player's file when that is newer, bindings applied.
void prepare_copy() {
    std::string text;
    if (newer(g_real, g_copy)) {
        if (read_all(g_real, text)) logf("vr profile: seeded from %ls", g_real.c_str());
    } else {
        read_all(g_copy, text);
    }
    if (text.empty()) return;  // nothing to seed from: the game writes the copy itself
    const std::string fixed = vr_ini_apply(text);
    std::string have;
    if (!read_all(g_copy, have) || have != fixed) write_all(g_copy, fixed);
}

bool is_real(const wchar_t* name) {
    if (!name) return false;
    const size_t n = wcslen(name);
    if (n < 12 || _wcsicmp(name + n - 12, L"\\killer7.ini") != 0) return false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_real.empty()) {  // looked up here rather than in DllMain (loader lock)
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &dir))) {
            g_real = std::wstring(dir) + L"\\killer7\\killer7.ini";
            CoTaskMemFree(dir);
        }
    }
    const bool match = !g_real.empty() && _wcsicmp(name, g_real.c_str()) == 0;
    ReleaseSRWLockExclusive(&g_lock);
    return match;
}

bool g_profile;  // the VR profile is on (else only the shader cache is handled)

bool is_shader_cache(const wchar_t* name) {
    if (!name) return false;
    const size_t n = wcslen(name);
    return n >= 14 && _wcsicmp(name + n - 14, L"pipeline.cache") == 0 &&
           (n == 14 || name[n - 15] == L'\\' || name[n - 15] == L'/');
}

HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                             HANDLE tmpl) {
    if (is_shader_cache(name)) {
        if (!(access & GENERIC_WRITE)) {
            static int logged;
            if (logged++ < 3) logf("shader cache: %ls not read (shaders are compiled and patched)", name);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        return o_CreateFileW(path_in(L"pipeline.cache").c_str(), access, share, sa, disp, flags, tmpl);
    }
    if (g_profile && is_real(name)) {
        AcquireSRWLockExclusive(&g_lock);
        if (access & GENERIC_READ) prepare_copy();
        ReleaseSRWLockExclusive(&g_lock);
        logf("vr profile: %s %ls", (access & GENERIC_WRITE) ? "writing" : "reading", g_copy.c_str());
        return o_CreateFileW(g_copy.c_str(), access, share, sa, disp, flags, tmpl);
    }
    return o_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                             HANDLE tmpl) {
    if (name) {
        wchar_t wide[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH) &&
            (is_shader_cache(wide) || (g_profile && is_real(wide))))
            return hk_CreateFileW(wide, access, share, sa, disp, flags, tmpl);
    }
    return o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

}  // namespace

void install_vr_profile() {
    const std::wstring ini = path_in(L"k7vr.ini");
    // The VR controllers now drive the game's own pad bindings (xinput.cpp), so the keyboard
    // profile is opt-in ([probe] vr_profile=1).
    g_profile = GetPrivateProfileIntW(L"probe", L"vr_profile", 0, ini.c_str()) != 0;
    wchar_t source[MAX_PATH];
    GetPrivateProfileStringW(L"probe", L"vr_profile_source", L"", source, MAX_PATH, ini.c_str());
    if (source[0]) g_real = source;
    g_copy = path_in(L"killer7.ini");
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_import(exe, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&hk_CreateFileW),
                 reinterpret_cast<void**>(&o_CreateFileW));
    patch_import(exe, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&hk_CreateFileA),
                 reinterpret_cast<void**>(&o_CreateFileA));
    if (g_profile) logf("vr profile: the game's settings come from %ls", g_copy.c_str());
}

}  // namespace k7
