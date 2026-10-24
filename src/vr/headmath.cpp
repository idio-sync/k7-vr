#include "headmath.h"

#include <cmath>

namespace k7::vr {

Pose identity() { return translation(0, 0, 0); }

Pose translation(float x, float y, float z) {
    Pose p{};
    p.m[0][0] = p.m[1][1] = p.m[2][2] = 1;
    p.m[0][3] = x;
    p.m[1][3] = y;
    p.m[2][3] = z;
    return p;
}

Pose compose(const Pose& a, const Pose& b) {
    Pose r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        r.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
    }
    return r;
}

Pose inverse(const Pose& p) {
    Pose r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = p.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * p.m[0][3] + r.m[i][1] * p.m[1][3] + r.m[i][2] * p.m[2][3]);
    return r;
}

Pose rotation_yaw_pitch(float yaw, float pitch) {
    // Head rotation: yaw about +y (turning right is negative rotation about +y), then pitch about +x.
    const float cy = cosf(-yaw), sy = sinf(-yaw), cp = cosf(pitch), sp = sinf(pitch);
    Pose ry{}, rx{};
    ry.m[0][0] = cy, ry.m[0][2] = sy, ry.m[1][1] = 1, ry.m[2][0] = -sy, ry.m[2][2] = cy;
    rx.m[0][0] = 1, rx.m[1][1] = cp, rx.m[1][2] = -sp, rx.m[2][1] = sp, rx.m[2][2] = cp;
    return compose(ry, rx);
}

Pose yaw_only(const Pose& p) {
    // Heading of the -z axis projected onto the horizontal plane.
    const float fx = -p.m[0][2], fz = -p.m[2][2];
    const float len = sqrtf(fx * fx + fz * fz);
    Pose r = translation(p.m[0][3], p.m[1][3], p.m[2][3]);
    if (len < 1e-6f) return r;
    const float yaw = atan2f(fx, -fz);  // 0 = looking down -z, positive = turned right
    const Pose rot = rotation_yaw_pitch(yaw, 0);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = rot.m[i][j];
    return r;
}

void eye_space_transform(const Pose& eye_in_anchor, float unit_m, float out[12]) {
    const Pose inv = inverse(eye_in_anchor);  // anchor -> eye, metres
    for (int i = 0; i < 3; ++i) {
        out[i * 4 + 0] = inv.m[i][0];
        out[i * 4 + 1] = inv.m[i][1];
        out[i * 4 + 2] = inv.m[i][2];
        out[i * 4 + 3] = inv.m[i][3] / unit_m;  // rotation is unitless; translation into game units
    }
}

void projection_rows(float l, float r, float b, float t, float out_x[4], float out_y[4]) {
    // x_ndc = (x/(-z) - (l+r)/2) / ((r-l)/2)  with w = -z  =>  x_clip = 2/(r-l) x + (r+l)/(r-l) z
    out_x[0] = 2 / (r - l);
    out_x[1] = 0;
    out_x[2] = (r + l) / (r - l);
    out_x[3] = 0;
    out_y[0] = 0;
    out_y[1] = 2 / (t - b);
    out_y[2] = (t + b) / (t - b);
    out_y[3] = 0;
}

}  // namespace k7::vr
