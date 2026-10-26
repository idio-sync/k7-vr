#include "vr.h"

#include <openvr.h>

#include <cmath>
#include <cstring>

#include "../proxy/common.h"
#include "headmath.h"

namespace k7::vr {

void poll_controllers(::vr::IVRSystem* sys);  // input.cpp

static Mode g_mode = Mode::Off;
static float g_unit = 0.25f;
static float g_ipd = 0.064f;
static float g_sim_fov = 100;
static float g_inflate = 150;
static bool g_destripe = true;
static float g_sim_yaw, g_sim_pitch;  // degrees
static bool g_recenter = true;
static Pose g_reference = identity();
static Pose g_rel = identity();           // this frame's head relative to the reference
static Pose g_eye_to_head[2] = {identity(), identity()};
static Pose g_hand = identity();          // right controller relative to the reference (hand_pitch/offset applied)
static bool g_hand_valid;
static Pose g_hand_adjust = identity();   // [vr] hand_pitch, hand_offset
static float g_hand_offset[3];
static float g_sim_hand_yaw, g_sim_hand_pitch;

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static ::vr::IVRSystem* g_system;
static ID3D11Texture2D* g_eye_tex[2];
static unsigned g_eye_w, g_eye_h;   // resize target for the game's 4:3 render targets (0 = leave)
static float g_content_aspect = 4.f / 3.f;

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

static bool g_inflate_suspended;
static void apply_inflation() { set_fov_inflate(stereo() && !g_inflate_suspended ? g_inflate : 0); }

void suspend_inflation(bool suspend) {
    if (suspend == g_inflate_suspended) return;
    g_inflate_suspended = suspend;
    apply_inflation();
}

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
    g_destripe = ini_float(L"destripe", 1) != 0;
    {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"vr", L"hand_offset", L"", buf, 64, path_in(L"k7vr.ini").c_str());
        if (buf[0]) swscanf_s(buf, L"%f,%f,%f", &g_hand_offset[0], &g_hand_offset[1], &g_hand_offset[2]);
        set_hand_pitch(ini_float(L"hand_pitch", 0));
    }
    wchar_t m[32];
    GetPrivateProfileStringW(L"vr", L"mode", L"off", m, 32, path_in(L"k7vr.ini").c_str());
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {  // safe mode: play flat without touching the ini
        logf("vr: Shift held at start: safe mode, VR off");
        wcscpy_s(m, L"off");
    }
    if (_wcsicmp(m, L"openvr") == 0) {
        g_mode = init_openvr() ? Mode::OpenVR : Mode::Off;  // safe mode: no runtime -> play flat
    } else if (_wcsicmp(m, L"sim") == 0) {
        g_mode = Mode::Sim;
    }
    // Eye render target size.
    wchar_t res[32];
    GetPrivateProfileStringW(L"vr", L"eye_res", L"auto", res, 32, path_in(L"k7vr.ini").c_str());
    unsigned w = 0, h = 0;
    if (swscanf_s(res, L"%ux%u", &w, &h) == 2 && w && h) {
        g_eye_w = w, g_eye_h = h;
    } else if (_wcsicmp(res, L"auto") == 0 && g_mode == Mode::OpenVR) {
        uint32_t rw = 0, rh = 0;
        g_system->GetRecommendedRenderTargetSize(&rw, &rh);
        const float scale = ini_float(L"eye_scale", 1.0f);
        g_eye_w = unsigned(rw * scale + 0.5f) & ~1u;
        g_eye_h = unsigned(rh * scale + 0.5f) & ~1u;
    }
    logf("vr: mode %ls -> %d, unit %.3f m, ipd %.3f m, inflate %g, eye targets %ux%u", m, int(g_mode), g_unit, g_ipd,
         g_inflate, g_eye_w, g_eye_h);
    apply_inflation();
}

bool eye_resolution(unsigned* width, unsigned* height) {
    if (!stereo() || !g_eye_w || !g_eye_h) return false;
    *width = g_eye_w;
    *height = g_eye_h;
    return true;
}

void set_content_aspect(float aspect) { g_content_aspect = aspect; }

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
float unit() { return g_unit; }

bool hand_pose(Pose* out) {
    *out = g_hand;
    return g_hand_valid;
}

