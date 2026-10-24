#include "vr.h"

#include <openvr.h>

#include <cmath>
#include <cstring>

#include "../proxy/common.h"
#include "headmath.h"

namespace k7::vr {

static Mode g_mode = Mode::Off;
static float g_unit = 0.25f;
static float g_ipd = 0.064f;
static float g_sim_fov = 100;
static float g_inflate = 150;
static float g_sim_yaw, g_sim_pitch;  // degrees
static bool g_recenter = true;
static Pose g_reference = identity();

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ::vr::IVRSystem* g_system;
static ID3D11Texture2D* g_eye_tex[2];

static float ini_float(const wchar_t* key, float fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"vr", key, L"", buf, 64, path_in(L"k7vr.ini").c_str());
    return buf[0] ? float(_wtof(buf)) : fallback;
}

static Pose from_openvr(const ::vr::HmdMatrix34_t& m) {
    Pose p;
    memcpy(p.m, m.m, sizeof(p.m));
    return p;
}

static void apply_inflation() { set_fov_inflate(stereo() ? g_inflate : 0); }

static bool init_openvr() {
    ::vr::EVRInitError err = ::vr::VRInitError_None;
    // Delay-loaded: openvr_api.dll is only touched here.
    __try {
        g_system = ::vr::VR_Init(&err, ::vr::VRApplication_Scene);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("vr: openvr_api.dll could not be loaded");
        return false;
    }
    if (!g_system || err != ::vr::VRInitError_None) {
        logf("vr: VR_Init failed (%d)", int(err));
        g_system = nullptr;
        return false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        float l, r, t, b;
        g_system->GetProjectionRaw(::vr::EVREye(eye), &l, &r, &t, &b);
        logf("vr: eye %d raw tangents l %.3f r %.3f t %.3f b %.3f", eye, l, r, t, b);
    }
    uint32_t w = 0, h = 0;
    g_system->GetRecommendedRenderTargetSize(&w, &h);
    logf("vr: OpenVR up, recommended %ux%u per eye", w, h);
    return true;
}

void init(ID3D11Device* device, ID3D11DeviceContext* context) {
    g_dev = device;
    g_ctx = context;
    g_unit = ini_float(L"unit_m", g_unit);
    g_ipd = ini_float(L"ipd_m", g_ipd);
    g_sim_fov = ini_float(L"sim_fov", g_sim_fov);
    g_inflate = ini_float(L"fov_inflate", g_inflate);
    wchar_t m[32];
    GetPrivateProfileStringW(L"vr", L"mode", L"off", m, 32, path_in(L"k7vr.ini").c_str());
    if (_wcsicmp(m, L"openvr") == 0) {
        g_mode = init_openvr() ? Mode::OpenVR : Mode::Off;  // safe mode: no runtime -> play flat
    } else if (_wcsicmp(m, L"sim") == 0) {
        g_mode = Mode::Sim;
    }
    logf("vr: mode %ls -> %d, unit %.3f m, ipd %.3f m, inflate %g", m, int(g_mode), g_unit, g_ipd, g_inflate);
    apply_inflation();
}

Mode mode() { return g_mode; }
bool stereo() { return g_mode != Mode::Off; }

void set_mode(Mode m) {
    if (m == Mode::OpenVR || g_mode == Mode::OpenVR) return;
    g_mode = m;
    g_recenter = true;
    apply_inflation();
    logf("vr: mode %d", int(g_mode));
}

void recenter() { g_recenter = true; }
void set_sim_view(float yaw_deg, float pitch_deg) { g_sim_yaw = yaw_deg, g_sim_pitch = pitch_deg; }
void set_ipd(float metres) { g_ipd = metres; }
void set_unit(float metres_per_unit) { g_unit = metres_per_unit; }

static void symmetric_rows(float tan_y, float game_xy_ratio, EyeConsts& e) {
    const float tan_x = tan_y * game_xy_ratio;
    projection_rows(-tan_x, tan_x, -tan_y, tan_y, e.px, e.py);
}

void begin_frame(float game_xy_ratio, EyeConsts eyes[2]) {
    if (game_xy_ratio <= 0) game_xy_ratio = 1.18f;
    const float d2r = 3.14159265f / 180;
    for (int eye = 0; eye < 2; ++eye) memset(&eyes[eye], 0, sizeof(EyeConsts));

    if (g_mode == Mode::Off) {
        // Flat: identity head; if the game culls with an inflated FOV, project with the intended one.
        eye_space_transform(identity(), g_unit, eyes[0].head);
        const float intended = fov_inflate_intended();
        if (intended > 0) {
            symmetric_rows(tanf(intended * d2r / 2), game_xy_ratio, eyes[0]);
            eyes[0].flags[0] = 1;
        }
        eyes[1] = eyes[0];
        return;
    }

    Pose head;
    Pose eye_to_head[2];
    if (g_mode == Mode::OpenVR) {
        ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];
        ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        const auto& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
        head = hmd.bPoseIsValid ? from_openvr(hmd.mDeviceToAbsoluteTracking) : identity();
        for (int eye = 0; eye < 2; ++eye) {
            eye_to_head[eye] = from_openvr(g_system->GetEyeToHeadTransform(::vr::EVREye(eye)));
            const ::vr::HmdMatrix44_t p = g_system->GetProjectionMatrix(::vr::EVREye(eye), 0.1f, 100.f);
            for (int i = 0; i < 4; ++i) {
                eyes[eye].px[i] = p.m[0][i];
                eyes[eye].py[i] = p.m[1][i];
            }
            eyes[eye].px[3] = eyes[eye].py[3] = 0;  // no translation terms for an x/y row
        }
    } else {
        head = rotation_yaw_pitch(g_sim_yaw * d2r, g_sim_pitch * d2r);
        for (int eye = 0; eye < 2; ++eye) {
            eye_to_head[eye] = translation(eye == 0 ? -g_ipd / 2 : g_ipd / 2, 0, 0);
            symmetric_rows(tanf(g_sim_fov * d2r / 2), game_xy_ratio, eyes[eye]);
        }
    }

    if (g_recenter) {
        g_reference = yaw_only(head);
        g_recenter = false;
        logf("vr: recentred");
    }
    const Pose rel = compose(inverse(g_reference), head);  // head relative to the seated reference
    for (int eye = 0; eye < 2; ++eye) {
        eye_space_transform(compose(rel, eye_to_head[eye]), g_unit, eyes[eye].head);
        eyes[eye].flags[0] = 1;
    }
}

