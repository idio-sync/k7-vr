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

// Clip-space x and y rows for an off-axis frustum given as tangents of the half-angles in
// y-up convention (left < 0 < right, bottom < 0 < top), for the game's projection layout where
// w = -z (camera looks down -z). out_x/out_y are dotted with (x, y, z, 1).
void projection_rows(float left, float right, float bottom, float top, float out_x[4], float out_y[4]);

}  // namespace k7::vr
