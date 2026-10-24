// D3D11 probe: hooks the game's device, immediate context (through the ctx_wrap stand-in)
// and swap chain. Each frame it counts draws; on request it writes the next frame's full
// draw list (targets, viewports, shaders, the GX projection from constant buffer 0) and a PNG.
#include "common.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../vr/headmath.h"
#include "../vr/vr.h"
#include "ctx_wrap.h"
#include "vtbl_index.h"

namespace k7 {

// Private data on shader objects: their DXBC hash.
static const GUID kShaderHashGuid = {0x6c1d3e0a, 0x4b7f, 0x4c55, {0x9a, 0x10, 0x2e, 0x77, 0x31, 0x5d, 0x0b, 0x01}};
// Private data on shader objects: bitmask of declared texture slots.
static const GUID kTextureSlotsGuid = {0x6c1d3e0a, 0x4b7f, 0x4c55, {0x9a, 0x10, 0x2e, 0x77, 0x31, 0x5d, 0x0b, 0x02}};

// GX vertex template's cbuffer: projMatrix follows 64 + 32 + 64 float4 registers.
constexpr UINT kProjOffset = (64 + 32 + 64) * 16;
constexpr UINT kTemplateCbMin = kProjOffset + 64;

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;      // the real immediate context
static ID3D11DeviceContext* g_wrapped;  // what the game holds

// ---- Constant buffer shadows (contents as last written by the CPU) ----

static std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> g_cb;
static float g_last_proj[16];  // the most recent perspective projMatrix the game wrote
static std::unordered_map<ID3D11Resource*, int> g_cb_kind;  // template cbuffers: 1 perspective, 2 orthographic
static std::unordered_map<ID3D11Resource*, void*> g_cb_mapped;

static UINT constant_buffer_size(ID3D11Resource* res) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
    D3D11_BUFFER_DESC desc;
    static_cast<ID3D11Buffer*>(res)->GetDesc(&desc);
    return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) ? desc.ByteWidth : 0;
}

// ---- Capture state ----

struct Capture {
    bool active = false;
    bool shot = false;
    unsigned index = 0;
    std::string text;
};
static Capture g_cap;
static unsigned g_frame;
static unsigned g_draws;
static unsigned g_draws_last;
static std::unordered_map<const void*, int> g_ids;

static int id_of(const void* p) {
    if (!p) return 0;
    auto [it, added] = g_ids.try_emplace(p, int(g_ids.size()) + 1);
    return it->second;
}

static uint64_t shader_hash(ID3D11DeviceChild* shader) {
    if (!shader) return 0;
    uint64_t h = 0;
    UINT size = sizeof(h);
    if (FAILED(shader->GetPrivateData(kShaderHashGuid, &size, &h))) return 0;
    return h;
}

static void appendf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) s.append(buf, size_t(n) < sizeof(buf) ? size_t(n) : sizeof(buf) - 1);
}

static void describe_resource(std::string& s, const char* label, ID3D11Resource* res) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        appendf(s, " %s=#%d(%ux%u f%d%s)", label, id_of(res), d.Width, d.Height, d.Format,
                d.SampleDesc.Count > 1 ? " msaa" : "");
    } else {
        appendf(s, " %s=#%d(dim%d)", label, id_of(res), dim);
    }
}

static void describe_view(std::string& s, const char* label, ID3D11View* view) {
    if (!view) return;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    describe_resource(s, label, res);
    res->Release();
}

static void append_matrix(std::string& s, const char* label, const float* m, int rows) {
    appendf(s, " %s=[", label);
    for (int i = 0; i < rows * 4; ++i) appendf(s, i == 0 ? "%.4g" : (i % 4 == 0 ? "|%.4g" : " %.4g"), m[i]);
    s += ']';
}

constexpr UINT kCaptureTextures = 4;

static void capture_draw(ID3D11DeviceContext* c, const char* kind, UINT count, UINT instances) {
    std::string& s = g_cap.text;
    appendf(s, "%4u %s n=%u", g_draws, kind, count);
    if (instances != 1) appendf(s, " inst=%u", instances);

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    describe_view(s, "rt", rtv);
    describe_view(s, "ds", dsv);
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();

    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    if (nvp) appendf(s, " vp=%g,%g,%gx%g", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);

    D3D11_PRIMITIVE_TOPOLOGY topo;
    c->IAGetPrimitiveTopology(&topo);
    appendf(s, " topo=%d", topo);

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
    appendf(s, " vs=%016llx ps=%016llx", shader_hash(vs), shader_hash(ps));
    if (gs) appendf(s, " gs=%016llx", shader_hash(gs));
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (gs) gs->Release();

    ID3D11Buffer* cb[2] = {};
    c->VSGetConstantBuffers(0, 2, cb);
    if (cb[0]) {
        auto it = g_cb.find(cb[0]);
        if (it == g_cb.end()) {
            appendf(s, " cb0=#%d(no shadow)", id_of(cb[0]));
        } else {
            const auto& bytes = it->second;
            appendf(s, " cb0=#%d(%zu)", id_of(cb[0]), bytes.size());
            auto f = reinterpret_cast<const float*>(bytes.data());
            if (bytes.size() >= kTemplateCbMin) {
                // GX: projMatrix rows, then rows 0-2 of position matrix 0 (model-view).
                const float* p = f + kProjOffset / 4;
                bool ortho = p[12] == 0 && p[13] == 0 && p[14] == 0 && p[15] == 1;
                appendf(s, " %s", ortho ? "ORTHO" : "PERSP");
                append_matrix(s, "proj", p, 4);
                append_matrix(s, "mv0", f, 3);
            } else if (bytes.size() >= 64) {
                append_matrix(s, "m", f, 4);
            }
        }
    }
    if (cb[1]) appendf(s, " cb1=#%d", id_of(cb[1]));
    for (auto b : cb)
        if (b) b->Release();
    ID3D11ShaderResourceView* srv[kCaptureTextures] = {};
    c->PSGetShaderResources(0, kCaptureTextures, srv);
    for (UINT i = 0; i < kCaptureTextures; ++i) {
        if (!srv[i]) continue;
        char label[8];
        sprintf_s(label, "t%u", i);
        describe_view(s, label, srv[i]);
        srv[i]->Release();
    }
    s += '\n';
}

// ---- Frame image: the game's own frame, as sampled by its final blit to the window ----
// (The backbuffer is useless while the window is minimized: DXGI shrinks it to 8x8.)

static ID3D11Texture2D* g_backbuffer;   // raw pointer for comparison only, refreshed each frame
static bool g_want_image;                // this frame's image is wanted (shot or capture)
static ID3D11Texture2D* g_image;         // our copy of the final blit's source
static bool g_image_valid;

// ---- Stereo ----
// The patched vertex template reads each eye's head transform and projection rows from slot
// b12 (vr::EyeConsts). While stereo is on, every draw into a game render texture (anything but
// the window's backbuffer) runs twice: the left eye as the game issued it, the right eye with
// its render targets, depth buffer and sampled textures swapped for right-eye twins. Clears and
// copies of twinned textures are mirrored, so post passes and EFB copies run per eye. HUD draws
// (orthographic draws into the scene target after the frame's first perspective draw) go once
// into a transparent overlay texture, shown on a panel in the headset. View-dependent effect
// cards (known pixel shaders) are scissored to where the game camera's frustum lands in each eye.

constexpr UINT kStereoSlot = 12;
constexpr UINT kMaxTargets = 4;
constexpr UINT kMaxTextures = 8;
static ID3D11Buffer* g_eye_cb[2];  // left, right
static ID3D11Buffer* g_gv_cb;      // game-view pass (vr::game_view)
static ID3D11Buffer* g_draw_cb[2];  // slot b13 per-draw transform: [0] none, [1] first-person arm and gun
constexpr UINT kDrawSlot = 13;
static bool g_aim_mode;             // the game's first-person aim camera is active this frame
static int g_gun_run;               // 0 before the frame's first 3D draw, 1 inside the arm/gun run, 2 after
static uint64_t g_gun_ps;
static vr::EyeConsts g_eyes[2];    // this frame's eyes
static bool g_pair;                // save both eyes at the end of this frame
static unsigned g_pair_index;

static std::unordered_map<ID3D11Resource*, ID3D11Texture2D*> g_twins;  // game texture -> right-eye twin
static std::unordered_map<ID3D11View*, ID3D11View*> g_twin_views;     // game view -> same view of the twin

static bool g_frame_3d;              // a perspective draw went into the scene target this frame
static ID3D11Texture2D* g_scene_tex; // the scene target of this frame's last perspective draw (diagnostics)
static ID3D11Texture2D* g_final[2];  // each eye's final image (the texture the game blits to the window)
static ID3D11Texture2D* g_hud_tex;
static ID3D11RenderTargetView* g_hud_rtv;
static std::vector<uint64_t> g_card_ps;  // pixel shaders of view-dependent effect cards
static bool g_card_skip;                 // [vr] card_mode=skip
static std::unordered_map<ID3D11RasterizerState*, ID3D11RasterizerState*> g_scissor_states;

// ---- Aim mode (M4) ----
// While the game's first-person aim camera is active:
//  - the anchor (the player's frame) is the level frame of the aim camera when aiming began, so
//    the world stays put however the game camera turns (the head transforms are rebuilt against
//    this frame's camera at its first 3D draw, after the game updated it);
//  - the right controller steers the game's aim: the camera looks from the aim eye at the point
//    aim_converge_m along the controller's ray, so shots land where the hand points at that range;
//  - the first-person arm and gun are moved from the game's aim view into the hand.
static float g_gun_scale = 0.6f;
static float g_gun_pivot[3] = {0.45f, -0.9f, -2.5f};  // the gun's grip in the game's aim view (game units)
static float g_gun_clip_z = -1.5f;                    // the arm is cut off nearer the camera than this
static float g_aim_converge = 10;                     // metres
static bool g_anchor_set;
static vr::Pose g_anchor;                             // world pose (game units) of the player's frame
static bool g_relocked;                               // this frame's head transforms follow its camera
static float g_laser_muzzle = 0.15f;                  // the laser starts this far forward of the grip, metres
static float g_laser_length = 30;                     // metres; [vr] laser_m, 0 = no laser
static float g_aim_hud_mask = 0.6f;                   // HUD centre band hidden in aim mode (the reticle)
static bool g_laser_depth = true;                     // walls hide the laser (`gun laserdepth 0`: debugging)
static bool g_laser_ready;                            // g_laser_pose is this frame's
static vr::Pose g_laser_pose;                         // the hand in the game camera's eye space (game units)

