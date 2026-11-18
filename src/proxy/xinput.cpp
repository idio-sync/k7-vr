// XINPUT1_3.dll exports. Calls go to the system xinput1_3.dll (loaded on first use). Player 1
// is the mod's pad when the VR controllers are on (vr::pad_state) or fake_pad is set (tests),
// with the scripted pad from control.cpp ORed in. The game polls through XInputGetStateEx
// (ordinal 100), which it looks up in the *system* xinput1_3.dll with GetProcAddress; the
// GetProcAddress hook below hands it ours instead.
#include "common.h"

#include <xinput.h>

#include "../vr/vr.h"

#pragma comment(linker, "/EXPORT:XInputGetState=_K7XInputGetState@8,@2")
#pragma comment(linker, "/EXPORT:XInputSetState=_K7XInputSetState@8,@3")
#pragma comment(linker, "/EXPORT:XInputGetCapabilities=_K7XInputGetCapabilities@12,@4")
#pragma comment(linker, "/EXPORT:XInputEnable=_K7XInputEnable@4,@5")
#pragma comment(linker, "/EXPORT:XInputGetDSoundAudioDeviceGuids=_K7XInputGetDSoundAudioDeviceGuids@12,@6")
#pragma comment(linker, "/EXPORT:XInputGetBatteryInformation=_K7XInputGetBatteryInformation@12,@7")
#pragma comment(linker, "/EXPORT:XInputGetKeystroke=_K7XInputGetKeystroke@12,@8")
#pragma comment(linker, "/EXPORT:K7XInputGetStateEx=_K7XInputGetStateEx@8,@100,NONAME")

static FARPROC real(LPCSTR name) {
    static HMODULE dll = [] {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\xinput1_3.dll");
        HMODULE m = LoadLibraryW(path);
        k7::logf("real xinput1_3: %p", m);
        return m;
    }();
    return dll ? GetProcAddress(dll, name) : nullptr;
}

template <typename T>
static T real_fn(LPCSTR name) {
    return reinterpret_cast<T>(real(name));
}

// The game only polls once it has seen a pad (hooks_input.cpp adds a virtual one); log that.
static void note_call(const char* what, DWORD user) {
    static int logged;
    if (logged++ < 6) k7::logf("pad: game calls %s(%u)", what, user);
}

// Player 1's state from the mod: the VR controllers and/or the scripted pad. False: not ours.
static bool mod_pad(XINPUT_STATE* state) {
    XINPUT_GAMEPAD vr_pad{};
    const bool vr_on = k7::vr::pad_state(&vr_pad);
    if (!vr_on && !k7::fake_pad_enabled()) return false;
    static XINPUT_GAMEPAD last;
    static DWORD packet;
    state->Gamepad = vr_pad;
    k7::pad_apply(state);
    if (memcmp(&state->Gamepad, &last, sizeof(last)) != 0) last = state->Gamepad, ++packet;
    state->dwPacketNumber = packet;
    return true;
}

static bool pad_on() {
    XINPUT_GAMEPAD unused;
    return k7::fake_pad_enabled() || k7::vr::pad_state(&unused);
}

extern "C" DWORD WINAPI K7XInputGetState(DWORD user, XINPUT_STATE* state) {
    note_call("XInputGetState", user);
    if (user == 0 && state && mod_pad(state)) return ERROR_SUCCESS;
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>("XInputGetState");
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI K7XInputGetStateEx(DWORD user, XINPUT_STATE* state) {
    note_call("XInputGetStateEx", user);
    if (user == 0 && state && mod_pad(state)) return ERROR_SUCCESS;
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(MAKEINTRESOURCEA(100));
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI K7XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
    static bool first = true;
    if (first) first = false, k7::logf("pad: game calls XInputSetState(%u)", user);
    if (user == 0 && vib) {  // the game's rumble: to the VR controllers too
        k7::vr::set_rumble(vib->wLeftMotorSpeed, vib->wRightMotorSpeed);
        static XINPUT_VIBRATION last;
        static int logged;
        if ((vib->wLeftMotorSpeed != last.wLeftMotorSpeed || vib->wRightMotorSpeed != last.wRightMotorSpeed) &&
            logged++ < 60)
            k7::logf("pad: rumble %u %u", vib->wLeftMotorSpeed, vib->wRightMotorSpeed);
        last = *vib;
    }
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*)>("XInputSetState");
    DWORD r = fn ? fn(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && r != ERROR_SUCCESS && pad_on()) r = ERROR_SUCCESS;
    return r;
}

extern "C" DWORD WINAPI K7XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    note_call("XInputGetCapabilities", user);
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*)>("XInputGetCapabilities");
    DWORD r = fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && caps && r != ERROR_SUCCESS && pad_on()) {
        ZeroMemory(caps, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xF3FF;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = -64;
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 255;
        r = ERROR_SUCCESS;
    }
    return r;
}

extern "C" void WINAPI K7XInputEnable(BOOL enable) {
    static auto fn = real_fn<void(WINAPI*)(BOOL)>("XInputEnable");
    if (fn) fn(enable);
}

extern "C" DWORD WINAPI K7XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>("XInputGetDSoundAudioDeviceGuids");
    return fn ? fn(user, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI K7XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION* info) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*)>("XInputGetBatteryInformation");
    return fn ? fn(user, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI K7XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE keystroke) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE)>("XInputGetKeystroke");
    return fn ? fn(user, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

namespace k7 {

using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
static GetProcAddress_t o_GetProcAddress;

static FARPROC WINAPI hk_GetProcAddress(HMODULE module, LPCSTR name) {
    const bool state_ex = IS_INTRESOURCE(name) && LOWORD(reinterpret_cast<UINT_PTR>(name)) == 100;
    const bool set_state = !IS_INTRESOURCE(name) && name && strcmp(name, "XInputSetState") == 0;
    if (state_ex || set_state) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        _wcslwr_s(path);
        if (wcsstr(path, L"xinput")) {
            logf("pad: game looks up %s in %ls: given the mod's", state_ex ? "XInputGetStateEx" : "XInputSetState", path);
            return state_ex ? reinterpret_cast<FARPROC>(&K7XInputGetStateEx) : reinterpret_cast<FARPROC>(&K7XInputSetState);
        }
    }
    return o_GetProcAddress(module, name);
}

void install_xinput_hook() {
    patch_import(GetModuleHandleW(nullptr), "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&hk_GetProcAddress),
                 reinterpret_cast<void**>(&o_GetProcAddress));
}

}  // namespace k7
