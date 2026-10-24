#pragma once
// Pose and projection math for the VR view, free of D3D/OpenVR so it can be unit-tested.
//
// Conventions match both OpenVR tracking space and the game's GX eye space: right-handed,
// +x right, +y up, -z forward. Poses are 3x4 row-major [R | t] mapping local to parent.

namespace k7::vr {

struct Pose {
    float m[3][4];  // rows: R row i, then t_i
};

Pose identity();
Pose compose(const Pose& a, const Pose& b);  // a * b (apply b, then a)
Pose inverse(const Pose& p);                 // rigid inverse
Pose translation(float x, float y, float z);
Pose rotation_yaw_pitch(float yaw_rad, float pitch_rad);  // head turned +yaw right, +pitch up

// Keeps only the rotation about +y (and the translation), for a level recentre reference.
Pose yaw_only(const Pose& p);

// Shader head transform for one eye: maps a point in the game camera's eye space (game
// units) to that eye's space (game units). `eye_in_anchor` is the eye's pose relative to the
// anchor (the game camera), in metres; `unit_m` is metres per game unit.
void eye_space_transform(const Pose& eye_in_anchor, float unit_m, float out[12]);

// ---- Aim mode (M4) ----
// The game's world is right-handed, +y up. Its aim camera looks along
// (sin yaw cos pitch, sin pitch, cos yaw cos pitch) (radians; probe 2026-10-24).

// World pose of a camera at `eye` looking at `target` (no roll), in the game's units.
Pose look_at(const float eye[3], const float target[3]);

// The game's aim angles for a world direction.
void aim_angles(const float dir[3], float* yaw, float* pitch);

// Pose `p` with its translation scaled (metres <-> game units).
Pose scaled(const Pose& p, float s);

// Shader 3x4 rows of a pose (no unit conversion).
void pose_rows(const Pose& p, float out[12]);

// Clip-space x and y rows for an off-axis frustum given as tangents of the half-angles in
// y-up convention (left < 0 < right, bottom < 0 < top), for the game's projection layout where
// w = -z (camera looks down -z). out_x/out_y are dotted with (x, y, z, 1).
void projection_rows(float left, float right, float bottom, float top, float out_x[4], float out_y[4]);

// Pixel rectangle {left, top, right, bottom} of a WxH eye image that covers the game camera's
// frustum (half-angle tangents tan_x, tan_y) as seen by an eye with shader transform `head`
// (eye_space_transform) and projection rows px/py. Directions only (the frustum's apex is the
// game camera; the eye offset is ignored). Empty when the frustum is entirely out of view.
void frustum_scissor(const float head[12], const float px[4], const float py[4], float tan_x, float tan_y, int width,
                     int height, int out[4]);

}  // namespace k7::vr