static void init_draw_buffers() {
    wchar_t buf[64];
    const std::wstring ini = path_in(L"k7vr.ini");
    GetPrivateProfileStringW(L"vr", L"gun_scale", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_scale = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"gun_pivot", L"", buf, 64, ini.c_str());
    if (buf[0]) swscanf_s(buf, L"%f,%f,%f", &g_gun_pivot[0], &g_gun_pivot[1], &g_gun_pivot[2]);
    GetPrivateProfileStringW(L"vr", L"gun_clip_z", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_clip_z = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"aim_converge_m", L"", buf, 64, ini.c_str());
    if (buf[0]) g_aim_converge = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"laser_m", L"", buf, 64, ini.c_str());
    if (buf[0]) g_laser_length = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"aim_hud_mask", L"", buf, 64, ini.c_str());
    if (buf[0]) g_aim_hud_mask = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"aim_pitch_limit", L"80", buf, 64, ini.c_str());
    const float limit = float(_wtof(buf));
    if (limit > 36 && limit < 90) aim_widen_clamp(limit * 3.14159265f / 180);
    // [0]: no transform. [1]: the arm and gun, rewritten each aim-mode frame (until the first,
    // shrunk in place around the grip).
    const float none[16] = {};
    const float s = g_gun_scale, k = 1 - s;
    const float gun[16] = {s, 0, 0, k * g_gun_pivot[0], 0, s, 0, k * g_gun_pivot[1], 0, 0, s, k * g_gun_pivot[2],
                           1, 100, 0, 0};  // not clipped
    D3D11_BUFFER_DESC d{sizeof(gun), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA i0{none, 0, 0}, i1{gun, 0, 0};
    g_dev->CreateBuffer(&d, &i0, &g_draw_cb[0]);
    g_dev->CreateBuffer(&d, &i1, &g_draw_cb[1]);
    g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
    logf("stereo: gun scale %g, aim converges at %g m", g_gun_scale, g_aim_converge);
}

void set_gun_param(const std::string& name, const float v[3]) {
    if (name == "scale") g_gun_scale = v[0];
    else if (name == "pivot") memcpy(g_gun_pivot, v, sizeof(g_gun_pivot));
    else if (name == "clip") g_gun_clip_z = v[0];
    else if (name == "converge") g_aim_converge = v[0];
    else if (name == "hudmask") g_aim_hud_mask = v[0];
    else if (name == "laser") g_laser_length = v[0];
    else if (name == "laserdepth") g_laser_depth = v[0] != 0;
    else logf("gun: unknown setting '%s'", name.c_str());
    logf("gun: scale %g pivot %g,%g,%g clip %g converge %g hud mask %g", g_gun_scale, g_gun_pivot[0], g_gun_pivot[1],
         g_gun_pivot[2], g_gun_clip_z, g_aim_converge, g_aim_hud_mask);
}

// Frame start, aim mode: point the game's aim where the hand points.
static void steer_aim() {
    vr::Pose hand;
    if (!g_anchor_set || !vr::hand_pose(&hand)) return;
    // Target point in the anchor frame (metres), then its direction from the aim eye in the world.
    const float c = g_aim_converge;
    const float p[3] = {hand.m[0][3] - c * hand.m[0][2], hand.m[1][3] - c * hand.m[1][2], hand.m[2][3] - c * hand.m[2][2]};
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = g_anchor.m[i][0] * p[0] + g_anchor.m[i][1] * p[1] + g_anchor.m[i][2] * p[2];
    float yaw, pitch;
    vr::aim_angles(d, &yaw, &pitch);
    aim_set(yaw, pitch);
}

// First 3D draw of an aim-mode frame: the game camera is final now. Rebuild both eyes against
// it, and put the gun in the hand.
static void relock_aim() {
    g_relocked = true;
    float eye[3], tgt[3];
    if (!g_anchor_set || !aim_camera(eye, tgt)) return;
    const float u = vr::unit();
    const vr::Pose cam = vr::compose(vr::inverse(g_anchor), vr::look_at(eye, tgt));  // game units
    vr::relock(vr::scaled(cam, u), g_eyes);
    for (int e = 0; e < 2; ++e) g_ctx->UpdateSubresource(g_eye_cb[e], 0, nullptr, &g_eyes[e], 0, 0);

    vr::Pose hand;
    if (!g_draw_cb[1] || !vr::hand_pose(&hand)) return;
    // Game aim view -> anchor: the grip goes to the hand, scaled by gun_scale around the grip.
    const vr::Pose q = vr::compose(vr::inverse(cam), vr::scaled(hand, 1 / u));
    g_laser_pose = q;
    g_laser_ready = g_laser_length > 0;
    const float s = g_gun_scale;
    float m[16] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = q.m[i][j] * s;
        m[i * 4 + 3] = q.m[i][3] - s * (q.m[i][0] * g_gun_pivot[0] + q.m[i][1] * g_gun_pivot[1] + q.m[i][2] * g_gun_pivot[2]);
    }
    m[12] = 1;
    m[13] = g_gun_clip_z;
    g_ctx->UpdateSubresource(g_draw_cb[1], 0, nullptr, m, 0, 0);
}

// ---- The aim laser ----
// A thin additive beam from the gun's muzzle along the hand's ray, drawn into both eyes' scene
// targets with the scene's depth (walls hide it), once per aim-mode frame at the first HUD draw.
// It is projected like the game's own draws: the eye's head transform and x/y rows (slot b12),
// the z/w rows of the game's last perspective projection and its pixel centre.
static bool g_laser_done;
static ID3D11VertexShader* g_laser_vs;
static ID3D11PixelShader* g_laser_ps;
static ID3D11Buffer* g_laser_cb;
static ID3D11RasterizerState* g_laser_rs;
static ID3D11BlendState* g_laser_bs;
static ID3D11DepthStencilState* g_laser_ds;
static ID3D11DepthStencilState* g_laser_ds_off;
static bool g_laser_failed;

// After projMatrix[4], ambientColor[2], materialColor[2] and five light arrays of 8.
constexpr UINT kPixelCenterOffset = kProjOffset + (4 + 2 + 2 + 8 * 5) * 16;
static_assert(kPixelCenterOffset + 16 == 3344, "pixelCenter is the template constants' last register");

static const char kLaserVs[] =
    "cbuffer Laser : register(b0) { float4 a; float4 b; float4 projz; float4 projw; float4 pc; float4 color; float4 width; };\n"
    "cbuffer K7Stereo : register(b12) { float4 k7_head[3]; float4 k7_px; float4 k7_py; float4 k7_flags; };\n"
    "struct V { float4 pos : SV_POSITION; float t : TEXCOORD0; };\n"
    "float3 eye_of(float4 p) { return float3(dot(k7_head[0], p), dot(k7_head[1], p), dot(k7_head[2], p)); }\n"
    "V main(uint id : SV_VertexID) {\n"
    "  float3 A = eye_of(a), B = eye_of(b);\n"
    "  float3 m = (id < 2) ? A : B;\n"
    "  float3 side = normalize(cross(B - A, m)) * max(width.x * length(m), width.y);\n"
    "  float4 p = float4(m + ((id & 1) ? side : -side), 1);\n"
    "  V o;\n"
    "  o.pos = float4(dot(p, k7_px), dot(p, k7_py), dot(p, projz), dot(p, projw));\n"
    "  o.pos.z = o.pos.w * pc.w - o.pos.z * pc.z;\n"
    "  o.pos.xy -= o.pos.w * pc.xy;\n"
    "  o.t = (id < 2) ? 0 : 1;\n"
    "  return o;\n"
    "}\n";
static const char kLaserPs[] =
    "cbuffer Laser : register(b0) { float4 a; float4 b; float4 projz; float4 projw; float4 pc; float4 color; float4 width; };\n"
    "float4 main(float4 pos : SV_POSITION, float t : TEXCOORD0) : SV_Target { return float4(color.rgb * (1 - 0.8 * t), 0); }\n";

static bool laser_init() {
    if (g_laser_vs) return true;
    if (g_laser_failed) return false;
    g_laser_failed = true;
    ID3DBlob* vs = compile_hlsl(kLaserVs, "vs_5_0", "k7vr_laser_vs");
    ID3DBlob* ps = compile_hlsl(kLaserPs, "ps_5_0", "k7vr_laser_ps");
    if (vs && ps && SUCCEEDED(g_dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_laser_vs)) &&
        SUCCEEDED(g_dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_laser_ps))) {
        D3D11_BUFFER_DESC bd{7 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.f, 0.f, TRUE, FALSE, FALSE, FALSE};
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
                                 D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        // The game clears depth to 0 and keeps the nearest = largest value.
        D3D11_DEPTH_STENCIL_DESC dd{TRUE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_GREATER_EQUAL};
        D3D11_DEPTH_STENCIL_DESC off{FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS};
        if (SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_laser_cb)) && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_laser_rs)) &&
            SUCCEEDED(g_dev->CreateBlendState(&blend, &g_laser_bs)) &&
            SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_laser_ds)) &&
            SUCCEEDED(g_dev->CreateDepthStencilState(&off, &g_laser_ds_off)))
            g_laser_failed = false;
    }
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (g_laser_failed) logf("aim: laser unavailable");
    return !g_laser_failed;
}

template <typename V>
static V* twin_view(V* view, bool create);

