// The VR controllers as the game's gamepad. The game polls XInput player 1 itself (it looks up
// XInputGetStateEx with GetProcAddress, which xinput.cpp answers); pad_state() gives it a pad
// built from the controllers, read through SteamVR's legacy controller state (no action manifest
// needed). Game pad bindings (killer7.ini [Pad], GameCube layout): A accept/walk/fire, B
// decline/turn, R2 aim mode, L2 scan, Y special, X map, Start menu/skip, D-pad menus, left stick
// movement/aim/junction choice, right stick click reload.
//
//   right trigger = A (fire)          left trigger = A (walk)
//   right grip    = R2 (aim mode)     left grip    = L2 (scan)
//   right A       = A (accept)        left X       = X (map)
//   right B       = B (decline/turn)  left Y       = Y (special)
//   right stick   = right stick       left stick   = left stick, and the D-pad past half travel
//   right click   = reload            left click   = Start (menu, skip)
//   both stick clicks for a second = recentre the view
#include "../proxy/common.h"

#include <openvr.h>
#include <xinput.h>

#include <cmath>
#include <mutex>

#include "vr.h"

namespace k7::vr {

namespace {

enum Hand { kLeft, kRight };

struct HandState {
    ::vr::TrackedDeviceIndex_t device = ::vr::k_unTrackedDeviceIndexInvalid;
    int stick_axis = -1, trigger_axis = -1;
    uint64_t buttons = 0;
    float sx = 0, sy = 0, trigger = 0;
    bool axis_big[::vr::k_unControllerStateAxisCount] = {};
};

HandState g_hands[2];
std::mutex g_pad_mx;
XINPUT_GAMEPAD g_pad;
bool g_pad_live;  // controllers are feeding the pad
ULONGLONG g_last_activate;
ULONGLONG g_recenter_since;

uint64_t mask(::vr::EVRButtonId id) { return ::vr::ButtonMaskFromId(id); }

void find_axes(::vr::IVRSystem* sys, HandState& h, Hand which) {
    h.stick_axis = h.trigger_axis = -1;
    for (int i = 0; i < int(::vr::k_unControllerStateAxisCount); ++i) {
        const int32_t type = sys->GetInt32TrackedDeviceProperty(
            h.device, ::vr::ETrackedDeviceProperty(::vr::Prop_Axis0Type_Int32 + i));
        if ((type == ::vr::k_eControllerAxis_Joystick || type == ::vr::k_eControllerAxis_TrackPad) && h.stick_axis < 0)
            h.stick_axis = i;
        if (type == ::vr::k_eControllerAxis_Trigger && h.trigger_axis < 0) h.trigger_axis = i;
    }
    // Some controllers (Steam Frame's own driver) don't describe their axes; SteamVR's legacy
    // layout puts the stick/trackpad on axis 0 and the trigger on axis 1.
    const bool assumed = h.stick_axis < 0 || h.trigger_axis < 0;
    if (h.stick_axis < 0) h.stick_axis = 0;
    if (h.trigger_axis < 0) h.trigger_axis = (h.stick_axis == 1) ? 0 : 1;
    char name[128] = {};
    sys->GetStringTrackedDeviceProperty(h.device, ::vr::Prop_ControllerType_String, name, sizeof(name));
    logf("input: %s controller '%s' (device %u), stick axis %d, trigger axis %d%s", which == kLeft ? "left" : "right",
         name, h.device, h.stick_axis, h.trigger_axis, assumed ? " (assumed)" : "");
}

void poll_hand(::vr::IVRSystem* sys, Hand which) {
    HandState& h = g_hands[which];
    const auto role = which == kLeft ? ::vr::TrackedControllerRole_LeftHand : ::vr::TrackedControllerRole_RightHand;
    const ::vr::TrackedDeviceIndex_t device = sys->GetTrackedDeviceIndexForControllerRole(role);
    if (device != h.device) {
        h = HandState{};
        h.device = device;
        if (device != ::vr::k_unTrackedDeviceIndexInvalid) find_axes(sys, h, which);
    }
    if (device == ::vr::k_unTrackedDeviceIndexInvalid) return;
    ::vr::VRControllerState_t s;
    if (!sys->GetControllerState(device, &s, sizeof(s))) return;
    if (s.ulButtonPressed != h.buttons)
        logf("input: %s buttons %016llx", which == kLeft ? "left" : "right", s.ulButtonPressed);
    // Diagnostics: which axes move (logged when one crosses half travel, a few times per axis).
    for (int i = 0; i < int(::vr::k_unControllerStateAxisCount); ++i) {
        static int logged[2][::vr::k_unControllerStateAxisCount];
        const bool big = fabsf(s.rAxis[i].x) > 0.5f || fabsf(s.rAxis[i].y) > 0.5f;
        if (big && !h.axis_big[i] && logged[which][i]++ < 4)
            logf("input: %s axis %d = %.2f, %.2f", which == kLeft ? "left" : "right", i, s.rAxis[i].x, s.rAxis[i].y);
        h.axis_big[i] = big;
    }
    h.buttons = s.ulButtonPressed;
    h.sx = s.rAxis[h.stick_axis].x;
    h.sy = s.rAxis[h.stick_axis].y;
    h.trigger = s.rAxis[h.trigger_axis].x;
}

SHORT stick(float v) {
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    return SHORT(v * 32767);
}

XINPUT_GAMEPAD build_pad() {
    const HandState& l = g_hands[kLeft];
    const HandState& r = g_hands[kRight];
    auto pressed = [](const HandState& h, ::vr::EVRButtonId id) { return (h.buttons & mask(id)) != 0; };
    XINPUT_GAMEPAD p{};
    if (r.trigger > 0.5f || l.trigger > 0.5f || pressed(r, ::vr::k_EButton_A)) p.wButtons |= XINPUT_GAMEPAD_A;
    if (pressed(r, ::vr::k_EButton_ApplicationMenu)) p.wButtons |= XINPUT_GAMEPAD_B;
    if (pressed(l, ::vr::k_EButton_A)) p.wButtons |= XINPUT_GAMEPAD_X;
    if (pressed(l, ::vr::k_EButton_ApplicationMenu)) p.wButtons |= XINPUT_GAMEPAD_Y;
    if (pressed(r, ::vr::k_EButton_SteamVR_Touchpad)) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (pressed(l, ::vr::k_EButton_SteamVR_Touchpad)) p.wButtons |= XINPUT_GAMEPAD_START;
    if (pressed(r, ::vr::k_EButton_Grip)) p.bRightTrigger = 255;
    if (pressed(l, ::vr::k_EButton_Grip)) p.bLeftTrigger = 255;
    p.sThumbLX = stick(l.sx), p.sThumbLY = stick(l.sy);
    p.sThumbRX = stick(r.sx), p.sThumbRY = stick(r.sy);
    if (l.sy > 0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (l.sy < -0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (l.sx < -0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (l.sx > 0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    return p;
}

}  // namespace

void poll_controllers(::vr::IVRSystem* sys) {
    if (!sys) return;
    poll_hand(sys, kLeft);
    poll_hand(sys, kRight);
    const XINPUT_GAMEPAD pad = build_pad();
    {
        std::lock_guard lock(g_pad_mx);
        // The game ignores input while it thinks its window is inactive; tell it otherwise when
        // something is pressed (at most once a second).
        if ((pad.wButtons & ~g_pad.wButtons) || (pad.bRightTrigger && !g_pad.bRightTrigger)) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_last_activate > 1000) post_activate(), g_last_activate = now;
        }
        g_pad = pad;
        g_pad_live = g_hands[kLeft].device != ::vr::k_unTrackedDeviceIndexInvalid ||
                     g_hands[kRight].device != ::vr::k_unTrackedDeviceIndexInvalid;
    }
    // Both stick clicks held for a second: recentre.
    const uint64_t click = mask(::vr::k_EButton_SteamVR_Touchpad);
    if ((g_hands[kLeft].buttons & click) && (g_hands[kRight].buttons & click)) {
        if (!g_recenter_since) g_recenter_since = GetTickCount64();
        if (GetTickCount64() - g_recenter_since > 1000) {
            recenter();
            g_recenter_since = ~0ull >> 1;  // once per hold
        }
    } else {
        g_recenter_since = 0;
    }
}

bool pad_state(XINPUT_GAMEPAD* out) {
    std::lock_guard lock(g_pad_mx);
    if (!g_pad_live) return false;
    *out = g_pad;
    return true;
}

}  // namespace k7::vr
