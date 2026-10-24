// Scripted keyboard input. The game reads the keyboard through raw input (WM_INPUT +
// GetRawInputData). post_key() posts WM_INPUT with a marker handle; the GetRawInputData
// import hook answers for marker handles with a made-up RAWKEYBOARD and passes real
// handles through. Posted messages arrive whether or not the window has focus.
#include "common.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace k7 {

static std::atomic<HWND> g_hwnd;
static HANDLE g_keyboard;  // a real keyboard device handle, in case the game filters by device

static std::mutex g_mx;
static std::unordered_map<UINT_PTR, RAWINPUT> g_fake;
static UINT_PTR g_next = 1;
constexpr UINT_PTR kMarker = 0x4B370000;  // "K7"; real HRAWINPUTs never look like this in practice

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static GetRawInputData_t o_GetRawInputData;

static UINT WINAPI hk_GetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT header_size) {
    UINT_PTR h = reinterpret_cast<UINT_PTR>(handle);
    if ((h & 0xFFFF0000) == kMarker) {
        std::lock_guard lock(g_mx);
        auto it = g_fake.find(h);
        if (it == g_fake.end() || !size) return UINT(-1);
        const RAWINPUT& ri = it->second;
        UINT needed = command == RID_HEADER ? sizeof(RAWINPUTHEADER) : ri.header.dwSize;
        if (!data) {
            *size = needed;
            return 0;
        }
        if (*size < needed) return UINT(-1);
        memcpy(data, &ri, needed);
        if (command == RID_INPUT) g_fake.erase(it);
        return needed;
    }
    return o_GetRawInputData(handle, command, data, size, header_size);
}

void set_game_window(HWND hwnd) {
    if (g_hwnd.exchange(hwnd) == hwnd) return;
    logf("game window %p", hwnd);
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    if (n) {
        auto list = new RAWINPUTDEVICELIST[n];
        n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
        for (UINT i = 0; i < n && n != UINT(-1); ++i)
            if (list[i].dwType == RIM_TYPEKEYBOARD) {
                g_keyboard = list[i].hDevice;
                break;
            }
        delete[] list;
    }
    if (GetPrivateProfileIntW(L"probe", L"auto_activate", 1, path_in(L"k7vr.ini").c_str())) post_activate();
}

void post_key(UINT vk, bool down) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) {
        logf("post_key: no game window yet");
        return;
    }
    RAWINPUT ri{};
    ri.header.dwType = RIM_TYPEKEYBOARD;
    ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);
    ri.header.hDevice = g_keyboard;
    ri.data.keyboard.MakeCode = USHORT(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    ri.data.keyboard.Flags = down ? RI_KEY_MAKE : RI_KEY_BREAK;
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) ri.data.keyboard.Flags |= RI_KEY_E0;
    ri.data.keyboard.VKey = USHORT(vk);
    ri.data.keyboard.Message = down ? WM_KEYDOWN : WM_KEYUP;
    UINT_PTR h;
    {
        std::lock_guard lock(g_mx);
        h = kMarker | (g_next++ & 0xFFFF);
        g_fake[h] = ri;
    }
    BOOL ok = PostMessageW(hwnd, WM_INPUT, RIM_INPUT, LPARAM(h));
    logf("post_key vk=%u %s -> %d (window %p, foreground %p)", vk, down ? "down" : "up", ok, hwnd,
         GetForegroundWindow());
}

static HANDLE g_mouse;

// Mouse buttons and motion through the same WM_INPUT path. flags are RI_MOUSE_* button flags.
void post_mouse(USHORT flags, LONG dx, LONG dy) {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) return;
    if (!g_mouse) {
        UINT n = 0;
        GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
        if (n) {
            auto list = new RAWINPUTDEVICELIST[n];
            n = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
            for (UINT i = 0; i < n && n != UINT(-1); ++i)
                if (list[i].dwType == RIM_TYPEMOUSE) {
                    g_mouse = list[i].hDevice;
                    break;
                }
            delete[] list;
        }
    }
    RAWINPUT ri{};
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.header.dwSize = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
    ri.header.hDevice = g_mouse;
    ri.data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
    ri.data.mouse.usButtonFlags = flags;
    ri.data.mouse.lLastX = dx;
    ri.data.mouse.lLastY = dy;
    UINT_PTR h;
    {
        std::lock_guard lock(g_mx);
        h = kMarker | (g_next++ & 0xFFFF);
        g_fake[h] = ri;
    }
    PostMessageW(hwnd, WM_INPUT, RIM_INPUT, LPARAM(h));

    // Buttons also as window messages, in case the game reads those instead of raw input.
    const LPARAM at = MAKELPARAM(640, 360);
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) PostMessageW(hwnd, WM_LBUTTONUP, 0, at);
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) PostMessageW(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, at);
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) PostMessageW(hwnd, WM_RBUTTONUP, 0, at);
}

