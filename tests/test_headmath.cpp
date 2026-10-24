// Unit tests for src/vr/headmath (no game, no D3D, no OpenVR). Run: ctest --test-dir out
#include <cmath>
#include <cstdio>

#include "../src/vr/headmath.h"

using namespace k7::vr;

static int g_failures;

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        const double va = (a), vb = (b);                                                               \
        if (!(std::fabs(va - vb) <= (tol))) {                                                          \
            std::printf("%s:%d: %s = %g, expected %s = %g\n", __FILE__, __LINE__, #a, va, #b, vb); \
            ++g_failures;                                                                              \
        }                                                                                              \
    } while (0)

static void apply(const float m[12], const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3];
}

static void apply_pose(const Pose& m, const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i][0] * p[0] + m.m[i][1] * p[1] + m.m[i][2] * p[2] + m.m[i][3];
}

static void test_inverse_compose() {
    Pose a = compose(translation(1, 2, 3), rotation_yaw_pitch(0.7f, -0.3f));
    Pose i = compose(a, inverse(a));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) CHECK_NEAR(i.m[r][c], (r == c) ? 1.0 : 0.0, 1e-5);
}

static void test_yaw_turns_right() {
    // Turning the head right by 90 degrees: forward (-z) becomes +x.
    Pose h = rotation_yaw_pitch(1.5707963f, 0);
    const float fwd[3] = {0, 0, -1};
    float out[3];
    apply_pose(h, fwd, out);
    CHECK_NEAR(out[0], 1, 1e-5);
    CHECK_NEAR(out[2], 0, 1e-5);
    // Pitching up: forward gains +y.
    Pose up = rotation_yaw_pitch(0, 0.5f);
    apply_pose(up, fwd, out);
    CHECK_NEAR(out[1], std::sin(0.5), 1e-5);
}

static void test_yaw_only_drops_pitch_and_roll() {
    Pose h = compose(translation(0.1f, 1.2f, -0.3f), rotation_yaw_pitch(0.8f, 0.4f));
    Pose y = yaw_only(h);
    Pose expect = rotation_yaw_pitch(0.8f, 0);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) CHECK_NEAR(y.m[r][c], expect.m[r][c], 1e-5);
    CHECK_NEAR(y.m[1][3], 1.2, 1e-6);  // keeps the translation
}

static void test_left_eye_sees_scene_shifted_right() {
    // Left eye 32 mm left of the anchor; a point straight ahead appears to its right.
    float m[12];
    eye_space_transform(translation(-0.032f, 0, 0), 0.25f, m);
    const float p[3] = {0, 0, -10};  // game units
    float out[3];
    apply(m, p, out);
    CHECK_NEAR(out[0], 0.032 / 0.25, 1e-6);  // +0.128 units
    CHECK_NEAR(out[2], -10, 1e-6);
}

static void test_head_turn_counter_rotates_scene() {
    // Head turned right: a point straight ahead of the anchor moves to the eye's left (-x).
    float m[12];
    eye_space_transform(rotation_yaw_pitch(0.5f, 0), 0.25f, m);
    const float p[3] = {0, 0, -10};
    float out[3];
    apply(m, p, out);
    CHECK_NEAR(out[0], -10 * std::sin(0.5), 1e-4);
}

static void test_projection_edges() {
    float px[4], py[4];
    const float l = -1.1f, r = 0.9f, b = -1.2f, t = 1.0f;
    projection_rows(l, r, b, t, px, py);
    // A point on the right edge of the frustum at depth z maps to NDC x = +1 (w = -z).
    const float z = -5;
    const float xr = r * -z, xl = l * -z, yt = t * -z, yb = b * -z;
    CHECK_NEAR((px[0] * xr + px[2] * z) / -z, 1, 1e-5);
    CHECK_NEAR((px[0] * xl + px[2] * z) / -z, -1, 1e-5);
    CHECK_NEAR((py[1] * yt + py[2] * z) / -z, 1, 1e-5);
    CHECK_NEAR((py[1] * yb + py[2] * z) / -z, -1, 1e-5);
}