// Into the bound scene target (left eye) and its twin (right eye).
static void draw_laser(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    if (g_laser_done) return;
    static int skipped;
    if (!g_laser_ready || !rtv || !dsv || !laser_init()) {
        if (skipped++ < 3) logf("laser: skipped (ready %d rtv %p dsv %p)", g_laser_ready, rtv, dsv);
        return;
    }
    g_laser_done = true;
    // The scene draw's projection z/w rows and pixel centre.
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kPixelCenterOffset + 16) {
        if (skipped++ < 3) logf("laser: no shadow of the scene constants");
        return;
    }
    const float* f = reinterpret_cast<const float*>(it->second.data());
    struct {
        float a[4], b[4], projz[4], projw[4], pc[4], color[4], width[4];
    } c{};
    const float u = vr::unit();
    const vr::Pose& q = g_laser_pose;
    const float d0 = g_laser_muzzle / u, d1 = g_laser_length / u;
    for (int i = 0; i < 3; ++i) {
        c.a[i] = q.m[i][3] - d0 * q.m[i][2];
        c.b[i] = q.m[i][3] - d1 * q.m[i][2];
    }
    c.a[3] = c.b[3] = 1;
    // The bound constants are the HUD's (orthographic): depth rows from the scene's perspective
    // projection, the pixel centre (per render target) from these.
    memcpy(c.projz, g_last_proj + 8, 16);
    memcpy(c.projw, g_last_proj + 12, 16);
    memcpy(c.pc, f + kPixelCenterOffset / 4, 16);
    const float color[4] = {1.0f, 0.12f, 0.08f, 1};
    memcpy(c.color, color, 16);
    c.width[0] = 0.0012f;          // half-width per unit of distance (constant angular size)
    c.width[1] = 0.003f / u;       // at least 3 mm
    g_ctx->UpdateSubresource(g_laser_cb, 0, nullptr, &c, 0, 0);
    static int logged;
    if (logged++ < 3) {
        // Where the beam starts and ends in the left eye (clip space), as the shader computes it.
        auto clip = [&](const float* p, float out[4]) {
            float e[4] = {0, 0, 0, 1};
            for (int i = 0; i < 3; ++i)
                e[i] = g_eyes[0].head[i * 4] * p[0] + g_eyes[0].head[i * 4 + 1] * p[1] + g_eyes[0].head[i * 4 + 2] * p[2] +
                       g_eyes[0].head[i * 4 + 3];
            auto dot4 = [&](const float* r) { return r[0] * e[0] + r[1] * e[1] + r[2] * e[2] + r[3] * e[3]; };
            out[0] = dot4(g_eyes[0].px), out[1] = dot4(g_eyes[0].py), out[2] = dot4(c.projz), out[3] = dot4(c.projw);
            out[2] = out[3] * c.pc[3] - out[2] * c.pc[2];
        };
        float ca[4], cb[4];
        clip(c.a, ca);
        clip(c.b, cb);
        logf("laser: a %.2f,%.2f,%.2f b %.2f,%.2f,%.2f -> clip a %.3f,%.3f,%.3f w%.3f b %.3f,%.3f,%.3f w%.3f pc %g,%g,%g,%g", c.a[0],
             c.a[1], c.a[2], c.b[0], c.b[1], c.b[2], ca[0], ca[1], ca[2], ca[3], cb[0], cb[1], cb[2], cb[3], c.pc[0], c.pc[1],
             c.pc[2], c.pc[3]);
    }

    // Save what we change.
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    ID3D11InputLayout* il = nullptr;
    g_ctx->IAGetInputLayout(&il);
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    g_ctx->VSGetShader(&vs, nullptr, nullptr);
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    g_ctx->GSGetShader(&gs, nullptr, nullptr);
    ID3D11Buffer *vcb = nullptr, *pcb = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &vcb);
    g_ctx->PSGetConstantBuffers(0, 1, &pcb);
    ID3D11RasterizerState* rs = nullptr;
    g_ctx->RSGetState(&rs);
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4];
    UINT mask;
    g_ctx->OMGetBlendState(&bs, factor, &mask);
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref;
    g_ctx->OMGetDepthStencilState(&ds, &ref);
    ID3D11RenderTargetView* old_rtv[kMaxTargets] = {};
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_ctx->OMGetRenderTargets(kMaxTargets, old_rtv, &old_dsv);

    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_laser_vs, nullptr, 0);
    g_ctx->PSSetShader(g_laser_ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_laser_cb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_laser_cb);
    g_ctx->RSSetState(g_laser_rs);
    const FLOAT zero[4] = {};
    g_ctx->OMSetBlendState(g_laser_bs, zero, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_laser_depth ? g_laser_ds : g_laser_ds_off, 0);
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11RenderTargetView* r = eye == 0 ? rtv : twin_view(rtv, false);
        ID3D11DepthStencilView* d = eye == 0 ? dsv : twin_view(dsv, false);
        if (!r || !d) continue;
        g_ctx->OMSetRenderTargets(1, &r, d);
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[eye]);
        g_ctx->Draw(4, 0);
    }
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);

    g_ctx->OMSetRenderTargets(kMaxTargets, old_rtv, old_dsv);
    g_ctx->OMSetDepthStencilState(ds, ref);
    g_ctx->OMSetBlendState(bs, factor, mask);
    g_ctx->RSSetState(rs);
    g_ctx->VSSetConstantBuffers(0, 1, &vcb);
    g_ctx->PSSetConstantBuffers(0, 1, &pcb);
    g_ctx->GSSetShader(gs, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->IASetInputLayout(il);
    g_ctx->IASetPrimitiveTopology(topo);
    for (auto v : old_rtv)
        if (v) v->Release();
    for (IUnknown* p : {static_cast<IUnknown*>(old_dsv), static_cast<IUnknown*>(ds), static_cast<IUnknown*>(bs),
                        static_cast<IUnknown*>(rs), static_cast<IUnknown*>(pcb), static_cast<IUnknown*>(vcb),
                        static_cast<IUnknown*>(gs), static_cast<IUnknown*>(ps), static_cast<IUnknown*>(vs),
                        static_cast<IUnknown*>(il)})
        if (p) p->Release();
}

// Before the game draws a frame: both eyes' transforms and projections (waits for headset
// poses in OpenVR mode).
static void set_eye_offsets() {
    if (!g_eye_cb[0] || !g_eye_cb[1]) return;
    const bool aim = vr::stereo() && aim_mode_now();
    if (aim != g_aim_mode) logf("stereo: aim mode %s", aim ? "on" : "off");
    if (aim && !g_aim_mode) {  // aiming began: the player's frame is the aim camera, levelled
        float eye[3], tgt[3];
        g_anchor_set = aim_camera(eye, tgt);
        if (g_anchor_set) g_anchor = vr::yaw_only(vr::look_at(eye, tgt));
    }
    if (!aim) g_anchor_set = false;
    g_aim_mode = aim;
    g_gun_run = 0;
    g_relocked = false;
    g_laser_ready = g_laser_done = false;
    if (g_draw_cb[0]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
    const float ratio = g_last_proj[0] != 0 ? g_last_proj[5] / g_last_proj[0] : 0;
    vr::begin_frame(ratio, g_eyes);
    if (aim) steer_aim();
    for (int eye = 0; eye < 2; ++eye) g_ctx->UpdateSubresource(g_eye_cb[eye], 0, nullptr, &g_eyes[eye], 0, 0);
    if (g_gv_cb) {
        vr::EyeConsts gv;
        vr::game_view(ratio, &gv);
        g_ctx->UpdateSubresource(g_gv_cb, 0, nullptr, &gv, 0, 0);
    }
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
}

static ID3D11Texture2D* texture_of(ID3D11View* view, D3D11_TEXTURE2D_DESC* desc) {
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    res->Release();  // the view keeps it alive
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    auto tex = static_cast<ID3D11Texture2D*>(res);
    tex->GetDesc(desc);
    return tex;
}

static ID3D11Texture2D* twin_tex(ID3D11Resource* res, bool create) {
    if (!res || res == g_backbuffer) return nullptr;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    auto it = g_twins.find(res);
    if (it != g_twins.end()) {
        D3D11_TEXTURE2D_DESC t;
        it->second->GetDesc(&t);
        if (t.Width == d.Width && t.Height == d.Height && t.Format == d.Format && t.ArraySize == d.ArraySize)
            return it->second;
        // The game freed this texture and made a different one at the same address.
        it->second->Release();
        g_twins.erase(it);
        for (auto& v : g_twin_views) v.second->Release();
        g_twin_views.clear();
    }
    // Only textures that can be sampled later need a per-eye copy (this also skips the backbuffer,
    // whose pointer isn't known until the first Present).
    if (!create || d.Usage != D3D11_USAGE_DEFAULT || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
    d.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                         D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
    ID3D11Texture2D* twin = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &twin))) return nullptr;
    logf("stereo: twin %ux%u f%d bind 0x%x", d.Width, d.Height, d.Format, d.BindFlags);
    g_twins[res] = twin;
    return twin;
}

// The same view on the right-eye twin of the view's texture. With create=false only existing
// twins are used (sampled textures that no eye ever rendered to stay shared).
template <typename V>
static V* twin_view(V* view, bool create) {
    auto it = g_twin_views.find(view);
    if (it != g_twin_views.end()) return static_cast<V*>(it->second);
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    res->Release();
    ID3D11Texture2D* twin = twin_tex(res, create);
    if (!twin) return nullptr;
    V* out = nullptr;
    HRESULT hr = E_FAIL;
    if constexpr (std::is_same_v<V, ID3D11RenderTargetView>) {
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        view->GetDesc(&vd);
        hr = g_dev->CreateRenderTargetView(twin, &vd, &out);
    } else if constexpr (std::is_same_v<V, ID3D11DepthStencilView>) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd;
        view->GetDesc(&vd);
        hr = g_dev->CreateDepthStencilView(twin, &vd, &out);
    } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        view->GetDesc(&vd);
        hr = g_dev->CreateShaderResourceView(twin, &vd, &out);
    }
    if (FAILED(hr)) return nullptr;
    g_twin_views[view] = out;
    return out;
}

// 1 = perspective GX draw, 2 = orthographic GX draw, 0 = other (post passes, fixed shaders).
static int draw_kind() {
    ID3D11Buffer* cb = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return 0;
    auto it = g_cb_kind.find(cb);
    cb->Release();
    return it == g_cb_kind.end() ? 0 : it->second;
}

