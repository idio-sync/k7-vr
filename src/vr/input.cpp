// M3: VR controllers drive the game through the injected raw-input keys that already work (the
// game reads keyboard/mouse via raw input; no pad is attached, so it never polls XInput).
// SteamVR's legacy controller state is used so no action manifest is needed (setting one turns
// legacy input off for the app); stick and trigger axes are found from each controller's axis types.
//
// Default layout (test profile key bindings; see docs/playtests):
//   right trigger = fire (J)            right grip (hold) = aim (Space)
//   right A = accept (Space)            right B = skip / decline (Esc)
//   right stick = aim/menu arrows       right stick click = reload (R)
//   left trigger (hold) = walk (W)      left grip (hold) = scan (Left Shift)
//   left stick: up W (walk/menu up), down S (turn around/menu down), left A, right D
//   left X = special (Q)                left Y = map (M)
//   left stick click = menu (Tab)       both stick clicks for 1 s = recentre the view
#include <openvr.h>

#include <cmath>

#include "../proxy/common.h"
#include "vr.h"

namespace k7::vr {

namespace {

enum Hand { kLeft, kRight };

struct HandState {
    ::vr::TrackedDeviceIndex_t device = ::vr::k_unTrackedDeviceIndexInvalid;
    int stick_axis = -1, trigger_axis = -1;
    uint64_t buttons = 0;
    bool trigger = false;
    int stick_dir[4] = {};  // up, down, left, right: held
};

HandState g_hands[2];
int g_key_holds[256];  // how many inputs hold each virtual key
ULONGLONG g_last_activate;
ULONGLONG g_recenter_since;

void key(UINT vk, bool down) {
    int& n = g_key_holds[vk & 0xFF];
    if (down) {
        if (n++ == 0) {
            const ULONGLONG now = GetTickCount64();
            if (now - g_last_activate > 1000) {  // the game ignores input unless it thinks it's active
                post_activate();
                g_last_activate = now;
            }
            post_key(vk, true);
        }
    } else if (n > 0 && --n == 0) {
        post_key(vk, false);
    }
}

void edge(bool was, bool now, UINT vk) {
    if (was != now) key(vk, now);
}

uint64_t mask(::vr::EVRButtonId id) { return ::vr::ButtonMaskFromId(id); }

void find_axes(::vr::IVRSystem* sys, HandState& h) {
    h.stick_axis = h.trigger_axis = -1;
    for (int i = 0; i < int(::vr::k_unControllerStateAxisCount); ++i) {
        const int32_t type = sys->GetInt32TrackedDeviceProperty(
            h.device, ::vr::ETrackedDeviceProperty(::vr::Prop_Axis0Type_Int32 + i));
        if ((type == ::vr::k_eControllerAxis_Joystick || type == ::vr::k_eControllerAxis_TrackPad) && h.stick_axis < 0)
            h.stick_axis = i;
        if (type == ::vr::k_eControllerAxis_Trigger && h.trigger_axis < 0) h.trigger_axis = i;
    }
    char name[128] = {};
    sys->GetStringTrackedDeviceProperty(h.device, ::vr::Prop_ControllerType_String, name, sizeof(name));
    logf("input: %s controller '%s' (device %u), stick axis %d, trigger axis %d", &h == &g_hands[kLeft] ? "left" : "right",
         name, h.device, h.stick_axis, h.trigger_axis);
}

void poll_hand(::vr::IVRSystem* sys, Hand which) {
    HandState& h = g_hands[which];
    const auto role = which == kLeft ? ::vr::TrackedControllerRole_LeftHand : ::vr::TrackedControllerRole_RightHand;
    const ::vr::TrackedDeviceIndex_t device = sys->GetTrackedDeviceIndexForControllerRole(role);
    if (device != h.device) {
        h = HandState{};
        h.device = device;
        if (device != ::vr::k_unTrackedDeviceIndexInvalid) find_axes(sys, h);
    }
    if (device == ::vr::k_unTrackedDeviceIndexInvalid) return;
    ::vr::VRControllerState_t s;
    if (!sys->GetControllerState(device, &s, sizeof(s))) return;

    const uint64_t was = h.buttons, now = s.ulButtonPressed;
    if (was != now) logf("input: %s buttons %016llx", which == kLeft ? "left" : "right", now);
    auto pressed = [&](::vr::EVRButtonId id) { return (now & mask(id)) != 0; };
    auto pressed_before = [&](::vr::EVRButtonId id) { return (was & mask(id)) != 0; };

    const float trig = h.trigger_axis >= 0 ? s.rAxis[h.trigger_axis].x : 0;
    const bool trigger = h.trigger ? trig > 0.4f : trig > 0.6f;  // hysteresis
    float sx = 0, sy = 0;
    if (h.stick_axis >= 0) sx = s.rAxis[h.stick_axis].x, sy = s.rAxis[h.stick_axis].y;
    const bool dir[4] = {sy > 0.6f, sy < -0.6f, sx < -0.6f, sx > 0.6f};

    if (which == kRight) {
        edge(h.trigger, trigger, 'J');
        edge(pressed_before(::vr::k_EButton_Grip), pressed(::vr::k_EButton_Grip), VK_SPACE);
        edge(pressed_before(::vr::k_EButton_A), pressed(::vr::k_EButton_A), VK_SPACE);
        edge(pressed_before(::vr::k_EButton_ApplicationMenu), pressed(::vr::k_EButton_ApplicationMenu), VK_ESCAPE);
        edge(pressed_before(::vr::k_EButton_SteamVR_Touchpad), pressed(::vr::k_EButton_SteamVR_Touchpad), 'R');
        const UINT arrows[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
        for (int i = 0; i < 4; ++i) edge(h.stick_dir[i] != 0, dir[i], arrows[i]);
    } else {
        edge(h.trigger, trigger, 'W');
        edge(pressed_before(::vr::k_EButton_Grip), pressed(::vr::k_EButton_Grip), VK_LSHIFT);
        edge(pressed_before(::vr::k_EButton_A), pressed(::vr::k_EButton_A), 'Q');
        edge(pressed_before(::vr::k_EButton_ApplicationMenu), pressed(::vr::k_EButton_ApplicationMenu), 'M');
        edge(pressed_before(::vr::k_EButton_SteamVR_Touchpad), pressed(::vr::k_EButton_SteamVR_Touchpad), VK_TAB);
        const UINT wasd[4] = {'W', 'S', 'A', 'D'};
        for (int i = 0; i < 4; ++i) edge(h.stick_dir[i] != 0, dir[i], wasd[i]);
    }
    h.buttons = now;
    h.trigger = trigger;
    for (int i = 0; i < 4; ++i) h.stick_dir[i] = dir[i];
}

}  // namespace

void poll_controllers(::vr::IVRSystem* sys) {
    if (!sys) return;
    poll_hand(sys, kLeft);
    poll_hand(sys, kRight);
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

}  // namespace k7::vr