void set_hand_pitch(float degrees) {
    g_hand_adjust = compose(translation(g_hand_offset[0], g_hand_offset[1], g_hand_offset[2]),
                            rotation_yaw_pitch(0, degrees * 3.14159265f / 180));
    logf("vr: hand pitch %g degrees", degrees);
}

void set_sim_hand(float yaw_deg, float pitch_deg) { g_sim_hand_yaw = yaw_deg, g_sim_hand_pitch = pitch_deg; }

void relock(const Pose& cam_in_anchor, EyeConsts eyes[2]) {
    if (g_mode == Mode::Off) return;
    const Pose to_cam = inverse(cam_in_anchor);
    for (int eye = 0; eye < 2; ++eye)
        eye_space_transform(compose(to_cam, compose(g_rel, g_eye_to_head[eye])), g_unit, eyes[eye].head);
}
void set_sim_view(float yaw_deg, float pitch_deg) { g_sim_yaw = yaw_deg, g_sim_pitch = pitch_deg; }
void set_ipd(float metres) { g_ipd = metres; }
void set_unit(float metres_per_unit) { g_unit = metres_per_unit; }

static void symmetric_rows(float tan_y, float game_xy_ratio, EyeConsts& e) {
    const float tan_x = tan_y * game_xy_ratio;
    projection_rows(-tan_x, tan_x, -tan_y, tan_y, e.px, e.py);
}

void game_view(float game_xy_ratio, EyeConsts* out) {
    if (game_xy_ratio <= 0) game_xy_ratio = 1.18f;
    memset(out, 0, sizeof(EyeConsts));
    eye_space_transform(identity(), g_unit, out->head);
    // If the game culls with an inflated FOV, project with the one it meant: its frame copies
    // also feed screen effects (cutscene tints, the junction backdrop), which must keep their
    // framing. (Laser-pointer screens suspend inflation, so their hit test sees the real view.)
    const float intended = fov_inflate_intended();
    if (intended > 0) {
        symmetric_rows(tanf(intended * 3.14159265f / 360), game_xy_ratio, *out);
        out->flags[0] = 1;
    }
}

// The controller's pointing pose relative to its raw pose: the "tip" component of its render
// model (where SteamVR's own laser pointers come from), so the aim follows the way the
// controller is held. Identity if the model has no tip; [vr] hand_pitch then corrects by hand.
static Pose controller_tip(::vr::TrackedDeviceIndex_t device) {
    static ::vr::TrackedDeviceIndex_t cached_device = ::vr::k_unTrackedDeviceIndexInvalid;
    static Pose tip = identity();
    if (device == cached_device) return tip;
    cached_device = device;
    tip = identity();
    char model[256] = {};
    g_system->GetStringTrackedDeviceProperty(device, ::vr::Prop_RenderModelName_String, model, sizeof(model));
    ::vr::IVRRenderModels* models = ::vr::VRRenderModels();
    ::vr::VRControllerState_t state{};
    ::vr::RenderModel_ControllerMode_State_t mode{};
    ::vr::RenderModel_ComponentState_t comp{};
    if (models && model[0] &&
        models->GetComponentState(model, ::vr::k_pch_Controller_Component_Tip, &state, &mode, &comp)) {
        tip = from_openvr(comp.mTrackingToComponentLocal);
        // How far the tip's forward (-z) is tilted from the raw pose's, for the log.
        const float pitch = asinf(-tip.m[1][2] > 1 ? 1 : (-tip.m[1][2] < -1 ? -1 : -tip.m[1][2])) * 180 / 3.14159265f;
        logf("vr: right controller model '%s': tip at %.3f,%.3f,%.3f m, pointing %.1f degrees %s the raw axis", model,
             tip.m[0][3], tip.m[1][3], tip.m[2][3], fabsf(pitch), pitch >= 0 ? "above" : "below");
    } else {
        logf("vr: right controller model '%s' has no tip component; aiming along its raw axis (hand_pitch)", model);
    }
    return tip;
}