static bool hud_target_like(ID3D11RenderTargetView* rtv) {
    D3D11_TEXTURE2D_DESC d;
    if (!texture_of(rtv, &d)) return false;
    if (g_hud_tex) {
        D3D11_TEXTURE2D_DESC have;
        g_hud_tex->GetDesc(&have);
        if (have.Width == d.Width && have.Height == d.Height && have.Format == d.Format) return true;
        g_hud_rtv->Release();
        g_hud_tex->Release();
        g_hud_tex = nullptr;
    }
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_hud_tex))) return false;
    if (FAILED(g_dev->CreateRenderTargetView(g_hud_tex, nullptr, &g_hud_rtv))) {
        g_hud_tex->Release();
        g_hud_tex = nullptr;
        return false;
    }
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_hud_rtv, clear);
    logf("stereo: HUD overlay %ux%u", d.Width, d.Height);
    return true;
}

// True when the bound pixel shader samples a texture an eye rendered (a full-screen effect over
// the scene, e.g. the dimmed frame copy behind a name card) rather than HUD artwork.
static bool samples_eye_image() {
    // Only the slots the pixel shader declares; other slots may hold stale bindings.
    uint32_t slots = 0xFFFFFFFF;
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) {
        UINT size = sizeof(slots);
        if (FAILED(ps->GetPrivateData(kTextureSlotsGuid, &size, &slots))) slots = 0xFFFFFFFF;
        ps->Release();
    }
    ID3D11ShaderResourceView* srv[kMaxTextures] = {};
    g_ctx->PSGetShaderResources(0, kMaxTextures, srv);
    bool eye = false;
    for (UINT i = 0; i < kMaxTextures; ++i) {
        ID3D11ShaderResourceView* s = srv[i];
        if (!s) continue;
        if (!eye && (slots & (1u << i))) {
            ID3D11Resource* res = nullptr;
            s->GetResource(&res);
            eye = twin_tex(res, false) != nullptr;
            res->Release();
        }
        s->Release();
    }
    return eye;
}

// HUD artwork (sprites, text) samples a texture; untextured full-screen quads are dims, fades
// and flashes that belong to the eyes.
static bool pixel_shader_textured() {
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return false;
    uint32_t slots = 0xFFFFFFFF;
    UINT size = sizeof(slots);
    if (FAILED(ps->GetPrivateData(kTextureSlotsGuid, &size, &slots))) slots = 0xFFFFFFFF;
    ps->Release();
    return slots != 0;
}

static bool is_card_shader() {
    if (g_card_ps.empty()) return false;
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    const uint64_t h = shader_hash(ps);
    if (ps) ps->Release();
    for (uint64_t c : g_card_ps)
        if (c == h) return true;
    return false;
}

// HUD draws render over a transparent overlay, but the game's blending doesn't write alpha.
// Same colour blending, and alpha = max(source alpha, existing) so the overlay gets coverage
// (colour ends up premultiplied, since the overlay starts black). Opaque draws write alpha 1.
static std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_hud_blend_states;

template <typename DrawFn>
static void draw_hud(DrawFn draw) {
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    g_ctx->OMGetBlendState(&bs, factor, &mask);
    ID3D11BlendState*& hud_bs = g_hud_blend_states[bs];
    if (!hud_bs) {
        D3D11_BLEND_DESC d{};
        d.RenderTarget[0] = {FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
                             D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        if (bs) bs->GetDesc(&d);
        auto& rt = d.RenderTarget[0];
        if (!rt.BlendEnable) {  // opaque: colour as before (src), alpha forced to 1 via the blend factor
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        } else {
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        }
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
        rt.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        d.IndependentBlendEnable = FALSE;
        g_dev->CreateBlendState(&d, &hud_bs);
    }
    const FLOAT one[4] = {1, 1, 1, 1};
    g_ctx->OMSetBlendState(hud_bs, one, mask);
    draw();
    g_ctx->OMSetBlendState(bs, factor, mask);
    if (bs) bs->Release();
}

// Draws once per scissor rectangle.
template <typename DrawFn>
static void draw_in_rects(const D3D11_RECT* rects, int n, DrawFn draw) {
    ID3D11RasterizerState* rs = nullptr;
    g_ctx->RSGetState(&rs);
    ID3D11RasterizerState*& scissor_rs = g_scissor_states[rs];
    if (!scissor_rs) {
        D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.f, 0.f, TRUE, FALSE, FALSE, FALSE};
        if (rs) rs->GetDesc(&rd);
        rd.ScissorEnable = TRUE;
        g_dev->CreateRasterizerState(&rd, &scissor_rs);
    }
    D3D11_RECT old[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nold = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_ctx->RSGetScissorRects(&nold, old);
    g_ctx->RSSetState(scissor_rs);
    for (int i = 0; i < n; ++i) {
        if (rects[i].right <= rects[i].left || rects[i].bottom <= rects[i].top) continue;
        g_ctx->RSSetScissorRects(1, &rects[i]);
        draw();
    }
    g_ctx->RSSetState(rs);
    g_ctx->RSSetScissorRects(nold, nold ? old : nullptr);
    if (rs) rs->Release();
}

// Draws with a scissor limited to where the game camera's (intended) frustum lands in `eye`.
template <typename DrawFn>
static void draw_scissored(int eye, DrawFn draw) {
    const float intended = fov_inflate_intended();
    const float ratio = g_last_proj[0] != 0 ? g_last_proj[5] / g_last_proj[0] : 0;
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    g_ctx->RSGetViewports(&nvp, &vp);
    if (intended <= 0 || ratio <= 0 || nvp == 0) {
        draw();
        return;
    }
    const float tan_y = tanf(intended * 3.14159265f / 360);
    int r[4];
    vr::frustum_scissor(g_eyes[eye].head, g_eyes[eye].px, g_eyes[eye].py, tan_y * ratio, tan_y, int(vp.Width),
                        int(vp.Height), r);
    if (r[2] <= r[0] || r[3] <= r[1]) return;  // the card's view is entirely outside this eye
    const D3D11_RECT rect{LONG(vp.TopLeftX) + r[0], LONG(vp.TopLeftY) + r[1], LONG(vp.TopLeftX) + r[2],
                          LONG(vp.TopLeftY) + r[3]};
    draw_in_rects(&rect, 1, draw);
}

// Aim mode: the HUD without its centre band, where the game's reticle is (centred, full height;
// the controller's laser replaces it). [vr] aim_hud_mask = band width as a fraction, 0 = keep.
template <typename DrawFn>
static void draw_hud_masked(DrawFn draw) {
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    g_ctx->RSGetViewports(&nvp, &vp);
    if (!g_aim_mode || g_aim_hud_mask <= 0 || nvp == 0) {
        draw();
        return;
    }
    const LONG x0 = LONG(vp.TopLeftX), y0 = LONG(vp.TopLeftY), y1 = LONG(vp.TopLeftY + vp.Height);
    const LONG band_l = LONG(vp.TopLeftX + vp.Width * (1 - g_aim_hud_mask) / 2);
    const LONG band_r = LONG(vp.TopLeftX + vp.Width * (1 + g_aim_hud_mask) / 2);
    const D3D11_RECT rects[2] = {{x0, y0, band_l, y1}, {band_r, y0, LONG(vp.TopLeftX + vp.Width), y1}};
    draw_in_rects(rects, 2, draw);
}

static void note_final_source() {
    ID3D11ShaderResourceView* srv = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    res->Release();  // the game keeps it alive
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    g_final[0] = static_cast<ID3D11Texture2D*>(res);
    g_final[1] = twin_tex(res, false);
}

// ---- Eye-resolution render targets ----
// In VR the game's 4:3 render and depth targets are created at the headset's eye resolution
// (vr::eye_resolution); draws into them get the game's viewports and scissors scaled to match.

static std::unordered_map<ID3D11Resource*, std::pair<float, float>> g_resized;  // texture -> (sx, sy)
static UINT g_orig_w, g_orig_h;  // the size the game asked for (its 4:3 targets)

using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t o_CreateTexture2D;

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
                                                    const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    unsigned w = 0, h = 0;
    // Not textures the CPU reads (the game's frame copies for its readbacks keep their size).
    if (d == g_dev && desc && !init && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
        !desc->CPUAccessFlags && desc->Width * 3 == desc->Height * 4 && desc->Height >= 480 &&
        vr::eye_resolution(&w, &h)) {
        D3D11_TEXTURE2D_DESC nd = *desc;
        nd.Width = w;
        nd.Height = h;
        HRESULT hr = o_CreateTexture2D(d, &nd, init, out);
        if (SUCCEEDED(hr) && out && *out) {
            g_resized[*out] = {float(w) / float(desc->Width), float(h) / float(desc->Height)};
            g_orig_w = desc->Width;
            g_orig_h = desc->Height;
            vr::set_content_aspect(float(desc->Width) / float(desc->Height));
            logf("eye targets: %ux%u f%d created at %ux%u", desc->Width, desc->Height, desc->Format, w, h);
        }
        return hr;
    }
    // (The game makes its readback staging textures from the resized targets' descriptions and
    // sizes its CPU reads from them, so those stay resized; the game-view targets match them.)
    return o_CreateTexture2D(d, desc, init, out);
}

// Scales the bound viewports and scissors to a resized render target for one draw.
struct ViewportScale {
    bool active = false;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];  // the game's
    D3D11_RECT sr[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT nsr = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    // Back to the game's own viewports (game-view pass) / to the scaled ones again.
    void unscale() const {
        if (!active) return;
        if (nvp) g_ctx->RSSetViewports(nvp, vp);
        if (nsr) g_ctx->RSSetScissorRects(nsr, sr);
    }
    void rescale() const {
        if (!active) return;
        if (nvp) g_ctx->RSSetViewports(nvp, svp);
        if (nsr) g_ctx->RSSetScissorRects(nsr, ssr);
    }
    D3D11_VIEWPORT svp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];  // scaled
    D3D11_RECT ssr[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

    explicit ViewportScale(ID3D11Resource* rt) {
        if (g_resized.empty() || !rt) return;
        auto it = g_resized.find(rt);
        if (it == g_resized.end()) return;
        const float sx = it->second.first, sy = it->second.second;
        g_ctx->RSGetViewports(&nvp, vp);
        g_ctx->RSGetScissorRects(&nsr, sr);
        for (UINT i = 0; i < nvp; ++i)
            svp[i] = {vp[i].TopLeftX * sx, vp[i].TopLeftY * sy, vp[i].Width * sx, vp[i].Height * sy, vp[i].MinDepth,
                      vp[i].MaxDepth};
        for (UINT i = 0; i < nsr; ++i)
            ssr[i] = {LONG(sr[i].left * sx), LONG(sr[i].top * sy), LONG(sr[i].right * sx), LONG(sr[i].bottom * sy)};
        if (nvp) g_ctx->RSSetViewports(nvp, svp);
        if (nsr) g_ctx->RSSetScissorRects(nsr, ssr);
        active = true;
    }
    ~ViewportScale() {
        if (!active) return;
        if (nvp) g_ctx->RSSetViewports(nvp, vp);
        if (nsr) g_ctx->RSSetScissorRects(nsr, sr);
    }
};

