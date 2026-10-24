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

int main() {
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
