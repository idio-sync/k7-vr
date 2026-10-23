// Logs the files the game opens (k7vr.ini [probe] log_files=1, default on), to see what a
// scene is loading or waiting on.
#include "common.h"

namespace k7 {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t o_CreateFileW;
static CreateFileA_t o_CreateFileA;

static HANDLE WINAPI hk_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    logf("open %ls%s", name ? name : L"(null)", h == INVALID_HANDLE_VALUE ? " FAILED" : "");
    return h;
}

static HANDLE WINAPI hk_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                    DWORD flags, HANDLE tmpl) {
    HANDLE h = o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    logf("open %s%s", name ? name : "(null)", h == INVALID_HANDLE_VALUE ? " FAILED" : "");
    return h;
}

void install_file_hooks() {
    if (!GetPrivateProfileIntW(L"probe", L"log_files", 1, path_in(L"k7vr.ini").c_str())) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    patch_import(exe, "KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&hk_CreateFileW),
                 reinterpret_cast<void**>(&o_CreateFileW));
    patch_import(exe, "KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&hk_CreateFileA),
                 reinterpret_cast<void**>(&o_CreateFileA));
}

}  // namespace k7