// ---- Game view ----
// The game reads its own frame back on the CPU (the port's emulation of GX "EFB peeks": hit
// tests among others) from copies it makes of the scene colour and depth targets. In stereo
// those targets hold eye images (head-tracked, headset projection, resized), so every scene draw
// is also drawn from the game camera with its own projection and viewports at the original size
// into game-view targets, and the game's copies out of the scene targets read from those.

static std::unordered_map<ID3D11Resource*, ID3D11Texture2D*> g_gv_tex;  // scene texture -> game view
static std::unordered_map<ID3D11View*, ID3D11View*> g_gv_views;

static ID3D11Texture2D* gv_tex(ID3D11Resource* res, bool create) {
    auto it = g_gv_tex.find(res);
    if (it != g_gv_tex.end()) return it->second;
    if (!create || !res) return nullptr;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    D3D11_TEXTURE2D_DESC d;  // same size as the (possibly resized) scene target, like the game's copies
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    d.MiscFlags &= ~UINT(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                         D3D11_RESOURCE_MISC_GDI_COMPATIBLE);
    ID3D11Texture2D* t = nullptr;
    if (FAILED(o_CreateTexture2D(g_dev, &d, nullptr, &t))) return nullptr;
    logf("game view: %ux%u f%d", d.Width, d.Height, d.Format);
    g_gv_tex[res] = t;
    return t;
}

template <typename V>
static V* gv_view(V* view, bool create) {
    auto it = g_gv_views.find(view);
    if (it != g_gv_views.end()) return static_cast<V*>(it->second);
    if (!create) return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    res->Release();
    ID3D11Texture2D* t = gv_tex(res, true);
    if (!t) return nullptr;
    V* out = nullptr;
    HRESULT hr;
    if constexpr (std::is_same_v<V, ID3D11RenderTargetView>) {
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        view->GetDesc(&vd);
        hr = g_dev->CreateRenderTargetView(t, &vd, &out);
    } else {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd;
        view->GetDesc(&vd);
        hr = g_dev->CreateDepthStencilView(t, &vd, &out);
    }
    if (FAILED(hr)) return nullptr;
    g_gv_views[view] = out;
    return out;
}

// One scene draw into the game-view targets, from the game camera at the game's viewports.
template <typename DrawFn>
static void draw_game_view(DrawFn draw, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                           const ViewportScale& scale) {
    if (!g_gv_cb) return;
    ID3D11RenderTargetView* grtv[kMaxTargets] = {};
    for (UINT i = 0; i < kMaxTargets; ++i)
        if (rtv[i] && !(grtv[i] = gv_view(rtv[i], true))) return;
    ID3D11DepthStencilView* gdsv = dsv ? gv_view(dsv, true) : nullptr;
    if (dsv && !gdsv) return;
    (void)scale;  // same size as the eye targets: the scaled viewports apply as they are
    g_ctx->OMSetRenderTargets(kMaxTargets, grtv, gdsv);
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_gv_cb);
    draw();
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
}

static bool points_or_lines() {
    D3D11_PRIMITIVE_TOPOLOGY t;
    g_ctx->IAGetPrimitiveTopology(&t);
    return t == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST || t == D3D11_PRIMITIVE_TOPOLOGY_LINELIST ||
           t == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
}

// Runs `draw` for the left eye, then for the right eye on twins (see the section comment).
template <typename DrawFn>
static void draw_stereo(DrawFn draw) {
    if (!vr::stereo() && g_resized.empty()) {
        draw();
        return;
    }
    ID3D11RenderTargetView* rtv[kMaxTargets] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_ctx->OMGetRenderTargets(kMaxTargets, rtv, &dsv);
    auto release_targets = [&] {
        for (auto v : rtv)
            if (v) v->Release();
        if (dsv) dsv->Release();
    };
    ID3D11Resource* rt0 = nullptr;
    if (rtv[0]) {
        rtv[0]->GetResource(&rt0);
        rt0->Release();
    }
    ViewportScale scale(rt0);
    if (!vr::stereo()) {  // stereo switched off at runtime with resized targets: just scale
        draw();
        release_targets();
        return;
    }
    if (!rtv[0] || rt0 == g_backbuffer) {  // the blit to the window: once
        if (rt0) note_final_source();
        draw();
        release_targets();
        return;
    }

    const int kind = draw_kind();
    if (kind == 1 && g_aim_mode && !g_relocked) relock_aim();
    bool scene = false;
    if (dsv) {
        D3D11_TEXTURE2D_DESC rd, dd;
        scene = texture_of(rtv[0], &rd) && texture_of(dsv, &dd) && rd.Width == dd.Width && rd.Height == dd.Height;
    }
    if (scene && kind == 1) {
        g_frame_3d = true;
        D3D11_TEXTURE2D_DESC sd;
        g_scene_tex = texture_of(rtv[0], &sd);
    }
    const bool eye_image = scene && kind == 2 && samples_eye_image();
    if (eye_image && !g_frame_3d) {
        // A screen-fixed backdrop made from last frame's image (drawn before the 3D scene, which
        // normally covers it). With head tracking it shows wherever geometry doesn't: skip it in
        // the eyes (the game's own view keeps it).
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }
    // HUD: textured 2D artwork and point/line overlays (sparkles, reticle lines) after the 3D
    // scene. Untextured triangle quads (dims, fades, clears) and screen-image effects stay per eye.
    if (scene && kind == 2 && g_frame_3d && !eye_image && (pixel_shader_textured() || points_or_lines()) &&
        hud_target_like(rtv[0])) {  // once, into the overlay
        // The laser goes in after the whole scene (it doesn't write depth, so later draws would
        // cover it) and before the HUD.
        if (g_aim_mode) draw_laser(rtv[0], dsv);
        g_ctx->OMSetRenderTargets(1, &g_hud_rtv, nullptr);
        draw_hud([&] { draw_hud_masked(draw); });
        g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }
    // In aim mode the first run of 3D draws (same pixel shader) after the clear is the
    // first-person arm and gun.
    bool gun = false;
    if (g_aim_mode && scene && kind == 1 && g_gun_run < 2) {
        ID3D11PixelShader* ps = nullptr;
        g_ctx->PSGetShader(&ps, nullptr, nullptr);
        const uint64_t h = shader_hash(ps);
        if (ps) ps->Release();
        if (g_gun_run == 0) g_gun_run = 1, g_gun_ps = h;
        if (h == g_gun_ps)
            gun = true;
        else
            g_gun_run = 2;
    }
    if (gun && g_draw_cb[1]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[1]);
    const bool card = kind == 1 && is_card_shader();
    if (card && g_card_skip) {  // card_mode=skip: view-dependent cards are left out of the eyes
        if (gun && g_draw_cb[0]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }

    // Left eye: as issued.
    if (card)
        draw_scissored(0, draw);
    else
        draw();

    // Right eye: twins for targets, depth and any sampled texture an eye rendered.
    ID3D11RenderTargetView* trtv[kMaxTargets] = {};
    bool ok = true;
    for (UINT i = 0; i < kMaxTargets; ++i)
        if (rtv[i] && !(trtv[i] = twin_view(rtv[i], true))) ok = false;
    ID3D11DepthStencilView* tdsv = dsv ? twin_view(dsv, true) : nullptr;
    if (dsv && !tdsv) ok = false;
    if (ok) {
        ID3D11ShaderResourceView* srv[kMaxTextures] = {};
        ID3D11ShaderResourceView* tsrv[kMaxTextures] = {};
        g_ctx->PSGetShaderResources(0, kMaxTextures, srv);
        bool swapped = false;
        for (UINT i = 0; i < kMaxTextures; ++i) {
            tsrv[i] = srv[i];
            if (srv[i])
                if (auto t = twin_view(srv[i], false)) tsrv[i] = t, swapped = true;
        }
        g_ctx->OMSetRenderTargets(kMaxTargets, trtv, tdsv);
        if (swapped) g_ctx->PSSetShaderResources(0, kMaxTextures, tsrv);
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[1]);
        if (card)
            draw_scissored(1, draw);
        else
            draw();
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
        if (swapped) g_ctx->PSSetShaderResources(0, kMaxTextures, srv);
        g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
        for (auto s : srv)
            if (s) s->Release();
    }
    if (gun && g_draw_cb[0]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
    if (scene) draw_game_view(draw, rtv, dsv, scale);
    release_targets();
}

// Clears and copies of twinned textures happen to the twins too.
static void mirror_clear_rtv(ID3D11RenderTargetView* rtv, const FLOAT color[4]) {
    if (!vr::stereo() || !rtv) return;
    if (auto t = twin_view(rtv, false)) g_ctx->ClearRenderTargetView(t, color);
    if (auto g = gv_view(rtv, false)) g_ctx->ClearRenderTargetView(g, color);
}

static void mirror_clear_dsv(ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil) {
    if (!vr::stereo() || !dsv) return;
    if (auto t = twin_view(dsv, false)) g_ctx->ClearDepthStencilView(t, flags, depth, stencil);
    if (auto g = gv_view(dsv, false)) g_ctx->ClearDepthStencilView(g, flags, depth, stencil);
}

static void mirror_copy(ID3D11Resource* dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ssub,
                        const D3D11_BOX* box, bool whole) {
    if (!vr::stereo()) return;
    ID3D11Texture2D* tsrc = twin_tex(src, false);
    ID3D11Texture2D* tdst = twin_tex(dst, tsrc != nullptr);  // copying an eye's image makes the target per-eye
    if (!tdst) return;
    ID3D11Resource* from = tsrc ? static_cast<ID3D11Resource*>(tsrc) : src;
    if (whole)
        g_ctx->CopyResource(tdst, from);
    else
        g_ctx->CopySubresourceRegion(tdst, dsub, x, y, z, from, ssub, box);
}

static void load_card_list() {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"vr", L"cards", L"d358d9e91e34870e", buf, 512, path_in(L"k7vr.ini").c_str());
    for (wchar_t* p = buf; *p;) {
        wchar_t* end = nullptr;
        const uint64_t h = _wcstoui64(p, &end, 16);
        if (end == p) break;
        g_card_ps.push_back(h);
        p = end;
        while (*p == L',' || *p == L' ') ++p;
    }
    wchar_t mode[16];
    GetPrivateProfileStringW(L"vr", L"card_mode", L"scissor", mode, 16, path_in(L"k7vr.ini").c_str());
    g_card_skip = _wcsicmp(mode, L"skip") == 0;
    logf("stereo: %zu effect-card shader(s), %s", g_card_ps.size(), g_card_skip ? "skipped" : "scissored");
}

