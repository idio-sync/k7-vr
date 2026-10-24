#pragma once
// The VR view: where each eye is and how it projects, from OpenVR or a simulated headset.
//
// k7vr.ini [vr]:
//   mode = off | sim | openvr   (default off; openvr starts SteamVR, so it is never the default)
//   unit_m = 0.25               metres per game unit (world scale)
//   ipd_m = 0.064               simulated eye separation (OpenVR uses the headset's)
//   sim_fov = 100               simulated vertical FOV, degrees
//   fov_inflate = 150           FOV the game culls with while stereo is on (0 = off)
//   eye_res = auto              size of the game's 4:3 render targets while stereo is on:
//                               auto (OpenVR: the headset's recommended size x eye_scale;
//                               sim: unchanged), WxH, or game (unchanged)
//   eye_scale = 1.0             multiplier on the recommended size for eye_res=auto
//   destripe = 1                blend the game's alternating 1-pixel columns in the eye and screen images
//   hand_pitch = 0              degrees the aim direction is tilted up from the controller's -z axis
//   hand_offset = 0,0,0         grip point in the controller's frame, metres
#include <d3d11.h>

#include "headmath.h"

namespace k7::vr {

// Matches the K7Stereo constant buffer the vertex shader patch declares (slot b12).
struct EyeConsts {
    float head[12];   // 3x4: game camera eye space -> this eye's space, game units
    float px[4];      // clip x row (used when flags[0] != 0)
    float py[4];      // clip y row
    float flags[4];   // [0] = replace the game's x/y projection rows for perspective draws
};

enum class Mode { Off, Sim, OpenVR };

void init(ID3D11Device* device, ID3D11DeviceContext* context);
Mode mode();
bool stereo();  // drawing two eyes (Sim or OpenVR)

// Called once per frame before the game draws: waits for poses (OpenVR) and fills both eyes.
// game_xy_ratio = the game projection's y scale / x scale (its aspect), for sim and flat modes.
void begin_frame(float game_xy_ratio, EyeConsts eyes[2]);

// The game camera's own view: identity head and the game's intended projection (undoing FOV
// inflation). Used for the game-view pass that feeds the game's CPU readbacks.
void game_view(float game_xy_ratio, EyeConsts* out);

// Called once per frame after the game drew it. 3D frames: both eye images plus the HUD
// (transparent, on a world-locked panel). 2D frames (menus, FMVs, loading): eyes null (black)
// and `screen` is the game's mono image, shown on a larger opaque virtual screen.
void present(ID3D11Texture2D* left, ID3D11Texture2D* right, ID3D11Texture2D* hud, ID3D11Texture2D* screen);

// What present() sent this frame (0 left, 1 right, 2 screen; sim mode prepares them too), or null.
ID3D11Texture2D* submitted(int which);

// The size the game's 4:3 render targets should be created at, or false to leave them alone.
bool eye_resolution(unsigned* width, unsigned* height);

// Aspect (width/height) of the picture inside HUD and screen textures, for the panels.
void set_content_aspect(float aspect);

// Aim mode (M4). The right controller's pose relative to the seated reference (metres, the
// anchor's frame), with [vr] hand_pitch / hand_offset applied; false when it isn't tracked.
// Sim mode: a scripted hand (set_sim_hand).
bool hand_pose(Pose* out);
void set_sim_hand(float yaw_deg, float pitch_deg);
// Rebuilds this frame's head transforms for a game camera at `cam_in_anchor` (metres; rotation
// and offset from the anchor), so the world stays put while the game camera turns.
void relock(const Pose& cam_in_anchor, EyeConsts eyes[2]);
float unit();

void recenter();
void set_mode(Mode m);  // Off <-> Sim from the command file; OpenVR only from the ini
void set_sim_view(float yaw_deg, float pitch_deg);
void set_ipd(float metres);
void set_unit(float metres_per_unit);

}  // namespace k7::vr
