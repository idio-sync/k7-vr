// Starts the game the way a Steam launch does, for testing the mod alongside the Steam overlay
// (tools\run.ps1 -SteamOverlay): Steam's environment variables, and Steam's GameOverlayRenderer.dll
// loaded into the game right after it starts. Players start the game from Steam, so the overlay
// is in their game; a plain start of Killer7Win.exe has no overlay.
//   steam_launch.exe <game exe> <working dir> <GameOverlayRenderer.dll>
// Prints "started PID <pid>". The Steam client must be running for the overlay to attach.
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::printf("usage: steam_launch <game exe> <working dir> <GameOverlayRenderer.dll>\n");
        return 2;
    }
    const wchar_t* exe = argv[1];
    const wchar_t* dir = argv[2];
    const wchar_t* overlay = argv[3];
    // Killer7's Steam app id; the overlay asks the Steam client about this game.
    for (const wchar_t* name : {L"SteamGameId", L"SteamAppId", L"SteamOverlayGameId"})
        SetEnvironmentVariableW(name, L"868510");
    SetEnvironmentVariableW(L"SteamClientLaunch", L"1");
    SetEnvironmentVariableW(L"SteamEnv", L"1");

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
        std::printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("started PID %lu\n", pi.dwProcessId);
    // The game's loader maps kernel32 first; then its LoadLibraryW (at the same address in every
    // 32-bit process) loads the overlay, well before the game makes its D3D11 device.
    Sleep(20);
    const size_t bytes = (wcslen(overlay) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE t = nullptr;
    if (remote && WriteProcessMemory(pi.hProcess, remote, overlay, bytes, nullptr)) {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        t = CreateRemoteThread(pi.hProcess, nullptr, 0, load, remote, 0, nullptr);
    }
    if (!t) {
        std::printf("overlay not loaded: %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(t, 15000);
    DWORD module = 0;
    GetExitCodeThread(t, &module);
    std::printf(module ? "overlay loaded\n" : "overlay not loaded (LoadLibrary failed)\n");
    CloseHandle(t);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return module ? 0 : 1;
}