static void grab_final_source() {
    ID3D11RenderTargetView* rtv = nullptr;
    g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* rt = nullptr;
    rtv->GetResource(&rt);
    rtv->Release();
    bool to_window = rt == g_backbuffer;
    rt->Release();
    if (!to_window) return;
    ID3D11ShaderResourceView* srv = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        auto tex = static_cast<ID3D11Texture2D*>(res);
        D3D11_TEXTURE2D_DESC d, mine;
        tex->GetDesc(&d);
        if (g_image) {
            g_image->GetDesc(&mine);
            if (mine.Width != d.Width || mine.Height != d.Height || mine.Format != d.Format) {
                g_image->Release();
                g_image = nullptr;
            }
        }
        if (!g_image && d.SampleDesc.Count == 1) {
            D3D11_TEXTURE2D_DESC nd = d;
            nd.MipLevels = 1;
            nd.ArraySize = 1;
            nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = 0;
            nd.CPUAccessFlags = 0;
            nd.MiscFlags = 0;
            g_dev->CreateTexture2D(&nd, nullptr, &g_image);
        }
        if (g_image) {
            g_ctx->CopySubresourceRegion(g_image, 0, 0, 0, 0, tex, 0, nullptr);
            g_image_valid = true;
        }
    }
    res->Release();
}

static void before_draw(const char* kind, UINT count, UINT instances) {
    if (g_cap.active) capture_draw(g_ctx, kind, count, instances);
    if (g_want_image) grab_final_source();
    ++g_draws;
}

// `capture N...`: the render target after draw N, as capture_NNN_dNNNN.png.
static std::vector<unsigned> g_snaps;
void set_capture_snaps(std::vector<unsigned> draws) { g_snaps = std::move(draws); }
static void save_texture(ID3D11Texture2D* tex, const std::wstring& path, bool alpha_only);

static void after_draw() {
    if (!g_cap.active || g_snaps.empty()) return;
    const unsigned n = g_draws - 1;
    if (std::find(g_snaps.begin(), g_snaps.end(), n) == g_snaps.end()) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        wchar_t name[64];
        swprintf(name, 64, L"capture_%03u_d%04u.png", g_cap.index, n);
        save_texture(static_cast<ID3D11Texture2D*>(res), path_in(name), false);
    }
    res->Release();
    rtv->Release();
    // ...and the textures it sampled (formats save_texture can't convert are skipped).
    ID3D11ShaderResourceView* srv[kCaptureTextures] = {};
    g_ctx->PSGetShaderResources(0, kCaptureTextures, srv);
    for (UINT i = 0; i < kCaptureTextures; ++i) {
        if (!srv[i]) continue;
        D3D11_TEXTURE2D_DESC d;
        if (ID3D11Texture2D* tex = texture_of(srv[i], &d)) {
            wchar_t name[64];
            swprintf(name, 64, L"capture_%03u_d%04u_t%u.png", g_cap.index, n, i);
            save_texture(tex, path_in(name), false);
        }
        srv[i]->Release();
    }
}

// ---- Context hooks (installed in the stand-in's table; `c` is the stand-in) ----

static void STDMETHODCALLTYPE w_DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    before_draw("DrawIndexed", count, 1);
    draw_stereo([&] { real_of(c)->DrawIndexed(count, start, base); });
    after_draw();
}

static void STDMETHODCALLTYPE w_Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
    before_draw("Draw", count, 1);
    draw_stereo([&] { real_of(c)->Draw(count, start); });
    after_draw();
}

static void STDMETHODCALLTYPE w_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start,
                                                     INT base, UINT start_inst) {
    before_draw("DrawIndexedInstanced", count, inst);
    draw_stereo([&] { real_of(c)->DrawIndexedInstanced(count, inst, start, base, start_inst); });
    after_draw();
}

static void STDMETHODCALLTYPE w_DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start,
                                              UINT start_inst) {
    before_draw("DrawInstanced", count, inst);
    draw_stereo([&] { real_of(c)->DrawInstanced(count, inst, start, start_inst); });
    after_draw();
}

static HRESULT STDMETHODCALLTYPE w_Map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
                                       UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    HRESULT hr = real_of(c)->Map(res, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && type != D3D11_MAP_READ && constant_buffer_size(res)) g_cb_mapped[res] = out->pData;
    if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) {  // the game reading GPU data back
        static int logged;
        if (logged++ < 40) {
            std::string s;
            describe_resource(s, "res", res);
            logf("readback: Map %s by exe+%x", s.c_str(),
                 unsigned(uintptr_t(_ReturnAddress()) - uintptr_t(GetModuleHandleW(nullptr))));
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE w_Unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    auto it = g_cb_mapped.find(res);
    if (it != g_cb_mapped.end()) {
        UINT size = constant_buffer_size(res);
        auto src = static_cast<const uint8_t*>(it->second);
        g_cb[res].assign(src, src + size);
        g_cb_mapped.erase(it);
        if (size >= kTemplateCbMin) {
            auto p = reinterpret_cast<const float*>(src + kProjOffset);
            const bool persp = p[12] == 0 && p[13] == 0 && p[14] == -1 && p[15] == 0;
            if (persp) memcpy(g_last_proj, p, sizeof(g_last_proj));
            g_cb_kind[res] = persp ? 1 : (p[15] == 1 ? 2 : 0);
        }
    }
    real_of(c)->Unmap(res, sub);
}

static void STDMETHODCALLTYPE w_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub,
                                                  const D3D11_BOX* box, const void* src, UINT row, UINT depth) {
    if (src) {
        if (UINT size = constant_buffer_size(dst)) {
            auto& shadow = g_cb[dst];
            shadow.resize(size);
            UINT left = box ? box->left : 0;
            UINT right = box ? box->right : size;
            if (right > size) right = size;
            if (left < right) memcpy(shadow.data() + left, src, right - left);
            if (size >= kTemplateCbMin && left == 0 && right >= kTemplateCbMin) {
                auto p = reinterpret_cast<const float*>(shadow.data() + kProjOffset);
                const bool persp = p[12] == 0 && p[13] == 0 && p[14] == -1 && p[15] == 0;
                if (persp) memcpy(g_last_proj, p, sizeof(g_last_proj));
                g_cb_kind[dst] = persp ? 1 : (p[15] == 1 ? 2 : 0);
            }
        } else if (g_cap.active) {
            appendf(g_cap.text, "     UpdateSubresource");
            describe_resource(g_cap.text, "dst", dst);
            g_cap.text += '\n';
        }
    }
    real_of(c)->UpdateSubresource(dst, sub, box, src, row, depth);
}

static void STDMETHODCALLTYPE w_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, const FLOAT color[4]) {
    if (g_cap.active) {
        appendf(g_cap.text, "     ClearRTV");
        describe_view(g_cap.text, "rt", rtv);
        appendf(g_cap.text, " color=%g,%g,%g,%g\n", color[0], color[1], color[2], color[3]);
    }
    real_of(c)->ClearRenderTargetView(rtv, color);
    mirror_clear_rtv(rtv, color);
}

static void STDMETHODCALLTYPE w_ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT d,
                                         UINT8 st) {
    if (g_cap.active) {
        appendf(g_cap.text, "     ClearDSV");
        describe_view(g_cap.text, "ds", dsv);
        appendf(g_cap.text, " flags=%u depth=%g\n", flags, d);
    }
    real_of(c)->ClearDepthStencilView(dsv, flags, d, st);
    mirror_clear_dsv(dsv, flags, d, st);
}

static void STDMETHODCALLTYPE w_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dsub, UINT x,
                                                      UINT y, UINT z, ID3D11Resource* src, UINT ssub,
                                                      const D3D11_BOX* box) {
    if (g_cap.active) {
        appendf(g_cap.text, "     CopySubresourceRegion");
        describe_resource(g_cap.text, "src", src);
        describe_resource(g_cap.text, "dst", dst);
        if (box) appendf(g_cap.text, " box=%u,%u-%u,%u", box->left, box->top, box->right, box->bottom);
        appendf(g_cap.text, " at=%u,%u\n", x, y);
    }
    if (box) {  // regional copies (readback candidates): log a few with their source
        static int logged;
        if (logged++ < 40) {
            std::string s;
            describe_resource(s, "src", src);
            describe_resource(s, "dst", dst);
            logf("readback: CopySubresourceRegion%s box=%u,%u-%u,%u at=%u,%u by exe+%x", s.c_str(), box->left,
                 box->top, box->right, box->bottom, x, y,
                 unsigned(uintptr_t(_ReturnAddress()) - uintptr_t(GetModuleHandleW(nullptr))));
        }
    }
    if (ID3D11Texture2D* g = vr::stereo() && !g_resized.count(dst) ? gv_tex(src, false) : nullptr) {
        real_of(c)->CopySubresourceRegion(dst, dsub, x, y, z, g, ssub, box);  // the game's view, for readbacks
        return;
    }
    real_of(c)->CopySubresourceRegion(dst, dsub, x, y, z, src, ssub, box);
    mirror_copy(dst, dsub, x, y, z, src, ssub, box, false);
}