// Tells the game it is the active window, so it acts on scripted input while in the background.
void post_activate() {
    HWND hwnd = g_hwnd.load();
    if (!hwnd) return;
    PostMessageW(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(hwnd, WM_SETFOCUS, 0, 0);
    logf("posted activation to %p", hwnd);
}

// The game hovers menu items under the real mouse cursor, so the probe reports its own cursor:
// parked far off the window unless a `mouse x y` command places it (window client coordinates).
static std::atomic<bool> g_cursor_set;
static std::atomic<LONG> g_cursor_x, g_cursor_y;

void set_fake_cursor(int x, int y) {
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_set = true;
}

// Defined in client coordinates of the window's *current* position, so the game's own
// ScreenToClient gets back exactly what was set, minimized (at -32000,-32000) or not.
static BOOL WINAPI hk_GetCursorPos(POINT* p) {
    if (!p) return FALSE;
    POINT pt{-1000, -1000};  // parked above-left of the window
    if (g_cursor_set) pt = {g_cursor_x, g_cursor_y};
    if (HWND hwnd = g_hwnd.load()) ClientToScreen(hwnd, &pt);
    *p = pt;
    return TRUE;
}

// A minimized window has an empty client rect; report the restored size so the game's
// cursor-to-screen mapping (and anything sized from the client area) keeps working.
bool restored_client_size(HWND hwnd, LONG* w, LONG* h) {
    if (!hwnd || hwnd != g_hwnd.load() || !IsIconic(hwnd)) return false;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectEx(&frame, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)));
    *w = (wp.rcNormalPosition.right - wp.rcNormalPosition.left) - (frame.right - frame.left);
    *h = (wp.rcNormalPosition.bottom - wp.rcNormalPosition.top) - (frame.bottom - frame.top);
    return *w > 0 && *h > 0;
}

using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
static GetClientRect_t o_GetClientRect;
static BOOL WINAPI hk_GetClientRect(HWND hwnd, LPRECT r) {
    LONG w, h;
    if (r && restored_client_size(hwnd, &w, &h)) {
        *r = {0, 0, w, h};
        return TRUE;
    }
    return o_GetClientRect(hwnd, r);
}

// While probing, the game must not grab the desktop: no cursor clipping or warping, no focus stealing.
static BOOL WINAPI hk_ClipCursor(const RECT*) { return TRUE; }
static BOOL WINAPI hk_SetCursorPos(int, int) { return TRUE; }
static BOOL WINAPI hk_SetForegroundWindow(HWND) { return TRUE; }
static HWND WINAPI hk_SetFocus(HWND hwnd) { return hwnd; }

// minimized=1: every time the game shows its window, it is shown minimized and inactive instead.
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
static ShowWindow_t o_ShowWindow;
static BOOL WINAPI hk_ShowWindow(HWND hwnd, int cmd) {
    if (cmd != SW_HIDE && !GetParent(hwnd)) cmd = SW_SHOWMINNOACTIVE;
    return o_ShowWindow(hwnd, cmd);
}

void install_input_hook() {
    HMODULE exe = GetModuleHandleW(nullptr);
    void* unused;
    patch_import(exe, "USER32.dll", "GetRawInputData", reinterpret_cast<void*>(&hk_GetRawInputData),
                 reinterpret_cast<void**>(&o_GetRawInputData));
    if (GetPrivateProfileIntW(L"probe", L"leave_desktop_alone", 0, path_in(L"k7vr.ini").c_str())) {
        patch_import(exe, "USER32.dll", "ClipCursor", reinterpret_cast<void*>(&hk_ClipCursor), &unused);
        patch_import(exe, "USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&hk_SetCursorPos), &unused);
        patch_import(exe, "USER32.dll", "SetForegroundWindow", reinterpret_cast<void*>(&hk_SetForegroundWindow),
                     &unused);
        patch_import(exe, "USER32.dll", "SetFocus", reinterpret_cast<void*>(&hk_SetFocus), &unused);
        patch_import(exe, "USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&hk_GetCursorPos), &unused);
    }
    if (GetPrivateProfileIntW(L"probe", L"minimized", 0, path_in(L"k7vr.ini").c_str())) {
        patch_import(exe, "USER32.dll", "ShowWindow", reinterpret_cast<void*>(&hk_ShowWindow),
                     reinterpret_cast<void**>(&o_ShowWindow));
        patch_import(exe, "USER32.dll", "GetClientRect", reinterpret_cast<void*>(&hk_GetClientRect),
                     reinterpret_cast<void**>(&o_GetClientRect));
    }
}

}  // namespace k7