void begin_frame(float game_xy_ratio, EyeConsts eyes[2]) {
    if (game_xy_ratio <= 0) game_xy_ratio = 1.18f;
    const float d2r = 3.14159265f / 180;
    for (int eye = 0; eye < 2; ++eye) memset(&eyes[eye], 0, sizeof(EyeConsts));

    if (g_mode == Mode::Off) {
        game_view(game_xy_ratio, &eyes[0]);
        eyes[1] = eyes[0];
        return;
    }

    Pose head;
    Pose* eye_to_head = g_eye_to_head;
    Pose hand = identity();
    g_hand_valid = false;
    if (g_mode == Mode::OpenVR) {
        ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];
        ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        poll_controllers(g_system);
        const auto& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
        head = hmd.bPoseIsValid ? from_openvr(hmd.mDeviceToAbsoluteTracking) : identity();
        const ::vr::TrackedDeviceIndex_t right =
            g_system->GetTrackedDeviceIndexForControllerRole(::vr::TrackedControllerRole_RightHand);
        if (right < ::vr::k_unMaxTrackedDeviceCount && poses[right].bPoseIsValid) {
            hand = compose(from_openvr(poses[right].mDeviceToAbsoluteTracking), controller_tip(right));
            g_hand_valid = true;
        }
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
        // A right hand held low and forward, pointing where set_sim_hand says.
        hand = compose(translation(0.15f, -0.25f, -0.35f),
                       rotation_yaw_pitch(g_sim_hand_yaw * d2r, g_sim_hand_pitch * d2r));
        g_hand_valid = true;
    }

    if (g_recenter) {
        g_reference = yaw_only(head);
        g_recenter = false;
        logf("vr: recentred");
    }
    g_rel = compose(inverse(g_reference), head);  // head relative to the seated reference
    if (g_hand_valid) {
        // Sim hands are already relative to the reference's origin.
        g_hand = g_mode == Mode::OpenVR ? compose(inverse(g_reference), hand) : hand;
        g_hand = compose(g_hand, g_hand_adjust);
    }
    for (int eye = 0; eye < 2; ++eye) {
        eye_space_transform(compose(g_rel, eye_to_head[eye]), g_unit, eyes[eye].head);
        eyes[eye].flags[0] = 1;
    }
}

static bool uav_capable(DXGI_FORMAT f);

// Our own texture shaped like `like` (shader resource, plus UAV for de-striping when the format
// allows), recreated when the shape changes.
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
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav_capable(d.Format) ? D3D11_BIND_UNORDERED_ACCESS : 0);
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

// De-striping: the game draws many surfaces as alternating 1-pixel columns (its 1920-wide frame
// is meant to be scaled down to the window, which blends each column pair). The headset shows
// the eye images near 1:1, where the columns read as fine vertical lines, so they're blended here
// with a horizontal 1-2-1 filter on the way to the headset ([vr] destripe=0 turns it off).
static ID3D11ComputeShader* g_destripe_cs;
static bool g_destripe_failed;

static const char kDestripeHlsl[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h; src.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  int x = int(id.x), y = int(id.y);\n"
    "  dst[id.xy] = 0.5f * src[int2(x, y)] + 0.25f * (src[int2(max(x - 1, 0), y)] + src[int2(min(x + 1, int(w) - 1), y)]);\n"
    "}\n";

static bool destripe_shader() {
    if (g_destripe_cs) return true;
    if (g_destripe_failed) return false;
    g_destripe_failed = true;  // until it works
    if (ID3DBlob* code = compile_hlsl(kDestripeHlsl, "cs_5_0", "k7vr_destripe")) {
        if (SUCCEEDED(g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_destripe_cs)))
            g_destripe_failed = false;
        code->Release();
    }
    return !g_destripe_failed;
}