static void STDMETHODCALLTYPE w_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_cap.active) {
        appendf(g_cap.text, "     CopyResource");
        describe_resource(g_cap.text, "src", src);
        describe_resource(g_cap.text, "dst", dst);
        g_cap.text += '\n';
    }
    if (ID3D11Texture2D* g = vr::stereo() && !g_resized.count(dst) ? gv_tex(src, false) : nullptr) {
        real_of(c)->CopyResource(dst, g);  // the game's view, for readbacks
        return;
    }
    real_of(c)->CopyResource(dst, src);
    mirror_copy(dst, 0, 0, 0, 0, src, 0, nullptr, true);
}

static void STDMETHODCALLTYPE w_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore) {
    static bool logged;
    if (!logged) {
        logged = true;
        logf("game executes command lists (deferred contexts) - their draws are not captured");
    }
    real_of(c)->ExecuteCommandList(list, restore);
}

// ---- Device hooks ----

using CreateVS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11VertexShader**);
using CreateGS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11GeometryShader**);
using CreatePS_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11PixelShader**);
using CreateDeferredContext_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using GetImmediateContext_t = void(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11DeviceContext**);
static CreateVS_t o_CreateVS;
static CreateGS_t o_CreateGS;
static CreatePS_t o_CreatePS;
static CreateDeferredContext_t o_CreateDeferredContext;
static GetImmediateContext_t o_GetImmediateContext;
static unsigned g_shader_count;

// Bit i set when the shader declares a texture at register t<i> (from the DXBC RDEF chunk).
static uint32_t texture_slots(const void* bc, SIZE_T len) {
    auto b = static_cast<const uint8_t*>(bc);
    auto u32 = [&](size_t off) {
        uint32_t v = 0;
        if (off + 4 <= len) memcpy(&v, b + off, 4);
        return v;
    };
    if (len < 32 || memcmp(b, "DXBC", 4) != 0) return 0xFFFFFFFF;
    const uint32_t chunks = u32(28);
    for (uint32_t c = 0; c < chunks; ++c) {
        const uint32_t off = u32(32 + 4 * c);
        if (memcmp(b + off, "RDEF", 4) != 0) continue;
        const size_t data = off + 8;
        const uint32_t count = u32(data + 8), table = u32(data + 12);
        uint32_t mask = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = data + table + 32 * i;  // D3D11_SHADER_INPUT_BIND_DESC layout in the chunk
            const uint32_t type = u32(e + 4), point = u32(e + 20), n = u32(e + 24);
            if (type == 2 /* D3D_SIT_TEXTURE */)
                for (uint32_t k = 0; k < n && point + k < 32; ++k) mask |= 1u << (point + k);
        }
        return mask;
    }
    return 0xFFFFFFFF;  // no reflection data: assume every slot
}

static void tag_shader(ID3D11DeviceChild* shader, const void* bc, SIZE_T len) {
    uint64_t h = dxbc_hash(bc, len);
    shader->SetPrivateData(kShaderHashGuid, sizeof(h), &h);
    const uint32_t slots = texture_slots(bc, len);
    shader->SetPrivateData(kTextureSlotsGuid, sizeof(slots), &slots);
    ++g_shader_count;
}

static HRESULT STDMETHODCALLTYPE hk_CreateVS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l,
                                             ID3D11VertexShader** out) {
    HRESULT hr = o_CreateVS(d, bc, len, l, out);
    if (SUCCEEDED(hr) && out && *out) tag_shader(*out, bc, len);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateGS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l,
                                             ID3D11GeometryShader** out) {
    HRESULT hr = o_CreateGS(d, bc, len, l, out);
    if (SUCCEEDED(hr) && out && *out) tag_shader(*out, bc, len);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreatePS(ID3D11Device* d, const void* bc, SIZE_T len, ID3D11ClassLinkage* l,
                                             ID3D11PixelShader** out) {
    HRESULT hr = o_CreatePS(d, bc, len, l, out);
    if (SUCCEEDED(hr) && out && *out) tag_shader(*out, bc, len);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateDeferredContext(ID3D11Device* d, UINT flags, ID3D11DeviceContext** out) {
    HRESULT hr = o_CreateDeferredContext(d, flags, out);
    if (d == g_dev) logf("game created a deferred context -> 0x%08x", hr);
    return hr;
}

static void STDMETHODCALLTYPE hk_GetImmediateContext(ID3D11Device* d, ID3D11DeviceContext** out) {
    o_GetImmediateContext(d, out);
    if (out && *out == g_ctx && g_wrapped) *out = g_wrapped;  // the real context holds the added reference
}

// ---- Swap chain: frame boundary, captures, screenshots, status ----

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static Present_t o_Present;
static Present1_t o_Present1;

// alpha_only: write the alpha channel as greyscale (RGBA8 textures), to check overlay transparency.
static void save_texture(ID3D11Texture2D* tex, const std::wstring& path, bool alpha_only = false) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    ID3D11Texture2D* src = tex;
    ID3D11Texture2D* resolved = nullptr;
    if (d.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = d;
        rd.SampleDesc = {1, 0};
        rd.BindFlags = 0;
        if (SUCCEEDED(g_dev->CreateTexture2D(&rd, nullptr, &resolved))) {
            g_ctx->ResolveSubresource(resolved, 0, tex, 0, d.Format);
            src = resolved;
        }
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &staging))) {
        g_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4);
            bool ok = true;
            for (UINT y = 0; y < d.Height && ok; ++y) {
                auto row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                auto dst = px.data() + size_t(y) * d.Width * 4;
                for (UINT x = 0; x < d.Width; ++x) {
                    switch (d.Format) {
                        case DXGI_FORMAT_B8G8R8A8_UNORM:
                        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                        case DXGI_FORMAT_B8G8R8X8_UNORM:
                            memcpy(dst + x * 4, row + x * 4, 4);
                            break;
                        case DXGI_FORMAT_R8G8B8A8_UNORM:
                        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                            if (alpha_only) {
                                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x * 4 + 3];
                                break;
                            }
                            dst[x * 4 + 0] = row[x * 4 + 2];
                            dst[x * 4 + 1] = row[x * 4 + 1];
                            dst[x * 4 + 2] = row[x * 4 + 0];
                            break;
                        case DXGI_FORMAT_R10G10B10A2_UNORM: {
                            uint32_t v;
                            memcpy(&v, row + x * 4, 4);
                            dst[x * 4 + 2] = uint8_t((v & 1023) >> 2);
                            dst[x * 4 + 1] = uint8_t(((v >> 10) & 1023) >> 2);
                            dst[x * 4 + 0] = uint8_t(((v >> 20) & 1023) >> 2);
                            break;
                        }
                        default:
                            ok = false;
                    }
                    dst[x * 4 + 3] = 255;
                }
            }
            g_ctx->Unmap(staging, 0);
            if (ok)
                save_png(path, d.Width, d.Height, d.Width * 4, px.data());
            else
                logf("screenshot: format %d not handled", d.Format);
        }
        staging->Release();
    }
    if (resolved) resolved->Release();
}

// The frame image: the final blit's source if one was seen this frame, else the backbuffer.
static void save_frame_image(IDXGISwapChain* sc, const std::wstring& path) {
    if (g_image_valid) {
        save_texture(g_image, path);
        return;
    }
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
        save_texture(bb, path);
        bb->Release();
    }
}

static void write_status(IDXGISwapChain* sc) {
    static ULONGLONG last;
    static unsigned last_frame;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    double fps = last ? (g_frame - last_frame) * 1000.0 / double(now - last) : 0;
    last = now;
    last_frame = g_frame;
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    set_game_window(d.OutputWindow);
    FILE* f = _wfopen(path_in(L"status.txt").c_str(), L"w");
    if (!f) return;
    fprintf(f, "frame=%u\nfps=%.1f\ndraws=%u\nshaders=%u\nbackbuffer=%ux%u f%d\nwindowed=%d\nminimized=%d\n", g_frame,
            fps, g_draws_last, g_shader_count, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format,
            d.Windowed, IsIconic(d.OutputWindow) ? 1 : 0);
    fclose(f);
}

// While minimized there is no vsync and the game spins at ~600 fps; hold it to 60.
static void limit_background_rate(IDXGISwapChain* sc) {
    static LARGE_INTEGER freq, last;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (IsIconic(d.OutputWindow) && last.QuadPart) {
        const LONGLONG period = freq.QuadPart / 60;
        LONGLONG elapsed = now.QuadPart - last.QuadPart;
        if (elapsed < period) {
            DWORD ms = DWORD((period - elapsed) * 1000 / freq.QuadPart);
            if (ms > 1) Sleep(ms - 1);
            do QueryPerformanceCounter(&now);
            while (now.QuadPart - last.QuadPart < period);
        }
    }
    last = now;
}

