#pragma once
// The VR view: where each eye is and how it projects, from OpenVR or a simulated headset.
//
// k7vr.ini [vr]:
//   mode = off | sim | openvr   (default off; openvr starts SteamVR, so it is never the default)
//   unit_m = 0.25               metres per game unit (world scale)
//   ipd_m = 0.064               simulated eye separation (OpenVR uses the headset's)
//   sim_fov = 100               simulated vertical FOV, degrees
//   fov_inflate = 150           FOV the game culls with while stereo is on (0 = off)
#include <d3d11.h>

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

// Called once per frame after the game drew it: hands both eye images to the compositor.
void submit(ID3D11Texture2D* left, ID3D11Texture2D* right);

void recenter();
void set_mode(Mode m);  // Off <-> Sim from the command file; OpenVR only from the ini
void set_sim_view(float yaw_deg, float pitch_deg);
void set_ipd(float metres);
void set_unit(float metres_per_unit);

}  // namespace k7::vr
