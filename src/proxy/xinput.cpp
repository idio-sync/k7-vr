// XINPUT1_3.dll exports. Calls go to the system xinput1_3.dll (loaded on first use);
// player 1 gets the scripted pad from control.cpp, and with fake_pad on it always
// reads as connected so scripted input works without a controller plugged in.
#include "common.h"

#include <xinput.h>

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

extern "C" DWORD WINAPI K7XInputGetState(DWORD user, XINPUT_STATE* state) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>("XInputGetState");
    DWORD r = fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && state && k7::fake_pad_enabled()) {
        if (r != ERROR_SUCCESS) {
            ZeroMemory(state, sizeof(*state));
            r = ERROR_SUCCESS;
        }
        k7::pad_apply(state);
    }
    return r;
}

extern "C" DWORD WINAPI K7XInputGetStateEx(DWORD user, XINPUT_STATE* state) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(MAKEINTRESOURCEA(100));
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI K7XInputSetState(DWORD user, XINPUT_VIBRATION* vib) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*)>("XInputSetState");
    DWORD r = fn ? fn(user, vib) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && r != ERROR_SUCCESS && k7::fake_pad_enabled()) r = ERROR_SUCCESS;
    return r;
}

extern "C" DWORD WINAPI K7XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    static auto fn = real_fn<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*)>("XInputGetCapabilities");
    DWORD r = fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (user == 0 && caps && r != ERROR_SUCCESS && k7::fake_pad_enabled()) {
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