static void test_frustum_scissor() {
    // Eye identical to the game camera: the game frustum fills the image.
    float head[12];
    eye_space_transform(identity(), 0.25f, head);
    float px[4], py[4];
    projection_rows(-0.6f, 0.6f, -0.5f, 0.5f, px, py);
    int r[4];
    frustum_scissor(head, px, py, 0.6f, 0.5f, 1920, 1440, r);
    CHECK_NEAR(r[0], 0, 1);
    CHECK_NEAR(r[1], 0, 1);
    CHECK_NEAR(r[2], 1920, 1);
    CHECK_NEAR(r[3], 1440, 1);
    // Head turned 30 degrees right: the game frustum's right edge (+31 deg) is 1 deg right of the
    // eye's centre: ndc = tan(atan(0.6) - 30 deg) / 0.6.
    eye_space_transform(rotation_yaw_pitch(0.5236f, 0), 0.25f, head);
    frustum_scissor(head, px, py, 0.6f, 0.5f, 1920, 1440, r);
    CHECK_NEAR(r[0], 0, 1);
    const double edge = std::tan(std::atan(0.6) - 0.5236) / 0.6;
    CHECK_NEAR(r[2], (edge * 0.5 + 0.5) * 1920, 2);
    // Head turned 120 degrees: the game frustum is out of view entirely.
    eye_space_transform(rotation_yaw_pitch(2.1f, 0), 0.25f, head);
    frustum_scissor(head, px, py, 0.6f, 0.5f, 1920, 1440, r);
    if (r[2] - r[0] > 0 && r[3] - r[1] > 0 && !(r[0] == 0 && r[2] == 1920)) {
        std::printf("turned away: rect %d,%d-%d,%d should be empty\n", r[0], r[1], r[2], r[3]);
        ++g_failures;
    }
}

static void test_aim_angles_match_probe() {
    // Aim controller readings from the game (yaw -2.59336, pitch 0.0058; target 100 units out).
    const float eye[3] = {-11, 18, 289.5f}, tgt[3] = {-63.1333f, 18.5739f, 204.167f};
    const float d[3] = {tgt[0] - eye[0], tgt[1] - eye[1], tgt[2] - eye[2]};
    float yaw, pitch;
    aim_angles(d, &yaw, &pitch);
    CHECK_NEAR(yaw, -2.59336, 1e-3);
    CHECK_NEAR(pitch, 0.00581, 1e-3);
    // look_at's -z axis is the view direction, x is level.
    const Pose cam = look_at(eye, tgt);
    CHECK_NEAR(-cam.m[0][2], d[0] / 100, 1e-3);
    CHECK_NEAR(-cam.m[2][2], d[2] / 100, 1e-3);
    CHECK_NEAR(cam.m[1][0], 0, 1e-6);
}

static void test_world_lock_keeps_world_still() {
    // Anchor: the level frame of the camera when aiming started. Later the game camera turns
    // (yaw and pitch); with the lock, a world point lands in the same place in the eye.
    const float eye[3] = {-11, 18, 289.5f};
    const float t0[3] = {-11, 18, 189.5f};
    const Pose anchor = yaw_only(look_at(eye, t0));
    const float world[3] = {5, 30, 250};
    float expected[3], got[3];
    apply_pose(inverse(anchor), world, expected);
    const float targets[2][3] = {{-63, 40, 204}, {20, -10, 380}};
    for (const auto& t1 : targets) {
        const Pose cam = look_at(eye, t1);
        float in_cam[3];
        apply_pose(inverse(cam), world, in_cam);  // what the game's vertex shader has
        const Pose cam_in_anchor = compose(inverse(anchor), cam);
        float head[12];
        eye_space_transform(compose(inverse(cam_in_anchor), identity()), 1.0f, head);
        apply(head, in_cam, got);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(got[i], expected[i], 1e-3);
    }
}

int main() {
    test_aim_angles_match_probe();
    test_world_lock_keeps_world_still();
    test_frustum_scissor();
    test_inverse_compose();
    test_yaw_turns_right();
    test_yaw_only_drops_pitch_and_roll();
    test_left_eye_sees_scene_shifted_right();
    test_head_turn_counter_rotates_scene();
    test_projection_edges();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all headmath tests passed\n");
    return 0;
}