static void end_frame(IDXGISwapChain* sc) {
    ++g_frame;
    g_draws_last = g_draws;
    g_draws = 0;

    if (g_cap.active) {
        g_cap.active = false;
        wchar_t name[64];
        swprintf(name, 64, L"capture_%03u.txt", g_cap.index);
        if (FILE* f = _wfopen(path_in(name).c_str(), L"wb")) {
            fwrite(g_cap.text.data(), 1, g_cap.text.size(), f);
            fclose(f);
        }
        swprintf(name, 64, L"capture_%03u.png", g_cap.index);
        save_frame_image(sc, path_in(name));
        logf("capture %u written (%u draws)", g_cap.index, g_draws_last);
        ++g_cap.index;
    }
    if (g_cap.shot) {
        g_cap.shot = false;
        static unsigned shot_index;
        wchar_t name[64];
        swprintf(name, 64, L"shot_%03u.png", shot_index++);
        save_frame_image(sc, path_in(name));
        logf("screenshot %ls", name);
    }
    // Hand the frame just drawn to the headset. 3D frames: each eye's final image and the HUD.
    // 2D frames (no perspective draw: menus, FMVs, loading): black eyes, the mono image on a screen.
    if (vr::stereo()) {
        ID3D11Texture2D* left = nullptr;
        ID3D11Texture2D* right = nullptr;
        ID3D11Texture2D* screen = nullptr;
        if (g_frame_3d && g_final[0]) {
            left = g_final[0];
            right = g_final[1] ? g_final[1] : g_final[0];
        } else {
            screen = g_final[0];
        }
        ID3D11Texture2D* hud = g_frame_3d ? g_hud_tex : nullptr;
        vr::present(left, right, hud, screen);
        if (g_pair) {
            g_pair = false;
            wchar_t name[64];
            if (left) {  // as sent to the headset (de-striped); scene_L/R below are the game's targets
                swprintf(name, 64, L"pair_%03u_L.png", g_pair_index);
                save_texture(vr::submitted(0) ? vr::submitted(0) : left, path_in(name));
                swprintf(name, 64, L"pair_%03u_R.png", g_pair_index);
                save_texture(vr::submitted(1) ? vr::submitted(1) : right, path_in(name));
            }
            if (g_scene_tex && left) {
                swprintf(name, 64, L"pair_%03u_scene_L.png", g_pair_index);
                save_texture(g_scene_tex, path_in(name));
                if (ID3D11Texture2D* t = twin_tex(g_scene_tex, false)) {
                    swprintf(name, 64, L"pair_%03u_scene_R.png", g_pair_index);
                    save_texture(t, path_in(name));
                }
            }
            if (hud) {
                swprintf(name, 64, L"pair_%03u_hud.png", g_pair_index);
                save_texture(hud, path_in(name));
                swprintf(name, 64, L"pair_%03u_hud_alpha.png", g_pair_index);
                save_texture(hud, path_in(name), true);
            }
            if (screen) {
                swprintf(name, 64, L"pair_%03u_screen.png", g_pair_index);
                save_texture(vr::submitted(2) ? vr::submitted(2) : screen, path_in(name));
            }
            logf("pair %u written (%s frame%s)", g_pair_index++, g_frame_3d ? "3D" : "2D",
                 g_final[1] || !left ? "" : ", right eye had no twin");
        }
        g_frame_3d = false;
        g_final[0] = g_final[1] = nullptr;
        g_scene_tex = nullptr;
        if (g_hud_rtv) {
            const float clear[4] = {0, 0, 0, 0};
            g_ctx->ClearRenderTargetView(g_hud_rtv, clear);
        }
    } else if (g_pair) {
        g_pair = false;
        logf("pair: stereo is off");
    }

    Requests req = control_poll();
    static bool f10_down;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !f10_down) req.capture = true;
    f10_down = f10;
    if (req.shot) g_cap.shot = true;
    if (req.pair) g_pair = true;
    if (req.view) {
        vr::set_sim_view(req.yaw, req.pitch);
        logf("view yaw %g pitch %g", req.yaw, req.pitch);
    }
    if (req.stereo >= 0) {  // stereo IPD_M: simulated headset with that eye separation; 0 = off
        if (req.stereo > 0) vr::set_ipd(req.stereo);
        vr::set_mode(req.stereo > 0 ? vr::Mode::Sim : vr::Mode::Off);
    }
    if (req.capture) {
        g_ids.clear();
        g_cap.active = true;
        g_cap.text.clear();
        appendf(g_cap.text, "# capture %u, frame %u\n", g_cap.index, g_frame);
    }

    // Set up the next frame's image grab.
    g_want_image = g_cap.shot || g_cap.active;
    set_eye_offsets();
    g_image_valid = false;
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
        g_backbuffer = bb;
        bb->Release();  // the swap chain keeps it alive; we only compare the pointer
    }

    write_status(sc);
    if (vr::mode() != vr::Mode::OpenVR) limit_background_rate(sc);  // the compositor paces VR
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) end_frame(sc);
    if (vr::mode() == vr::Mode::OpenVR) sync = 0;  // WaitGetPoses paces frames; no monitor vsync on top
    return o_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* params) {
    if (!(flags & DXGI_PRESENT_TEST)) end_frame(sc);
    return o_Present1(sc, sync, flags, params);
}

// Minimized, the game resizes the swap chain to the empty client area (8x8); keep the
// restored size instead so the frame and anything derived from it stay normal.
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffers_t o_ResizeBuffers;
static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                                  UINT flags) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    LONG rw, rh;
    if (restored_client_size(d.OutputWindow, &rw, &rh)) {
        logf("ResizeBuffers %ux%u while minimized -> %ldx%ld", w, h, rw, rh);
        w = UINT(rw);
        h = UINT(rh);
    }
    return o_ResizeBuffers(sc, count, w, h, fmt, flags);
}

// A throwaway swap chain on the game's device gives us the swap chain vtable to patch.
static void hook_swapchain(ID3D11Device* dev) {
    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxdev)))) return;
    if (SUCCEEDED(dxdev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        HWND wnd = CreateWindowExW(0, L"STATIC", L"k7vr", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr,
                                   nullptr);
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferCount = 1;
        d.BufferDesc.Width = 64;
        d.BufferDesc.Height = 64;
        d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.OutputWindow = wnd;
        d.SampleDesc.Count = 1;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* sc = nullptr;
        if (SUCCEEDED(factory->CreateSwapChain(dev, &d, &sc))) {
            patch_vtbl(sc, vt::sc_Present, reinterpret_cast<void*>(&hk_Present), reinterpret_cast<void**>(&o_Present));
            patch_vtbl(sc, vt::sc_ResizeBuffers, reinterpret_cast<void*>(&hk_ResizeBuffers),
                       reinterpret_cast<void**>(&o_ResizeBuffers));
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
                patch_vtbl(sc1, vt::sc1_Present1, reinterpret_cast<void*>(&hk_Present1),
                           reinterpret_cast<void**>(&o_Present1));
                sc1->Release();
            }
            sc->Release();
            logf("swap chain hooked (Present slot %d, Present1 slot %d)", vt::sc_Present, vt::sc1_Present1);
        } else {
            logf("swap chain hook: CreateSwapChain failed");
        }
        DestroyWindow(wnd);
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    dxdev->Release();
}

template <typename T>
static void hook(void* obj, int slot, T hook_fn, T* orig) {
    patch_vtbl(obj, slot, reinterpret_cast<void*>(hook_fn), reinterpret_cast<void**>(orig));
}

template <typename T>
static void wrap(int slot, T hook_fn) {
    wrap_set(slot, reinterpret_cast<void*>(hook_fn));
}

static void hook_device(ID3D11Device* dev) {
    hook(dev, vt::dev_CreateVertexShader, &hk_CreateVS, &o_CreateVS);
    hook(dev, vt::dev_CreateGeometryShader, &hk_CreateGS, &o_CreateGS);
    hook(dev, vt::dev_CreatePixelShader, &hk_CreatePS, &o_CreatePS);
    hook(dev, vt::dev_CreateTexture2D, &hk_CreateTexture2D, &o_CreateTexture2D);
    hook(dev, vt::dev_CreateDeferredContext, &hk_CreateDeferredContext, &o_CreateDeferredContext);
    hook(dev, vt::dev_GetImmediateContext, &hk_GetImmediateContext, &o_GetImmediateContext);

    static_assert(sizeof(vr::EyeConsts) % 16 == 0, "constant buffer size");
    D3D11_BUFFER_DESC eye{sizeof(vr::EyeConsts), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    vr::EyeConsts initial{};
    initial.head[0] = initial.head[5] = initial.head[10] = 1;  // identity, game projection
    D3D11_SUBRESOURCE_DATA init{&initial, 0, 0};
    dev->CreateBuffer(&eye, &init, &g_eye_cb[0]);
    dev->CreateBuffer(&eye, &init, &g_eye_cb[1]);
    dev->CreateBuffer(&eye, &init, &g_gv_cb);
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    vr::init(dev, g_ctx);
    load_card_list();
    init_draw_buffers();

    g_wrapped = wrap_context(g_ctx);
    wrap(vt::ctx_DrawIndexed, &w_DrawIndexed);
    wrap(vt::ctx_Draw, &w_Draw);
    wrap(vt::ctx_DrawIndexedInstanced, &w_DrawIndexedInstanced);
    wrap(vt::ctx_DrawInstanced, &w_DrawInstanced);
    wrap(vt::ctx_Map, &w_Map);
    wrap(vt::ctx_Unmap, &w_Unmap);
    wrap(vt::ctx_UpdateSubresource, &w_UpdateSubresource);
    wrap(vt::ctx_ClearRenderTargetView, &w_ClearRTV);
    wrap(vt::ctx_ClearDepthStencilView, &w_ClearDSV);
    wrap(vt::ctx_CopySubresourceRegion, &w_CopySubresourceRegion);
    wrap(vt::ctx_CopyResource, &w_CopyResource);
    wrap(vt::ctx_ExecuteCommandList, &w_ExecuteCommandList);

    hook_swapchain(dev);
}

// ---- D3D11CreateDevice import hook ----

using CreateDevice_t = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static CreateDevice_t o_CreateDevice;

static HRESULT WINAPI hk_CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                      const D3D_FEATURE_LEVEL* levels, UINT nlevels, UINT sdk, ID3D11Device** dev,
                                      D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    HRESULT hr = o_CreateDevice(adapter, type, sw, flags, levels, nlevels, sdk, dev, level, ctx);
    logf("D3D11CreateDevice(type=%d flags=0x%x) -> 0x%08x", type, flags, hr);
    if (SUCCEEDED(hr) && dev && *dev && !g_dev) {
        g_dev = *dev;
        g_dev->GetImmediateContext(&g_ctx);  // before GetImmediateContext is hooked
        g_ctx->Release();                    // the device keeps it alive
        logf("device feature level 0x%x", g_dev->GetFeatureLevel());
        hook_device(g_dev);
        if (ctx && *ctx == g_ctx) *ctx = g_wrapped;
    }
    return hr;
}

// scanproj: find where the game keeps the current perspective projection (its x and y scale,
// exactly as last written to a constant buffer).
void scan_projection() {
    float p[16];
    memcpy(p, g_last_proj, sizeof(p));
    logf("scanproj: x %.9g y %.9g z %.9g/%.9g", p[0], p[5], p[10], p[11]);
    if (p[0] != 0) mem_scanpair(p[0], p[5], 1e-6f, 64);
}

void install_d3d_hooks() {
    patch_import(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&hk_CreateDevice),
                 reinterpret_cast<void**>(&o_CreateDevice));
}

}  // namespace k7