// Our own texture shaped like `like` (shader-resource only), recreated when the shape changes.
static bool ensure_like(ID3D11Texture2D*& t, ID3D11Texture2D* like) {
    D3D11_TEXTURE2D_DESC d;
    like->GetDesc(&d);
    if (t) {
        D3D11_TEXTURE2D_DESC have;
        t->GetDesc(&have);
        if (have.Width == d.Width && have.Height == d.Height && have.Format == d.Format) return true;
        t->Release();
        t = nullptr;
    }
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return false;
    logf("vr: texture %ux%u f%d", d.Width, d.Height, d.Format);
    return true;
}

static ID3D11Texture2D* black_texture() {
    static ID3D11Texture2D* black;
    if (!black) {
        static const uint32_t pixels[16 * 16] = {};
        D3D11_TEXTURE2D_DESC d{16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{pixels, 16 * 4, 0};
        g_dev->CreateTexture2D(&d, &init, &black);
    }
    return black;
}

static ::vr::HmdMatrix34_t to_openvr(const Pose& p) {
    ::vr::HmdMatrix34_t m;
    memcpy(m.m, p.m, sizeof(m.m));
    return m;
}

// A world-locked SteamVR overlay panel in front of the seated reference.
struct Panel {
    const char* key;
    const char* name;
    float width_m, distance_m, drop_m;
    bool opaque;
    ::vr::VROverlayHandle_t handle = ::vr::k_ulOverlayHandleInvalid;
    ID3D11Texture2D* copy = nullptr;
    bool shown = false;
};
static Panel g_hud_panel{"k7vr.hud", "Killer7 VR HUD", 1.4f, 2.0f, 0.1f, false};
static Panel g_screen_panel{"k7vr.screen", "Killer7 VR screen", 3.2f, 3.0f, 0.0f, true};

static void update_panel(Panel& p, ID3D11Texture2D* src) {
    auto overlay = ::vr::VROverlay();
    if (!overlay) return;
    if (!src) {
        if (p.shown) overlay->HideOverlay(p.handle), p.shown = false;
        return;
    }
    if (p.handle == ::vr::k_ulOverlayHandleInvalid) {
        const ::vr::EVROverlayError e = overlay->CreateOverlay(p.key, p.name, &p.handle);
        if (e != ::vr::VROverlayError_None) {
            logf("vr: CreateOverlay %s -> %d", p.key, int(e));
            return;
        }
        if (p.opaque)
            overlay->SetOverlayFlag(p.handle, ::vr::VROverlayFlags_IgnoreTextureAlpha, true);
        else  // the HUD's colour is already multiplied by its alpha (see draw_hud)
            overlay->SetOverlayFlag(p.handle, ::vr::VROverlayFlags_IsPremultiplied, true);
        overlay->SetOverlayWidthInMeters(p.handle, p.width_m);
    }
    if (!ensure_like(p.copy, src)) return;
    g_ctx->CopySubresourceRegion(p.copy, 0, 0, 0, 0, src, 0, nullptr);
    ::vr::Texture_t tex{p.copy, ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
    overlay->SetOverlayTexture(p.handle, &tex);
    const ::vr::HmdMatrix34_t m = to_openvr(compose(g_reference, translation(0, -p.drop_m, -p.distance_m)));
    overlay->SetOverlayTransformAbsolute(p.handle, ::vr::VRCompositor()->GetTrackingSpace(), &m);
    if (!p.shown) overlay->ShowOverlay(p.handle), p.shown = true;
}

void present(ID3D11Texture2D* left, ID3D11Texture2D* right, ID3D11Texture2D* hud, ID3D11Texture2D* screen) {
    if (g_mode != Mode::OpenVR) return;
    ID3D11Texture2D* eyes[2] = {black_texture(), black_texture()};
    if (left && right && ensure_like(g_eye_tex[0], left) && ensure_like(g_eye_tex[1], right)) {
        g_ctx->CopySubresourceRegion(g_eye_tex[0], 0, 0, 0, 0, left, 0, nullptr);
        g_ctx->CopySubresourceRegion(g_eye_tex[1], 0, 0, 0, 0, right, 0, nullptr);
        eyes[0] = g_eye_tex[0];
        eyes[1] = g_eye_tex[1];
    }
    for (int eye = 0; eye < 2; ++eye) {
        ::vr::Texture_t tex{eyes[eye], ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
        const ::vr::EVRCompositorError e = ::vr::VRCompositor()->Submit(::vr::EVREye(eye), &tex);
        static int errors;
        if (e != ::vr::VRCompositorError_None && errors++ < 10) logf("vr: Submit eye %d -> %d", eye, int(e));
    }
    update_panel(g_hud_panel, hud);
    update_panel(g_screen_panel, screen);
}

}  // namespace k7::vr
