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

Pose look_at(const float eye[3], const float target[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (len < 1e-6f) return translation(eye[0], eye[1], eye[2]);
    for (float& v : f) v /= len;
    // right = f x up(0,1,0), up = right x f; columns of R are right, up, back (-f).
    float r[3] = {-f[2], 0, f[0]};
    len = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (len < 1e-6f) r[0] = 1, r[2] = 0, len = 1;  // straight up/down: any heading
    r[0] /= len, r[2] /= len;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    Pose p = translation(eye[0], eye[1], eye[2]);
    for (int i = 0; i < 3; ++i) p.m[i][0] = r[i], p.m[i][1] = u[i], p.m[i][2] = -f[i];
    return p;
}

void aim_angles(const float d[3], float* yaw, float* pitch) {
    const float h = sqrtf(d[0] * d[0] + d[2] * d[2]);
    *yaw = atan2f(d[0], d[2]);
    *pitch = atan2f(d[1], h);
}

Pose scaled(const Pose& p, float s) {
    Pose r = p;
    for (int i = 0; i < 3; ++i) r.m[i][3] *= s;
    return r;
}

void pose_rows(const Pose& p, float out[12]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = p.m[i][j];
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

void frustum_scissor(const float head[12], const float px[4], const float py[4], float tan_x, float tan_y, int width,
                     int height, int out[4]) {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    bool unbounded = false;
    for (int c = 0; c < 4; ++c) {
        const float g[3] = {(c & 1 ? 1 : -1) * tan_x, (c & 2 ? 1 : -1) * tan_y, -1};
        float e[3];
        for (int i = 0; i < 3; ++i) e[i] = head[i * 4] * g[0] + head[i * 4 + 1] * g[1] + head[i * 4 + 2] * g[2];
        if (e[2] > -1e-4f) {  // corner at or behind the eye plane: the region reaches the image edge
            unbounded = true;
            continue;
        }
        const float w = -e[2];
        const float nx = (px[0] * e[0] + px[1] * e[1] + px[2] * e[2]) / w;
        const float ny = (py[0] * e[0] + py[1] * e[1] + py[2] * e[2]) / w;
        min_x = fminf(min_x, nx), max_x = fmaxf(max_x, nx);
        min_y = fminf(min_y, ny), max_y = fmaxf(max_y, ny);
    }
    if (unbounded) {
        min_x = min_y = -1;
        max_x = max_y = 1;
    }
    auto clampi = [](float v, int hi) { return v < 0 ? 0 : (v > float(hi) ? hi : int(v + 0.5f)); };
    out[0] = clampi((min_x * 0.5f + 0.5f) * width, width);
    out[2] = clampi((max_x * 0.5f + 0.5f) * width, width);
    out[1] = clampi((0.5f - max_y * 0.5f) * height, height);
    out[3] = clampi((0.5f - min_y * 0.5f) * height, height);
    if (out[2] < out[0]) out[2] = out[0];
    if (out[3] < out[1]) out[3] = out[1];
}

}  // namespace k7::vr