static bool uav_capable(DXGI_FORMAT f) {
    UINT support = 0;
    return SUCCEEDED(g_dev->CheckFormatSupport(f, &support)) &&
           (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
}

// dst (made by ensure_like with UAV binding) = filtered src. False: caller copies instead.
static bool destripe(ID3D11Texture2D* src, ID3D11Texture2D* dst) {
    if (!g_destripe || !destripe_shader()) return false;
    D3D11_TEXTURE2D_DESC sd, dd;
    src->GetDesc(&sd);
    dst->GetDesc(&dd);
    if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sd.SampleDesc.Count != 1 ||
        !(dd.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
        return false;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format = sd.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    bool ok = SUCCEEDED(g_dev->CreateShaderResourceView(src, &svd, &srv)) &&
              SUCCEEDED(g_dev->CreateUnorderedAccessView(dst, nullptr, &uav));
    if (ok) {
        // Keep the game's compute state as it was.
        ID3D11ComputeShader* old_cs = nullptr;
        ID3D11ShaderResourceView* old_srv = nullptr;
        ID3D11UnorderedAccessView* old_uav = nullptr;
        g_ctx->CSGetShader(&old_cs, nullptr, nullptr);
        g_ctx->CSGetShaderResources(0, 1, &old_srv);
        g_ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);
        g_ctx->CSSetShader(g_destripe_cs, nullptr, 0);
        g_ctx->CSSetShaderResources(0, 1, &srv);
        g_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        g_ctx->Dispatch((sd.Width + 7) / 8, (sd.Height + 7) / 8, 1);
        ID3D11UnorderedAccessView* none = nullptr;
        g_ctx->CSSetUnorderedAccessViews(0, 1, old_uav ? &old_uav : &none, nullptr);
        g_ctx->CSSetShaderResources(0, 1, &old_srv);
        g_ctx->CSSetShader(old_cs, nullptr, 0);
        if (old_cs) old_cs->Release();
        if (old_srv) old_srv->Release();
        if (old_uav) old_uav->Release();
    }
    if (srv) srv->Release();
    if (uav) uav->Release();
    return ok;
}

// Copy of `src` in `dst` (recreated to match), de-striped when possible.
static bool prepare(ID3D11Texture2D*& dst, ID3D11Texture2D* src) {
    if (!ensure_like(dst, src)) return false;
    if (!destripe(src, dst)) g_ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, nullptr);
    return true;
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
    if (src != p.copy) {
        if (!ensure_like(p.copy, src)) return;
        g_ctx->CopySubresourceRegion(p.copy, 0, 0, 0, 0, src, 0, nullptr);
    }
    D3D11_TEXTURE2D_DESC d;
    src->GetDesc(&d);  // the picture inside is 4:3 even when the texture was resized for the eyes
    overlay->SetOverlayTexelAspect(p.handle, g_content_aspect * float(d.Height) / float(d.Width));
    ::vr::Texture_t tex{p.copy, ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
    overlay->SetOverlayTexture(p.handle, &tex);
    const ::vr::HmdMatrix34_t m = to_openvr(compose(g_reference, translation(0, -p.drop_m, -p.distance_m)));
    overlay->SetOverlayTransformAbsolute(p.handle, ::vr::VRCompositor()->GetTrackingSpace(), &m);
    if (!p.shown) overlay->ShowOverlay(p.handle), p.shown = true;
}

static ID3D11Texture2D* g_screen_tex;
static bool g_show_hud = true, g_show_screen = true;
static ID3D11Texture2D* g_submitted[3];  // this frame's left, right, screen as sent (diagnostics)

ID3D11Texture2D* submitted(int which) { return which >= 0 && which < 3 ? g_submitted[which] : nullptr; }

void present(ID3D11Texture2D* left, ID3D11Texture2D* right, ID3D11Texture2D* hud, ID3D11Texture2D* screen) {
    if (g_mode == Mode::Off) return;
    ID3D11Texture2D* eyes[2] = {black_texture(), black_texture()};
    g_submitted[0] = g_submitted[1] = g_submitted[2] = nullptr;
    if (left && right && prepare(g_eye_tex[0], left) && prepare(g_eye_tex[1], right)) {
        eyes[0] = g_submitted[0] = g_eye_tex[0];
        eyes[1] = g_submitted[1] = g_eye_tex[1];
    }
    if (screen && prepare(g_screen_tex, screen)) screen = g_submitted[2] = g_screen_tex;
    if (g_mode != Mode::OpenVR) return;  // sim: prepared for the pair captures only
    for (int eye = 0; eye < 2; ++eye) {
        ::vr::Texture_t tex{eyes[eye], ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
        const ::vr::EVRCompositorError e = ::vr::VRCompositor()->Submit(::vr::EVREye(eye), &tex);
        static int errors;
        if (e != ::vr::VRCompositorError_None && errors++ < 10) logf("vr: Submit eye %d -> %d", eye, int(e));
    }
    update_panel(g_hud_panel, g_show_hud ? hud : nullptr);
    update_panel(g_screen_panel, g_show_screen ? screen : nullptr);
}

ID3D11Texture2D* panel_texture(int which) { return which == 0 ? g_hud_panel.copy : g_screen_panel.copy; }

void set_panels(bool hud, bool screen) {
    g_show_hud = hud;
    g_show_screen = screen;
    logf("vr: panels hud %d screen %d", int(hud), int(screen));
}

}  // namespace k7::vr
