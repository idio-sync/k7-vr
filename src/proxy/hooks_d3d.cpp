// D3D11 probe: hooks the game's device, immediate context (through the ctx_wrap stand-in)
// and swap chain. Each frame it counts draws; on request it writes the next frame's full
// draw list (targets, viewports, shaders, the GX projection from constant buffer 0) and a PNG.
#include "common.h"

#include <dxgi1_6.h>
#include <psapi.h>

#include <algorithm>
#include <array>
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
// Where the game's dynamic vertex buffers were last mapped for writing. It appends to one large
// buffer (WRITE_NO_OVERWRITE), so its 2D quads' vertices can be read there at draw time
// (overlay_cover) instead of being read back from the GPU.
static std::unordered_map<ID3D11Resource*, const uint8_t*> g_vb_mapped;
static INT g_draw_base;  // the current indexed draw's base vertex (its quads: vertices base..base+3)

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

// The vertices of a non-indexed draw from vertex buffer 0 (read back now: a stall, for probing).
static bool read_vertices(ID3D11DeviceContext* c, UINT first_vertex, UINT count, std::vector<float>& out, UINT* stride_out) {
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    bool ok = false;
    if (vb && stride) {
        const UINT bytes = count * stride;
        D3D11_BUFFER_DESC bd{bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Buffer* staging = nullptr;
        if (SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &staging))) {
            const UINT begin = offset + first_vertex * stride;
            const D3D11_BOX box{begin, 0, 0, begin + bytes, 1, 1};
            g_ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, vb, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                const float* f = static_cast<const float*>(m.pData);
                out.assign(f, f + bytes / 4);
                *stride_out = stride;
                g_ctx->Unmap(staging, 0);
                ok = true;
            }
            staging->Release();
        }
    }
    if (vb) vb->Release();
    return ok;
}

static void capture_draw(ID3D11DeviceContext* c, const char* kind, UINT count, UINT instances, INT first_vertex) {
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
    ID3D11RasterizerState* rs = nullptr;
    c->RSGetState(&rs);
    if (rs) {
        D3D11_RASTERIZER_DESC rd;
        rs->GetDesc(&rd);
        rs->Release();
        if (rd.ScissorEnable) {
            D3D11_RECT r;
            UINT nr = 1;
            c->RSGetScissorRects(&nr, &r);
            if (nr) appendf(s, " scissor=%ld,%ld-%ld,%ld", r.left, r.top, r.right, r.bottom);
        }
    }

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
                // Texture generation: TEXMTX0 (register 30) and post-transform matrices 0-1
                // (registers 96+), which the template applies to eye-space positions.
                append_matrix(s, "tm0", f + 30 * 4, 3);
                append_matrix(s, "pt0", f + 96 * 4, 3);
                append_matrix(s, "pt1", f + 99 * 4, 3);
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
    {  // vertex buffer 0: how the game supplies vertices (usage 2 = dynamic, 0 = default)
        ID3D11Buffer* vb = nullptr;
        UINT vstride = 0, voffset = 0;
        c->IAGetVertexBuffers(0, 1, &vb, &vstride, &voffset);
        if (vb) {
            D3D11_BUFFER_DESC bd;
            vb->GetDesc(&bd);
            appendf(s, " vb0=#%d(%u bytes, usage %d, cpu 0x%x) offset=%u first=%d", id_of(vb), bd.ByteWidth,
                    int(bd.Usage), bd.CPUAccessFlags, voffset, first_vertex);
            vb->Release();
        }
    }
    // Tiny non-indexed draws (points, lines, sprites): their vertices, as floats.
    std::vector<float> f;
    UINT stride = 0;
    // (Indexed 6-index quads: their four vertices from the base vertex.)
    const UINT shown = first_vertex >= 0 ? count : count / 6 * 4;
    if (count <= 12 && (first_vertex >= 0 || (count % 6 == 0 && g_draw_base >= 0)) &&
        read_vertices(c, UINT(first_vertex >= 0 ? first_vertex : g_draw_base), shown, f, &stride)) {
        appendf(s, " verts(stride %u)=", stride);
        for (UINT v = 0; v < shown; ++v) {
            s += v ? "|" : "";
            for (UINT i = 0; i < stride / 4 && i < 8; ++i) appendf(s, i ? " %.6g" : "%.6g", f[v * (stride / 4) + i]);
        }
    }
    s += '\n';
}

// `scanpoint`: at the next single-vertex point draw (e.g. the aiming tutorial's laser dot),
// search memory for its x,y pair while the frame is held (memscan.cpp).
static bool g_scan_point;
void request_scan_point() { g_scan_point = true; }

static void maybe_scan_point(UINT count, UINT first_vertex) {
    if (!g_scan_point || count != 1) return;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_POINTLIST) return;
    g_scan_point = false;
    std::vector<float> f;
    UINT stride = 0;
    if (!read_vertices(g_ctx, first_vertex, 1, f, &stride) || f.size() < 3) return;
    logf("scanpoint: vertex %g %g %g", f[0], f[1], f[2]);
    mem_scanpair(f[0], f[1], 1e-6f, 8);
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
static ID3D11Buffer* g_own_cb;     // ...for cameras the game didn't inflate: their own projection
static float g_gv_tan[2];          // its frustum's half-angle tangents this frame (x, y; 0: not replaced)
// Flat frames: frames that draw a 3D picture only to copy it into a texture, then clear the
// scene target and draw a 2D screen with that copy. Harman's TV does this: the persona's portrait
// and the channel's text are drawn full-frame, copied, and the copy is drawn onto the TV's curved
// glass inside the TV set's artwork. Split for the headset, the portrait would be drawn with the
// eyes' frustum and the text taken to the HUD panel, and the glass would show neither. A frame
// that shows the pattern makes the next one flat: drawn with the game's camera into both eyes,
// its text kept in place, and handed off as a 2D frame (the screen panel).
static bool g_flat_frame;    // this frame is drawn flat
static bool g_frame_persp;   // a perspective draw went into the scene target this frame (flat or not)
static bool g_scene_copied;  // ...and after it a draw copied the scene target into another frame-sized texture
static bool g_capture_seen;  // ...and after that the scene target was cleared
static ID3D11Buffer* g_draw_cb[2];  // slot b13 per-draw transform: [0] none, [1] first-person arm and gun
constexpr UINT kDrawSlot = 13;
static bool g_aim_mode;             // the game's first-person aim camera is active this frame
static D3D11_RECT g_letterbox;      // this frame's cutscene letterbox band (eye-target pixels)
static bool g_letterbox_on;
static int g_gun_run;               // 0 before the frame's first 3D draw, 1 inside the arm/gun run, 2 after
static vr::EyeConsts g_eyes[2];    // this frame's eyes
static bool g_pair;                // save both eyes at the end of this frame
static unsigned g_pair_index;
static bool g_sweep;               // `sweep`: save this frame's left eye for one head direction
static float g_sweep_yaw, g_sweep_pitch;
static unsigned g_sweep_index;

static std::unordered_map<ID3D11Resource*, ID3D11Texture2D*> g_twins;  // game texture -> right-eye twin
static std::unordered_map<ID3D11View*, ID3D11View*> g_twin_views;     // game view -> same view of the twin

static bool g_frame_3d;              // a perspective draw went into the scene target this frame
static bool g_prev_frame_3d;         // ...and the frame before it
// The game's own viewport for this frame's 3D scene (the largest a perspective draw used) and the
// band its scissor shows: cutscenes draw a letterboxed band, which the eyes fill (fill_eye). For
// the pair log (tools\catalog.py compares that band with the eye).
static D3D11_VIEWPORT g_scene_vp;
static D3D11_RECT g_scene_band;
static bool g_scene_vp_set;
// Menu screens over the 3D scene (the pause menu and its pages) are built from many untextured 2D
// quads (panels, frames, the dim) among the textured artwork: 32 a frame, against 1-3 (fades,
// letterbox bars) in gameplay and cutscenes and up to 14 with LT held (the eye icon's view).
// Split by the HUD rule, the artwork goes to the HUD panel and the quads to the eyes, and the
// layering between them is lost, so such frames go to the screen panel whole, as the game view
// draws them.
constexpr int kMenuQuads = 24;
// An untextured 2D quad covering at least this share of the screen is a fade (it stays in the
// eyes, over the whole view); smaller ones belong to an overlay (they go to the HUD panel).
constexpr float kFadeCover = 0.95f;
static int g_menu_quads;             // untextured 2D quads drawn after the scene this frame (eyes or HUD)
static bool g_menu_shown;            // the last frame went to the screen panel as a menu (logs changes)
static unsigned g_eye_targets_made;  // the game's 4:3 targets made at the eye size (hk_CreateTexture2D)
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
//    where the controller's ray meets the scene (measured in the eye's depth, see "Aim distance"),
//    so shots land where the hand points at any range (aim_converge_m along the ray without one);
//  - the first-person arm and gun are moved from the game's aim view into the hand.
static float g_gun_scale = 0.6f;
// The gun's grip in the game's aim view (game units). Each character holds the gun elsewhere
// (Garcian ~2.5 units ahead, Dan ~4.3), so it is found at the start of each aim session: the
// first arm/gun draw whose model sits 1-8 units ahead (skinned arm pieces report other matrices),
// followed while the arm is raised and fixed once it holds still, so the game's recoil still
// moves the gun. [vr] gun_pivot (or `gun pivot`) sets it by hand instead.
static float g_gun_pivot[3] = {0.45f, -0.9f, -2.5f};
static bool g_gun_pivot_manual;
// The gun's own axes in the game's aim view ([row][col]; columns are the model's x, y and z, and
// +z is the barrel), from the grip draw's model-view. The game angles each gun in towards its
// screen centre, where its shots go (Dan's revolver ~10 degrees left and 4 down, rolled ~20;
// Garcian's pistol ~2 left). Moved into the hand as it is, the barrel would point that far off
// the controller's pointing axis, which the shot follows; so the gun is turned straight in the
// hand: barrel along the pointing axis, model up = controller up ([vr] gun_align=0: as the game
// holds it, and the shot follows its barrel instead).
static float g_gun_axes[3][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}};  // straight already
static bool g_gun_align = true;
static float g_gun_fix[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // aim view -> straightened gun
static vr::Pose g_barrel = vr::identity();                         // the barrel's pose in the hand's frame
static void update_gun_fix();
static int g_pivot_frames;                            // aim-mode frames so far this session
static bool g_pivot_seen;                             // found in this frame's arm/gun draws
static int g_pivot_still;                             // frames the grip has held still
static bool g_pivot_locked;                           // the grip is fixed for this aim session
static float g_gun_clip = 2.0f;                       // the arm (sleeve) is cut off this far behind the grip (units)
// The cut follows the grip as the game's recoil moves it: a shot kicks the gun back and up, and a
// cut fixed behind where the grip rested sliced the hand open (seen through its back). The cut
// sits behind the nearer of the rest grip and the last frame's, and moves back during a frame
// if that frame's grip draw comes nearer still (the arm pieces may be drawn before the grip).
// The recoil itself is damped in the hand: Dan's revolver kicks the grip 2.3 units back and 3.2
// up, which in the hand put the gun and forearm in the player's face. [vr] gun_recoil (or `gun
// recoil`): the share of the kick's movement kept (its rotation is kept whole).
static float g_gun_recoil = 0.25f;
static bool g_grip_seen;                              // this frame's grip draw has been seen
static float g_grip_now[3], g_grip_last[3];           // its position (game aim view), and last frame's
static bool g_grip_last_valid;
static float g_clip_base;                             // the z the cut is behind this frame
static float g_draw_m[16];                            // the arm/gun transform as last uploaded (slot b13)
static float g_draw_qt[3];                            // ...the hand's position in it (before the grip offset)
static void set_draw_grip(const float grip[3]);
static float g_aim_converge = 10;                     // metres, when the ray's hit isn't known
static bool g_aim_depth = true;                       // aim at the ray's measured hit ([vr] aim_depth, `gun depth`)
static float g_hit_min = 0.6f, g_hit_max = 60;        // metres along the ray searched: past the gun, to far out
static float g_hit_ignore = 1.2f;                     // metres from the eye: nearer is the gun and hand ([vr] aim_ignore_m)
static bool g_aim_ray_ready;                          // g_laser_pose is this frame's hand (also without a laser)
static bool g_hit_done;                               // the ray was measured this frame
static unsigned g_hit_session;                        // aim sessions so far (stale results are dropped)
static bool g_anchor_set;
static vr::Pose g_anchor;                             // world pose (game units) of the player's frame
static bool g_relocked;                               // this frame's head transforms follow its camera
static int g_aim_trace;                               // `aimtrace N` frames left
static bool g_prev_cam_valid;                         // last frame's camera is known (this aim session)
static float g_aim_written_yaw;
static float g_laser_muzzle = 0.15f;                  // the laser starts this far forward of the grip, metres
static float g_laser_length = 30;                     // metres; [vr] laser_m, 0 = no laser
static bool g_aim_laser;                              // a laser from the gun in aim mode ([vr] aim_laser=1; off: the game has none)
static float g_aim_hud_mask = 0.6f;                   // HUD centre band hidden in aim mode (the reticle)
static bool g_laser_depth = true;                     // walls hide the laser (`gun laserdepth 0`: debugging)
static bool g_steer = true;                           // the hand steers the game's aim / pointer (`gun steer 0`)
static bool g_texgen_ref = true;                      // screen gradients from the aim-start camera ([vr] texgen_ref, `gun texref`)
static bool g_cam_from_draw = true;                   // aim mode: the camera each frame was drawn with, read from its draws (`gun camdraw`)
static bool g_laser_ready;                            // g_laser_pose is this frame's
static vr::Pose g_laser_pose;                         // the hand in the game camera's eye space (game units)
static float g_laser_from, g_laser_to;                // this frame's beam, metres along the hand's ray
static bool g_pointer_mode;                           // a laser-pointer screen follows the hand this frame
// Guns without a laser in aim mode, by the vertex count of the gun mesh the grip is found on
// (the game has no persona flag we know of; the meshes differ). [vr] no_laser_guns, default Dan's
// revolver (1269; Garcian's pistol is 690).
static std::vector<UINT> g_no_laser_guns = {1269};
static UINT g_grip_mesh;                              // vertex count of this aim session's grip draw
static UINT g_draw_count;                             // the current draw's vertex count
static INT g_draw_first = -1;                         // and its first vertex (-1: an indexed draw)

static bool gun_has_laser() {
    return std::find(g_no_laser_guns.begin(), g_no_laser_guns.end(), g_grip_mesh) == g_no_laser_guns.end();
}

static void init_draw_buffers() {
    wchar_t buf[64];
    const std::wstring ini = path_in(L"k7vr.ini");
    GetPrivateProfileStringW(L"vr", L"gun_scale", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_scale = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"gun_pivot", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_pivot_manual = swscanf_s(buf, L"%f,%f,%f", &g_gun_pivot[0], &g_gun_pivot[1], &g_gun_pivot[2]) == 3;
    GetPrivateProfileStringW(L"vr", L"gun_clip", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_clip = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"gun_recoil", L"", buf, 64, ini.c_str());
    if (buf[0]) g_gun_recoil = std::clamp(float(_wtof(buf)), 0.f, 1.f);
    GetPrivateProfileStringW(L"vr", L"aim_converge_m", L"", buf, 64, ini.c_str());
    if (buf[0]) g_aim_converge = float(_wtof(buf));
    g_aim_depth = GetPrivateProfileIntW(L"vr", L"aim_depth", 1, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"vr", L"aim_ignore_m", L"", buf, 64, ini.c_str());
    if (buf[0]) g_hit_ignore = float(_wtof(buf));
    g_texgen_ref = GetPrivateProfileIntW(L"vr", L"texgen_ref", 1, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"vr", L"laser_m", L"", buf, 64, ini.c_str());
    if (buf[0]) g_laser_length = float(_wtof(buf));
    g_aim_laser = GetPrivateProfileIntW(L"vr", L"aim_laser", 0, ini.c_str()) != 0;
    g_gun_align = GetPrivateProfileIntW(L"vr", L"gun_align", 1, ini.c_str()) != 0;
    update_gun_fix();
    GetPrivateProfileStringW(L"vr", L"aim_hud_mask", L"", buf, 64, ini.c_str());
    if (buf[0]) g_aim_hud_mask = float(_wtof(buf));
    GetPrivateProfileStringW(L"vr", L"no_laser_guns", L"1269", buf, 64, ini.c_str());
    g_no_laser_guns.clear();
    for (wchar_t *ctx = nullptr, *tok = wcstok_s(buf, L", ", &ctx); tok; tok = wcstok_s(nullptr, L", ", &ctx))
        g_no_laser_guns.push_back(UINT(_wtoi(tok)));
    GetPrivateProfileStringW(L"vr", L"aim_pitch_limit", L"80", buf, 64, ini.c_str());
    const float limit = float(_wtof(buf));
    if (limit > 36 && limit < 90) aim_widen_clamp(limit * 3.14159265f / 180);
    pointer_install();
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
    logf("stereo: gun scale %g, aim at the ray's hit %s (else %g m)", g_gun_scale, g_aim_depth ? "on" : "off",
         g_aim_converge);
}

void set_gun_param(const std::string& name, const float v[3]) {
    if (name == "scale") g_gun_scale = v[0];
    else if (name == "pivot") memcpy(g_gun_pivot, v, sizeof(g_gun_pivot)), g_gun_pivot_manual = true;
    else if (name == "autopivot") g_gun_pivot_manual = false, g_pivot_frames = g_pivot_still = 0, g_pivot_locked = false;
    else if (name == "clip") g_gun_clip = v[0];
    else if (name == "recoil") g_gun_recoil = std::clamp(v[0], 0.f, 1.f);
    else if (name == "converge") g_aim_converge = v[0];
    else if (name == "hudmask") g_aim_hud_mask = v[0];
    else if (name == "laser") g_laser_length = v[0], g_aim_laser = v[0] > 0;
    else if (name == "align") g_gun_align = v[0] != 0, update_gun_fix();  // 0: the gun as the game holds it
    else if (name == "laserdepth") g_laser_depth = v[0] != 0;
    else if (name == "steer") g_steer = v[0] != 0;  // 0: the hand doesn't steer the game's aim (testing)
    else if (name == "camdraw") g_cam_from_draw = v[0] != 0;  // 0: assume the one-frame camera lag (as before)
    else if (name == "texref") g_texgen_ref = v[0] != 0;  // 0: gradients follow the game camera (as before)
    else if (name == "depth") g_aim_depth = v[0] != 0;  // 0: aim at converge M along the ray (the old way)
    else if (name == "ignore") g_hit_ignore = v[0];     // metres from the eye that don't count (gun, hand)
    else logf("gun: unknown setting '%s'", name.c_str());
    logf("gun: scale %g pivot %g,%g,%g clip %g converge %g depth %d hud mask %g", g_gun_scale, g_gun_pivot[0],
         g_gun_pivot[1], g_gun_pivot[2], g_gun_clip, g_aim_converge, int(g_aim_depth), g_aim_hud_mask);
}

// An arm/gun draw in the first frames of an aim session: its model position (model-view
// translation) is the grip if it sits 1-8 units in front of the camera. The transform for the
// rest of this frame's run is rewritten right away.
static void relock_aim();

// Game units. The arm, gun and body behind the camera sit within ~10 (Garcian's 2.0-9.5,
// Kaede's 2.7-6.7); the nearest world models seen while aiming are ~17 out (25 took Kaede's
// lobby geometry into the "gun", so it moved with the hand).
constexpr float kGunRunRadius = 12;

// The bound draw's model position in eye space (the translation of its position matrix 0).
static bool model_translation(float t[3]) {
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return false;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kTemplateCbMin) return false;
    const float* f = reinterpret_cast<const float*>(it->second.data());
    t[0] = f[3], t[1] = f[7], t[2] = f[11];
    return true;
}

// The bound draw's model axes in eye space (the columns of position matrix 0's rotation).
static bool model_axes(float g[3][3]) {
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return false;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kTemplateCbMin) return false;
    const float* f = reinterpret_cast<const float*>(it->second.data());
    for (int c = 0; c < 3; ++c) {
        const float len = sqrtf(f[c] * f[c] + f[4 + c] * f[4 + c] + f[8 + c] * f[8 + c]);
        if (len < 1e-6f) return false;
        for (int r = 0; r < 3; ++r) g[r][c] = f[r * 4 + c] / len;
    }
    return true;
}

// g_gun_fix and g_barrel from g_gun_axes and g_gun_align.
static void update_gun_fix() {
    // Straight = the model's x, y, z along the hand's -x, y, -z: fix = straight * axes^T.
    static const float kStraight[3] = {-1, 1, -1};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) g_gun_fix[i][j] = g_gun_align ? kStraight[i] * g_gun_axes[j][i] : float(i == j);
    // The barrel (model +z) after the fix, in the hand's frame; the aim follows it.
    float b[3];
    for (int i = 0; i < 3; ++i)
        b[i] = g_gun_fix[i][0] * g_gun_axes[0][2] + g_gun_fix[i][1] * g_gun_axes[1][2] + g_gun_fix[i][2] * g_gun_axes[2][2];
    const float z[3] = {-b[0], -b[1], -b[2]};  // the pose's +z: the barrel points along its -z
    float x[3] = {z[2], 0, -z[0]};             // (0,1,0) x z
    const float xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6f) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    g_barrel = vr::identity();
    for (int i = 0; i < 3; ++i) g_barrel.m[i][0] = x[i], g_barrel.m[i][1] = y[i], g_barrel.m[i][2] = z[i];
}

static void find_gun_pivot() {
    if (g_pivot_seen || g_pivot_locked) return;
    float t[3];
    if (!model_translation(t) || !(t[2] < -1 && t[2] > -8)) return;
    g_pivot_seen = true;
    g_grip_mesh = g_draw_count;
    float axes[3][3];
    bool turned = false;
    if (model_axes(axes)) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) turned = turned || fabsf(axes[r][c] - g_gun_axes[r][c]) > 1e-4f;
        if (turned) memcpy(g_gun_axes, axes, sizeof(axes)), update_gun_fix();
    }
    if (g_gun_pivot_manual) {
        if (turned) relock_aim();
        return;
    }
    // The arm is still being raised in the first frames: follow it until it holds still.
    const float moved = fabsf(t[0] - g_gun_pivot[0]) + fabsf(t[1] - g_gun_pivot[1]) + fabsf(t[2] - g_gun_pivot[2]);
    g_pivot_still = moved < 0.01f ? g_pivot_still + 1 : 0;
    if (g_pivot_still >= 10 || g_pivot_frames > 120) {
        g_pivot_locked = true;
        const float r2d = 180 / 3.14159265f;
        logf("aim: gun grip at %.2f,%.2f,%.2f (gun mesh %u vertices), barrel %.1f deg %s %.1f deg %s of the game "
             "camera's axis, %s", t[0], t[1], t[2], g_grip_mesh, fabsf(asinf(g_gun_axes[0][2])) * r2d,
             g_gun_axes[0][2] < 0 ? "left" : "right", fabsf(asinf(g_gun_axes[1][2])) * r2d,
             g_gun_axes[1][2] < 0 ? "down" : "up", g_gun_align ? "turned straight in the hand" : "held as the game holds it");
    }
    if (moved < 1e-4f && !turned) return;
    memcpy(g_gun_pivot, t, sizeof(t));
    relock_aim();  // same camera and hand, new grip
}

static bool aim_hit_distance(float* metres);

// Frame start, aim mode: point the game's aim where the hand points.
static void steer_aim() {
    vr::Pose hand;
    if (!g_steer || !g_anchor_set || !vr::hand_pose(&hand)) return;
    hand = vr::compose(hand, g_barrel);  // the ray: along the gun's barrel (the pointing axis once straightened)
    // The game fires from its aim eye, the hand's ray starts at the controller: they meet at the
    // target point, so it is where the ray hits the scene when that is known.
    float c = g_aim_converge;
    if (g_aim_depth) aim_hit_distance(&c);
    // Target point in the anchor frame (metres), then in the world (game units), then its direction
    // from the aim eye as the game has it now (the anchor's origin is where it was when aiming began).
    const float u = vr::unit();
    const float p[3] = {(hand.m[0][3] - c * hand.m[0][2]) / u, (hand.m[1][3] - c * hand.m[1][2]) / u,
                        (hand.m[2][3] - c * hand.m[2][2]) / u};
    float eye[3], tgt[3];
    if (!aim_camera(eye, tgt))
        for (int i = 0; i < 3; ++i) eye[i] = g_anchor.m[i][3];
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = g_anchor.m[i][0] * p[0] + g_anchor.m[i][1] * p[1] + g_anchor.m[i][2] * p[2] + g_anchor.m[i][3] - eye[i];
    float yaw, pitch;
    vr::aim_angles(d, &yaw, &pitch);
    aim_set(yaw, pitch);
    g_aim_written_yaw = yaw;
}

// First 3D draw of an aim-mode frame: the game camera is final now. Rebuild both eyes against
// it, and put the gun in the hand.
// `aimtrace N`: for N aim-mode frames, log the yaw written, the aim controller's, the camera
// manager's at the first 3D draw, and the one the world was drawn with (first world draw's
// model-view rotation; level geometry has none of its own), to measure any camera lag.
void request_aim_trace(int frames) { g_aim_trace = frames; }

static float yaw_of(float fx, float fz) { return atan2f(fx, fz); }

// Aim-mode cameras (game units, in the anchor frame): the manager's at this frame's first draw,
// the one it reported a frame earlier, the one this frame is taken to be drawn with, and the
// one when aiming began (the gradients' reference).
static vr::Pose g_now_cam, g_next_cam, g_drawn_cam, g_ref_cam;

// Both eyes' head transforms, the gradients' reference transform and the gun in the hand, for
// a frame drawn with the game camera `cam`.
static void apply_aim_cam(const vr::Pose& cam) {
    const float u = vr::unit();
    vr::relock(vr::scaled(cam, u), g_eyes);
    // The game's screen-space gradients are generated from the reference camera's view, not the
    // hand-steered one (see patch_texgen in hooks_compile.cpp). [vr] texgen_ref=0 / `gun texref 0`: off.
    const vr::Pose to_ref = vr::compose(vr::inverse(g_ref_cam), cam);
    for (int e = 0; e < 2; ++e) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) g_eyes[e].ref[i * 4 + j] = to_ref.m[i][j];
        g_eyes[e].flags[1] = g_texgen_ref ? 1.f : 0.f;
    }
    for (int e = 0; e < 2; ++e) g_ctx->UpdateSubresource(g_eye_cb[e], 0, nullptr, &g_eyes[e], 0, 0);

    vr::Pose hand;
    if (!g_draw_cb[1] || !vr::hand_pose(&hand)) return;
    // Game aim view -> anchor: the grip goes to the hand, turned straight (g_gun_fix) and scaled
    // by gun_scale around the grip.
    const vr::Pose q = vr::compose(vr::inverse(cam), vr::scaled(hand, 1 / u));
    g_laser_pose = vr::compose(q, g_barrel);  // the aim ray, in the game camera's eye space
    g_aim_ray_ready = true;
    g_laser_ready = g_aim_laser && g_laser_length > 0 && gun_has_laser();
    g_laser_from = g_laser_muzzle, g_laser_to = g_laser_length;
    const float s = g_gun_scale;
    float qf[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) qf[i][j] = q.m[i][0] * g_gun_fix[0][j] + q.m[i][1] * g_gun_fix[1][j] + q.m[i][2] * g_gun_fix[2][j];
    memset(g_draw_m, 0, sizeof(g_draw_m));
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) g_draw_m[i * 4 + j] = qf[i][j] * s;
        g_draw_qt[i] = q.m[i][3];
    }
    g_draw_m[12] = 1;
    const bool moved = g_pivot_locked && g_grip_last_valid;
    g_clip_base = moved ? g_grip_last[2] : g_gun_pivot[2];
    set_draw_grip(moved ? g_grip_last : g_gun_pivot);
}

// The arm/gun transform (slot b13) for the grip at `grip` (game aim view): the hand holds the
// resting grip (g_gun_pivot) and gun_recoil of the way to `grip`; the cut is gun_clip behind the
// nearer of `grip` and g_clip_base.
static void set_draw_grip(const float grip[3]) {
    float pe[3];
    for (int j = 0; j < 3; ++j) pe[j] = g_gun_pivot[j] + (1 - g_gun_recoil) * (grip[j] - g_gun_pivot[j]);
    for (int i = 0; i < 3; ++i)
        g_draw_m[i * 4 + 3] = g_draw_qt[i] - (g_draw_m[i * 4] * pe[0] + g_draw_m[i * 4 + 1] * pe[1] + g_draw_m[i * 4 + 2] * pe[2]);
    g_clip_base = std::max(g_clip_base, grip[2]);
    g_draw_m[13] = g_clip_base + g_gun_clip;
    if (g_draw_cb[1]) g_ctx->UpdateSubresource(g_draw_cb[1], 0, nullptr, g_draw_m, 0, 0);
}

// An arm/gun draw in aim mode once the grip is fixed: the grip draw gives this frame's grip, and
// the draws after it follow it (those before it had last frame's; see g_grip_seen).
static void track_grip() {
    float t[3];
    if (!g_pivot_locked || g_grip_seen || g_draw_count != g_grip_mesh || !model_translation(t) || !(t[2] < -1 && t[2] > -8))
        return;
    g_grip_seen = true;
    memcpy(g_grip_now, t, sizeof(t));
    static int logged;
    if (fabsf(t[2] - g_gun_pivot[2]) > 0.05f && logged < 20) {
        ++logged;
        logf("aim: recoil, grip at %.2f,%.2f,%.2f (rests at %.2f,%.2f,%.2f)", t[0], t[1], t[2], g_gun_pivot[0],
             g_gun_pivot[1], g_gun_pivot[2]);
    }
    set_draw_grip(t);
}

static void relock_aim() {
    const bool first = !g_relocked;
    g_relocked = true;
    float eye[3], tgt[3];
    if (!g_anchor_set || !aim_camera(eye, tgt)) return;
    if (first && g_aim_trace > 0) {
        float aimctl[2] = {};
        memcpy(aimctl, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0x46beac), 8);
        logf("aimtrace frame %u: written %.3f aimctl %.3f manager %.3f", g_frame, g_aim_written_yaw, aimctl[1],
             yaw_of(tgt[0] - eye[0], tgt[2] - eye[2]));
    }
    // The game usually draws a frame with the camera it reported one frame earlier (aimtrace: the
    // manager's camera at the first draw is the next frame's), so lock against that for now; the
    // frame's first world draw then says which camera it really was (correct_aim_cam).
    if (first) {
        g_now_cam = vr::compose(vr::inverse(g_anchor), vr::look_at(eye, tgt));
        if (!g_prev_cam_valid) g_ref_cam = g_now_cam;  // the camera when aiming began
        g_drawn_cam = g_prev_cam_valid ? g_next_cam : g_now_cam;
        g_next_cam = g_now_cam;
        g_prev_cam_valid = true;
    }
    apply_aim_cam(g_drawn_cam);
}

// Angle between two cameras' forward axes, radians.
static float forward_angle(const vr::Pose& a, const vr::Pose& b) {
    const float d = a.m[0][2] * b.m[0][2] + a.m[1][2] * b.m[1][2] + a.m[2][2] * b.m[2][2];
    return acosf(d > 1 ? 1 : d < -1 ? -1 : d);
}

// The frame's first world draw (after the arm and gun): the camera it was really drawn with.
// The one-frame lag above doesn't always hold: above 60 fps the game's camera updates and its
// draws don't pair up one to one, so while the hand moves the eyes and the gradients' reference
// were a frame off on some frames (the gradients swam with the gun in the headset). The draw's
// model-view rotation is the camera's (level geometry has none of its own); it is used when it
// is close to one of the recent cameras, so a first draw with its own rotation changes nothing.
static void correct_aim_cam() {
    if (!g_aim_mode || !g_anchor_set || !g_relocked || !g_cam_from_draw) return;
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kTemplateCbMin) return;
    const float* f = reinterpret_cast<const float*>(it->second.data());  // model-view rows (world -> eye)
    vr::Pose world{};  // the camera in the world: the transposed rotation, at the aim eye
    for (int r = 0; r < 3; ++r) {
        const float len = sqrtf(f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] + f[r * 4 + 2] * f[r * 4 + 2]);
        if (len < 1e-6f) return;
        for (int c = 0; c < 3; ++c) world.m[c][r] = f[r * 4 + c] / len;
    }
    vr::Pose cam = vr::compose(vr::inverse(g_anchor), world);
    for (int i = 0; i < 3; ++i) cam.m[i][3] = g_drawn_cam.m[i][3];  // the aim eye doesn't move
    constexpr float kTolerance = 3 * 3.14159265f / 180;
    const float to_drawn = forward_angle(cam, g_drawn_cam), to_now = forward_angle(cam, g_now_cam);
    if (to_drawn > kTolerance && to_now > kTolerance) return;
    if (to_drawn < 1e-5f) return;  // already right
    g_drawn_cam = cam;
    apply_aim_cam(cam);
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
    const float d0 = g_laser_from / u, d1 = g_laser_to / u;
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

// ---- Aim distance ----
// The game fires from its aim eye through the centre of its view; the hand's ray starts at the
// controller, below and right of the eye. Steered at a fixed point along the ray, the two lines
// only meet there: nearer walls were hit above and left of the laser. So once per aim-mode frame,
// at the first HUD draw (the scene's depth is complete), a compute pass marches the hand's ray
// through the left eye's depth buffer, and steer_aim aims at the first sample behind a surface.
// Read back a frame or more later without waiting. Samples are spaced in proportion to their
// distance (~4% apart), so the miss left over is a few % of the eye-to-hand offset.
constexpr UINT kHitSamples = 128;  // = the shader's thread count
constexpr int kHitRing = 4;
static ID3D11ComputeShader* g_hit_cs;
static ID3D11Buffer* g_hit_cb;
static ID3D11Buffer* g_hit_buf;  // [0] = kHitSamples - first hit (0: none), [1] = 1 + the last sample in view (0: none)
static ID3D11UnorderedAccessView* g_hit_uav;
static ID3D11Buffer* g_hit_trace;  // per-sample float4s (logged while `aimtrace` runs: a stall)
static ID3D11UnorderedAccessView* g_hit_trace_uav;
static ID3D11Buffer* g_hit_trace_staging;
static ID3D11Buffer* g_hit_staging[kHitRing];
static unsigned g_hit_slot_frame[kHitRing];    // g_frame + 1 when written, 0 = free
static unsigned g_hit_slot_session[kHitRing];
static int g_hit_next;                         // the next slot written (the oldest in flight)
static ID3D11Texture2D* g_hit_depth_tex;       // what g_hit_srv views (the view keeps it alive)
static ID3D11ShaderResourceView* g_hit_srv;
static bool g_hit_valid;
static float g_hit_m;                          // metres along the ray
static unsigned g_hit_frame;
static bool g_hit_failed;

static const char kHitCs[] =
    "Texture2D<float> depth : register(t0);\n"
    "RWBuffer<uint> result : register(u0);\n"
    "RWBuffer<float4> trace : register(u1);\n"  // per sample: its depth, the buffer's, the pixel (`aimtrace`)
    "cbuffer Hit : register(b0) { float4 org; float4 dir; float4 head[3]; float4 px; float4 py; float4 projz; float4 projw;\n"
    "                             float4 pc; float4 ray; float4 nearz; };\n"  // ray: first distance (units), ln(step), depth size
    "[numthreads(128, 1, 1)] void main(uint i : SV_GroupIndex) {\n"
    "  float4 p = float4(org.xyz + ray.x * exp(ray.y * i) * dir.xyz, 1);\n"
    "  float4 e = float4(dot(head[0], p), dot(head[1], p), dot(head[2], p), 1);\n"
    "  float4 c = float4(dot(e, px), dot(e, py), dot(e, projz), dot(e, projw));\n"
    "  trace[i] = float4(-1, -1, c.w, 0);\n"
    "  if (c.w <= 0) return;\n"
    "  c.z = c.w * pc.w - c.z * pc.z;\n"
    "  c.xy -= c.w * pc.xy;\n"
    "  float2 ndc = c.xy / c.w;\n"
    "  trace[i] = float4(-2, -2, ndc);\n"
    "  if (abs(ndc.x) >= 1 || abs(ndc.y) >= 1) return;\n"
    "  uint2 pix = uint2((ndc.x * 0.5 + 0.5) * ray.z, (0.5 - ndc.y * 0.5) * ray.w);\n"
    "  float b = depth[pix];\n"
    "  trace[i] = float4(c.z / c.w, b, pix);\n"
    // The game's depth is cleared to 0 and keeps the nearest = largest value. Anything nearer the
    // eye than nearz.x is the gun and hand (or a wall in your face): this sample is unknown.
    "  if (b > nearz.x) return;\n"
    "  InterlockedMax(result[1], i + 1);\n"
    "  if (b > c.z / c.w * 1.003) InterlockedMax(result[0], 128u - i);\n"
    "}\n";

static bool hit_init() {
    if (g_hit_cs) return true;
    if (g_hit_failed) return false;
    g_hit_failed = true;
    if (ID3DBlob* cs = compile_hlsl(kHitCs, "cs_5_0", "k7vr_aim_hit")) {
        D3D11_BUFFER_DESC cbd{12 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_BUFFER_DESC bd{8, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 2;
        D3D11_BUFFER_DESC sd{8, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        D3D11_BUFFER_DESC td{kHitSamples * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0};
        D3D11_BUFFER_DESC tsd{kHitSamples * 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC tud{};
        tud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        tud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        tud.Buffer.NumElements = kHitSamples;
        bool ok = SUCCEEDED(g_dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_hit_cs)) &&
                  SUCCEEDED(g_dev->CreateBuffer(&cbd, nullptr, &g_hit_cb)) &&
                  SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_hit_buf)) &&
                  SUCCEEDED(g_dev->CreateUnorderedAccessView(g_hit_buf, &ud, &g_hit_uav)) &&
                  SUCCEEDED(g_dev->CreateBuffer(&td, nullptr, &g_hit_trace)) &&
                  SUCCEEDED(g_dev->CreateUnorderedAccessView(g_hit_trace, &tud, &g_hit_trace_uav)) &&
                  SUCCEEDED(g_dev->CreateBuffer(&tsd, nullptr, &g_hit_trace_staging));
        for (auto& s : g_hit_staging) ok = ok && SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &s));
        g_hit_failed = !ok;
        cs->Release();
    }
    if (g_hit_failed) logf("aim: depth measurement unavailable; aiming at %g m along the ray", g_aim_converge);
    return !g_hit_failed;
}

static ID3D11Texture2D* texture_of(ID3D11View* view, D3D11_TEXTURE2D_DESC* desc);

// The left eye's depth, readable by the compute pass (null if its format isn't one we know).
static ID3D11ShaderResourceView* hit_depth_view(ID3D11DepthStencilView* dsv, UINT* w, UINT* h) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D* tex = texture_of(dsv, &d);
    if (!tex || d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
    *w = d.Width, *h = d.Height;
    if (tex == g_hit_depth_tex && g_hit_srv) return g_hit_srv;
    if (g_hit_srv) g_hit_srv->Release(), g_hit_srv = nullptr;
    g_hit_depth_tex = tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = d.Format == DXGI_FORMAT_R32_TYPELESS          ? DXGI_FORMAT_R32_FLOAT
                : d.Format == DXGI_FORMAT_R24G8_TYPELESS      ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                : d.Format == DXGI_FORMAT_R32G8X24_TYPELESS   ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                : d.Format == DXGI_FORMAT_R16_TYPELESS        ? DXGI_FORMAT_R16_UNORM
                                                              : DXGI_FORMAT_UNKNOWN;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    if (vd.Format == DXGI_FORMAT_UNKNOWN || FAILED(g_dev->CreateShaderResourceView(tex, &vd, &g_hit_srv))) {
        static int logged;
        if (logged++ < 3) logf("aim: can't read depth format %d", int(d.Format));
        g_hit_srv = nullptr;
    }
    return g_hit_srv;
}

// At the first HUD draw of an aim-mode frame (`dsv` = the left eye's scene depth).
static void measure_aim_hit(ID3D11DepthStencilView* dsv) {
    if (g_hit_done || !g_aim_depth || !g_aim_ray_ready || !dsv) return;
    g_hit_done = true;
    if (g_hit_slot_frame[g_hit_next] || !hit_init()) return;  // every slot still in flight
    UINT w = 0, h = 0;
    ID3D11ShaderResourceView* srv = hit_depth_view(dsv, &w, &h);
    if (!srv) return;
    // The pixel centre from the bound (HUD) constants, as for the laser.
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kPixelCenterOffset + 16) return;
    const float* f = reinterpret_cast<const float*>(it->second.data());
    struct {
        float org[4], dir[4], head[12], px[4], py[4], projz[4], projw[4], pc[4], ray[4], nearz[4];
    } c{};
    static_assert(sizeof(c) == 12 * 16, "the shader's Hit constants");
    const vr::Pose& q = g_laser_pose;  // the hand in the game camera's eye space (game units)
    for (int i = 0; i < 3; ++i) c.org[i] = q.m[i][3], c.dir[i] = -q.m[i][2];
    c.org[3] = 1;
    memcpy(c.head, g_eyes[0].head, sizeof(c.head));
    memcpy(c.px, g_eyes[0].px, 16);
    memcpy(c.py, g_eyes[0].py, 16);
    memcpy(c.projz, g_last_proj + 8, 16);
    memcpy(c.projw, g_last_proj + 12, 16);
    memcpy(c.pc, f + kPixelCenterOffset / 4, 16);
    c.ray[0] = g_hit_min / vr::unit();
    c.ray[1] = std::log(g_hit_max / g_hit_min) / float(kHitSamples - 1);
    c.ray[2] = float(w), c.ray[3] = float(h);
    {  // the depth of a point g_hit_ignore metres straight ahead of the eye
        const float zn = -g_hit_ignore / vr::unit();
        const float z = c.projz[2] * zn + c.projz[3], pw = c.projw[2] * zn + c.projw[3];
        c.nearz[0] = pw > 0 ? (pw * c.pc[3] - z * c.pc[2]) / pw : 1e30f;
    }
    g_ctx->UpdateSubresource(g_hit_cb, 0, nullptr, &c, 0, 0);

    // The depth can't be read while it is bound for output: unbind the targets for the pass.
    ID3D11RenderTargetView* old_rtv[kMaxTargets] = {};
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_ctx->OMGetRenderTargets(kMaxTargets, old_rtv, &old_dsv);
    ID3D11ComputeShader* old_cs = nullptr;
    ID3D11ShaderResourceView* old_srv = nullptr;
    ID3D11UnorderedAccessView* old_uav[2] = {};
    ID3D11Buffer* old_cb = nullptr;
    g_ctx->CSGetShader(&old_cs, nullptr, nullptr);
    g_ctx->CSGetShaderResources(0, 1, &old_srv);
    g_ctx->CSGetUnorderedAccessViews(0, 2, old_uav);
    g_ctx->CSGetConstantBuffers(0, 1, &old_cb);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    const UINT zero[4] = {};
    g_ctx->ClearUnorderedAccessViewUint(g_hit_uav, zero);
    g_ctx->CSSetShader(g_hit_cs, nullptr, 0);
    g_ctx->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uavs[2] = {g_hit_uav, g_hit_trace_uav};
    g_ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    g_ctx->CSSetConstantBuffers(0, 1, &g_hit_cb);
    g_ctx->Dispatch(1, 1, 1);
    ID3D11ShaderResourceView* no_srv = nullptr;
    g_ctx->CSSetShaderResources(0, 1, &no_srv);  // before the depth is bound for output again
    g_ctx->CSSetUnorderedAccessViews(0, 2, old_uav, nullptr);
    g_ctx->CSSetShaderResources(0, 1, &old_srv);
    g_ctx->CSSetConstantBuffers(0, 1, &old_cb);
    g_ctx->CSSetShader(old_cs, nullptr, 0);
    g_ctx->OMSetRenderTargets(kMaxTargets, old_rtv, old_dsv);
    for (IUnknown* p : {static_cast<IUnknown*>(old_cs), static_cast<IUnknown*>(old_srv), static_cast<IUnknown*>(old_uav[0]),
                        static_cast<IUnknown*>(old_uav[1]), static_cast<IUnknown*>(old_cb), static_cast<IUnknown*>(old_dsv)})
        if (p) p->Release();
    for (auto v : old_rtv)
        if (v) v->Release();

    if (g_aim_trace > 0) {  // every 8th sample: distance, its depth, the buffer's, the pixel
        g_ctx->CopyResource(g_hit_trace_staging, g_hit_trace);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_hit_trace_staging, 0, D3D11_MAP_READ, 0, &m))) {
            const float* t = static_cast<const float*>(m.pData);
            std::string s;
            for (UINT i = 0; i < kHitSamples; i += 8)
                appendf(s, " | %.2fm %.4f %.4f %.0f,%.0f", g_hit_min * std::exp(c.ray[1] * float(i)), t[i * 4], t[i * 4 + 1],
                        t[i * 4 + 2], t[i * 4 + 3]);
            g_ctx->Unmap(g_hit_trace_staging, 0);
            logf("aimtrace frame %u: ray %.2f,%.2f,%.2f dir %.2f,%.2f,%.2f depth %ux%u%s", g_frame, c.org[0], c.org[1],
                 c.org[2], c.dir[0], c.dir[1], c.dir[2], w, h, s.c_str());
        }
    }

    g_ctx->CopyResource(g_hit_staging[g_hit_next], g_hit_buf);
    g_hit_slot_frame[g_hit_next] = g_frame + 1;
    g_hit_slot_session[g_hit_next] = g_hit_session;
    g_hit_next = (g_hit_next + 1) % kHitRing;
}

// The results that are ready, oldest first; the newest of this aim session wins.
static void poll_aim_hits() {
    for (int k = 0; k < kHitRing; ++k) {
        const int s = (g_hit_next + k) % kHitRing;
        if (!g_hit_slot_frame[s]) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = g_ctx->Map(g_hit_staging[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;  // and the newer ones too
        const unsigned frame = g_hit_slot_frame[s] - 1;
        const bool mine = g_hit_slot_session[s] == g_hit_session;
        g_hit_slot_frame[s] = 0;
        if (FAILED(hr)) continue;
        uint32_t r[2];
        memcpy(r, m.pData, sizeof(r));
        g_ctx->Unmap(g_hit_staging[s], 0);
        if (!mine) continue;
        // Nothing new when the ray starts behind something (a hand inside a wall) or leaves the
        // eye's view (or goes behind the gun) before meeting anything: the last hit stays a while.
        const unsigned hit = r[0] ? kHitSamples - r[0] : kHitSamples;
        if (hit == 0 || (hit == kHitSamples && r[1] < kHitSamples)) continue;
        const float step = std::log(g_hit_max / g_hit_min) / float(kHitSamples - 1);
        // Between the sample before the first one behind a surface and that one; the far end if none.
        g_hit_m = hit < kHitSamples ? g_hit_min * std::exp(step * (float(hit) - 0.5f)) : g_hit_max;
        if (!g_hit_valid) logf("aim: ray hits at %.2f m", g_hit_m);
        g_hit_valid = true;
        g_hit_frame = frame;
    }
}

// Where the hand's ray meets the scene (metres along it), from a recent frame. False: unknown.
static bool aim_hit_distance(float* metres) {
    if (!g_hit_cs) return false;
    poll_aim_hits();
    if (!g_hit_valid || g_frame - g_hit_frame > 8) return false;
    *metres = g_hit_m;
    if (g_aim_trace > 0) logf("aimtrace frame %u: ray hit %.2f m (frame %u)", g_frame, g_hit_m, g_hit_frame);
    return true;
}

// Laser-pointer screens (the silhouette shot that starts a target): the game's dot goes where
// the hand's ray meets its plane in front of the game camera, and the laser runs from the
// controller to it. Outside aim mode the player's frame is the game camera's eye space.
static void steer_pointer(float plane) {
    vr::Pose hand;
    if (!vr::hand_pose(&hand)) return;
    const float u = vr::unit();
    const float o[3] = {hand.m[0][3] / u, hand.m[1][3] / u, hand.m[2][3] / u};
    const float d[3] = {-hand.m[0][2], -hand.m[1][2], -hand.m[2][2]};
    if (d[2] > -0.05f) return;  // not pointing towards the screen: the game's own controls
    const float t = (-plane - o[2]) / d[2];
    pointer_set(true, o[0] + t * d[0], o[1] + t * d[1]);
    g_pointer_mode = true;
    g_laser_pose = vr::scaled(hand, 1 / u);
    g_laser_from = 0.02f, g_laser_to = t * u;
    g_laser_ready = g_laser_length > 0;
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
        g_pivot_frames = g_pivot_still = 0;
        g_pivot_locked = false;
        g_prev_cam_valid = false;
        ++g_hit_session;
        g_hit_valid = false;
    } else if (aim) {
        ++g_pivot_frames;
    }
    g_pivot_seen = false;
    g_grip_last_valid = g_grip_seen;
    if (g_grip_seen) memcpy(g_grip_last, g_grip_now, sizeof(g_grip_last));
    g_grip_seen = false;
    if (!aim) g_anchor_set = false;
    g_aim_mode = aim;
    g_gun_run = 0;
    g_relocked = false;
    g_laser_ready = g_laser_done = false;
    g_aim_ray_ready = g_hit_done = false;
    g_letterbox_on = false;
    if (g_draw_cb[0]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
    const float ratio = g_last_proj[0] != 0 ? g_last_proj[5] / g_last_proj[0] : 0;
    vr::begin_frame(ratio, g_eyes);
    if (aim) steer_aim();
    float plane = 0;
    const bool pointer = pointer_active(&plane) && vr::stereo() && !aim;
    if (pointer != g_pointer_mode) logf("stereo: laser pointer %s", pointer ? "on" : "off");
    vr::suspend_inflation(pointer);
    g_pointer_mode = false;
    pointer_set(false, 0, 0);
    if (pointer) steer_pointer(plane);
    for (int eye = 0; eye < 2; ++eye) g_ctx->UpdateSubresource(g_eye_cb[eye], 0, nullptr, &g_eyes[eye], 0, 0);
    if (g_gv_cb) {
        vr::EyeConsts gv;
        vr::game_view(ratio, &gv);
        g_ctx->UpdateSubresource(g_gv_cb, 0, nullptr, &gv, 0, 0);
        // The frustum this frame's game view is drawn with (the pair log places it in the eye;
        // the camera's FOV can change later in the frame).
        g_gv_tan[0] = gv.flags[0] && gv.px[0] ? 1 / gv.px[0] : 0;
        g_gv_tan[1] = gv.flags[0] && gv.py[1] ? 1 / gv.py[1] : 0;
        if (g_own_cb) {  // the same view with the draw's own projection
            gv.flags[0] = 0;
            g_ctx->UpdateSubresource(g_own_cb, 0, nullptr, &gv, 0, 0);
        }
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

// Twins and views dropped mid-frame may still be bound, or picked for this draw's other slots
// or for the frame's hand-off to the headset: they are released after the frame is presented.
static std::vector<IUnknown*> g_retired;
static void retire(IUnknown* p) {
    if (p) g_retired.push_back(p);
}
static void release_retired() {
    for (IUnknown* p : g_retired) p->Release();
    g_retired.clear();
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
        // The game freed this texture and made a different one at the same address: drop the
        // old twin and the views of it (other textures' twin views stay valid).
        ID3D11Texture2D* old = it->second;
        g_twins.erase(it);
        for (auto v = g_twin_views.begin(); v != g_twin_views.end();) {
            ID3D11Resource* of = nullptr;
            v->second->GetResource(&of);
            of->Release();
            if (of == old) {
                retire(v->second);
                v = g_twin_views.erase(v);
            } else {
                ++v;
            }
        }
        retire(old);
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
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    res->Release();
    ID3D11Texture2D* twin = twin_tex(res, create);
    // The game frees views and makes new ones, and a new view can land at a freed one's address
    // while its texture is new: a cached entry only counts if it is a view of this texture's twin.
    auto it = g_twin_views.find(view);
    if (it != g_twin_views.end()) {
        ID3D11Resource* cached = nullptr;
        it->second->GetResource(&cached);
        cached->Release();
        if (twin && cached == twin) return static_cast<V*>(it->second);
        retire(it->second);
        g_twin_views.erase(it);
    }
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

// True when the bound pixel shader samples a texture an eye rendered, or a frame-sized copy of
// one (resized like the eye targets), rather than HUD artwork: a screen-image effect over the
// scene (the dimmed frame copy behind a name card, the frame redrawn in strips before the static
// between scenes).
struct Resized;
static const Resized* resized_info(ID3D11Resource* res);

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
            eye = twin_tex(res, false) != nullptr || resized_info(res) != nullptr;
            res->Release();
        }
        s->Release();
    }
    return eye;
}

// `hudlog N`: for N frames, log each draw that goes to the HUD overlay and what it samples.
static int g_hud_log;
void request_hud_log(int frames) { g_hud_log = frames; }

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

// Whether this perspective draw's projection is the inflated one (fov_inflate), so the game view
// should project it with the FOV the game meant. Cameras that don't go through the inflation stub
// (the TV's portrait camera, about 33 degrees) keep their own projection. True when unknown.
static bool draw_proj_inflated() {
    const float inflated = fov_inflate_current();
    if (inflated <= 0) return true;
    ID3D11Buffer* cb = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return true;
    auto it = g_cb.find(cb);
    cb->Release();
    if (it == g_cb.end() || it->second.size() < kTemplateCbMin) return true;
    const float* p = reinterpret_cast<const float*>(it->second.data() + kProjOffset);
    if (!(p[12] == 0 && p[13] == 0 && p[14] == -1 && p[15] == 0) || p[5] == 0) return true;
    const float t = 1 / p[5], ti = tanf(inflated * 3.14159265f / 360);
    return fabsf(t / ti - 1) < 0.02f;
}

// Whether the bound pixel shader reads `tex` (only the slots it declares).
static bool samples_texture(ID3D11Resource* tex) {
    if (!tex) return false;
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
    bool found = false;
    for (UINT i = 0; i < kMaxTextures; ++i) {
        if (!srv[i]) continue;
        if (!found && (slots & (1u << i))) {
            ID3D11Resource* res = nullptr;
            srv[i]->GetResource(&res);
            found = res == tex;
            res->Release();
        }
        srv[i]->Release();
    }
    return found;
}

static bool ps_in(const std::vector<uint64_t>& list) {
    if (list.empty()) return false;
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    const uint64_t h = shader_hash(ps);
    if (ps) ps->Release();
    for (uint64_t c : list)
        if (c == h) return true;
    return false;
}

static bool is_card_shader() { return ps_in(g_card_ps); }

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

// Where the game camera's (intended) frustum lands in a width x height image of `eye`: rect =
// left, top, right, bottom pixels. False when the game isn't inflating its FOV (no intended
// frustum to place) or its projection isn't known yet; an empty rect when it's out of view.
static bool game_view_in_eye(int eye, int width, int height, int rect[4]) {
    const float intended = fov_inflate_intended();
    const float ratio = g_last_proj[0] != 0 ? g_last_proj[5] / g_last_proj[0] : 0;
    if (intended <= 0 || ratio <= 0) return false;
    const float tan_y = tanf(intended * 3.14159265f / 360);
    vr::frustum_scissor(g_eyes[eye].head, g_eyes[eye].px, g_eyes[eye].py, tan_y * ratio, tan_y, width, height, rect);
    return true;
}

// Draws with a scissor limited to where the game camera's (intended) frustum lands in `eye`.
template <typename DrawFn>
static void draw_scissored(int eye, DrawFn draw) {
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    g_ctx->RSGetViewports(&nvp, &vp);
    int r[4];
    if (nvp == 0 || !game_view_in_eye(eye, int(vp.Width), int(vp.Height), r)) {
        draw();
        return;
    }
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
// The game never learns: GetDesc called from the game's code reports the size it asked for, so
// everything it derives from sizes (its frame readback buffers and the pixel reads its hit tests
// make, copy regions, the window blit) stays as in the unmodded game at any eye resolution.
// Copies between resized textures get their regions scaled; the game-view targets (what its
// readbacks read) are the original size.

struct Resized {
    float sx, sy;   // real / original
    UINT ow, oh;    // the size the game asked for
    UINT rw, rh;    // the real size
};
// Read from any thread (GetDesc, the game's loader creating textures), written by texture
// creation: behind a lock. Entries are nodes, so pointers to them stay valid until their own
// erase, which only happens once the texture is gone and its address is reused.
static std::unordered_map<ID3D11Resource*, Resized> g_resized;
static SRWLOCK g_resized_lock = SRWLOCK_INIT;
static UINT g_orig_w, g_orig_h;  // the size the game asked for (its 4:3 targets)
static uintptr_t g_exe_lo, g_exe_hi;

static const Resized* resized_info(ID3D11Resource* res) {
    if (!res) return nullptr;
    AcquireSRWLockShared(&g_resized_lock);
    auto it = g_resized.find(res);
    const Resized* r = it == g_resized.end() ? nullptr : &it->second;
    ReleaseSRWLockShared(&g_resized_lock);
    return r;
}

static bool any_resized() {
    AcquireSRWLockShared(&g_resized_lock);
    const bool any = !g_resized.empty();
    ReleaseSRWLockShared(&g_resized_lock);
    return any;
}

using Tex2DGetDesc_t = void(STDMETHODCALLTYPE*)(ID3D11Texture2D*, D3D11_TEXTURE2D_DESC*);
static Tex2DGetDesc_t o_Tex2DGetDesc;

static void STDMETHODCALLTYPE hk_Tex2DGetDesc(ID3D11Texture2D* tex, D3D11_TEXTURE2D_DESC* desc) {
    o_Tex2DGetDesc(tex, desc);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller < g_exe_lo || caller >= g_exe_hi) return;  // us, d3d11, the SteamVR compositor: the truth
    if (const Resized* r = resized_info(tex))
        if (desc->Width == r->rw && desc->Height == r->rh) desc->Width = r->ow, desc->Height = r->oh;
}

using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                      const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t o_CreateTexture2D;

// Whether a call comes from this DLL (the VR hand-off's copies, twins, the HUD target).
static bool called_from_self(uintptr_t caller) {
    static uintptr_t lo, hi;
    if (!lo) {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&called_from_self), &m);
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof(mi));
        lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        hi = lo + mi.SizeOfImage;
    }
    return caller >= lo && caller < hi;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
                                                    const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    unsigned w = 0, h = 0;
    // Frame-sized (4:3) textures without initial data: render targets and the copies the game
    // makes of them. Not textures the CPU reads (its readback staging keeps the original size)
    // and not loaded artwork (which comes with data). Ours are made as asked: a copy of the
    // original-size game view (a menu screen's hand-off) resized here no longer matched it, and
    // was made again every frame.
    if (d == g_dev && desc && !init && !desc->CPUAccessFlags && desc->Usage == D3D11_USAGE_DEFAULT &&
        !called_from_self(reinterpret_cast<uintptr_t>(_ReturnAddress())) &&
        desc->Width * 3 == desc->Height * 4 && desc->Height >= 480 && vr::eye_resolution(&w, &h)) {
        D3D11_TEXTURE2D_DESC nd = *desc;
        nd.Width = w;
        nd.Height = h;
        // K7VR_REFUSE_EYE_TARGETS=1 (tests): act as a driver that refuses the eye size.
        static const bool refuse = GetEnvironmentVariableW(L"K7VR_REFUSE_EYE_TARGETS", nullptr, 0) > 0;
        HRESULT hr = refuse ? E_OUTOFMEMORY : o_CreateTexture2D(d, &nd, init, out);
        if (SUCCEEDED(hr) && out && *out) {
            AcquireSRWLockExclusive(&g_resized_lock);
            g_resized[*out] = {float(w) / float(desc->Width), float(h) / float(desc->Height), desc->Width, desc->Height, w, h};
            ReleaseSRWLockExclusive(&g_resized_lock);
            g_orig_w = desc->Width;
            g_orig_h = desc->Height;
            vr::set_content_aspect(float(desc->Width) / float(desc->Height));
            if (++g_eye_targets_made <= 50)
                logf("eye targets: %ux%u f%d created at %ux%u", desc->Width, desc->Height, desc->Format, w, h);
            if (g_eye_targets_made == 50) logf("eye targets: 50 made; not logging more");
            if (!o_Tex2DGetDesc) {  // all 2D textures share the vtable: hook once
                patch_vtbl(*out, vt::tex2d_GetDesc, reinterpret_cast<void*>(&hk_Tex2DGetDesc),
                           reinterpret_cast<void**>(&o_Tex2DGetDesc));
                MODULEINFO mi{};
                GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
                g_exe_lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
                g_exe_hi = g_exe_lo + mi.SizeOfImage;
            }
            return hr;
        }
        // The driver refused the eye size (a driver may refuse what another accepts): the game
        // gets its target at the size it asked for rather than a failure it doesn't expect.
        static int refused;
        if (refused++ < 10)
            logf("eye targets: %ux%u f%d samples %u bind 0x%x REFUSED at %ux%u (0x%08x); made at the game's size",
                 desc->Width, desc->Height, desc->Format, desc->SampleDesc.Count, desc->BindFlags, w, h, unsigned(hr));
    }
    // A texture the game creates "like" a resized one (it sees the original size) is made at
    // that size: its readback staging textures, copy destinations.
    const HRESULT hr = o_CreateTexture2D(d, desc, init, out);
    if (SUCCEEDED(hr) && out && *out && resized_info(*out)) {  // a freed resized texture's address, reused
        AcquireSRWLockExclusive(&g_resized_lock);
        g_resized.erase(*out);
        ReleaseSRWLockExclusive(&g_resized_lock);
    }
    return hr;
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
    // A 3D draw over (nearly) the whole frame, in the eyes: the whole eye image. The game frames
    // cutscenes with a shifted viewport and a letterbox scissor; in the headset that would cut
    // the view and move it off the lens centre. (Small viewports, picture-in-picture views, stay.)
    void fill_eye() const {
        if (!active || !nvp || vp[0].Width < 0.9f * g_orig_w || vp[0].Height < 0.7f * g_orig_h) return;
        // Remember the letterbox band: the bars the game draws over the rest are left out of the eyes.
        if (nsr && (ssr[0].top > 0 || ssr[0].bottom < LONG(rh)) && ssr[0].right - ssr[0].left >= LONG(0.9f * rw))
            g_letterbox = ssr[0], g_letterbox_on = true;
        const D3D11_VIEWPORT full{0, 0, float(rw), float(rh), vp[0].MinDepth, vp[0].MaxDepth};
        const D3D11_RECT all{0, 0, LONG(rw), LONG(rh)};
        g_ctx->RSSetViewports(1, &full);
        g_ctx->RSSetScissorRects(1, &all);
    }
    UINT rw = 0, rh = 0;  // the real size of the render target
    D3D11_VIEWPORT svp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];  // scaled
    D3D11_RECT ssr[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

    explicit ViewportScale(ID3D11Resource* rt) {
        const Resized* r = resized_info(rt);
        if (!r) return;
        const float sx = r->sx, sy = r->sy;
        rw = r->rw, rh = r->rh;
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
// is also drawn from the game camera with its own projection and viewports into game-view
// targets of the original size, and the game's copies out of the scene targets into textures of
// the original size (its readback staging, which it sizes from what GetDesc tells it) read from those.

static std::unordered_map<ID3D11Resource*, ID3D11Texture2D*> g_gv_tex;  // scene texture -> game view
static std::unordered_map<ID3D11View*, ID3D11View*> g_gv_views;

static ID3D11Texture2D* gv_tex(ID3D11Resource* res, bool create) {
    if (!res) return nullptr;
    auto it = g_gv_tex.find(res);
    if (it == g_gv_tex.end() && !create) return nullptr;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    D3D11_TEXTURE2D_DESC d;  // the size the game asked for, like its readback copies
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    if (const Resized* r = resized_info(res)) d.Width = r->ow, d.Height = r->oh;
    if (it != g_gv_tex.end()) {
        D3D11_TEXTURE2D_DESC have;
        it->second->GetDesc(&have);
        if (have.Width == d.Width && have.Height == d.Height && have.Format == d.Format) return it->second;
        // The game freed this texture and made a different one at the same address.
        retire(it->second);
        g_gv_tex.erase(it);
        if (!create) return nullptr;
    }
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
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    res->Release();
    ID3D11Texture2D* t = gv_tex(res, create);
    auto it = g_gv_views.find(view);  // only a view of this texture's game view counts (see twin_view)
    if (it != g_gv_views.end()) {
        ID3D11Resource* cached = nullptr;
        it->second->GetResource(&cached);
        cached->Release();
        if (t && cached == t) return static_cast<V*>(it->second);
        retire(it->second);
        g_gv_views.erase(it);
    }
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
    scale.unscale();  // original-size targets: the game's own viewports
    g_ctx->OMSetRenderTargets(kMaxTargets, grtv, gdsv);
    ID3D11Buffer* cb = g_own_cb && draw_kind() == 1 && !draw_proj_inflated() ? g_own_cb : g_gv_cb;
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &cb);
    draw();
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
    scale.rescale();
}

// ---- The junction screen ----
// At a junction the game tints a copy of the frame red over the whole screen ([vr]
// junction_shaders: the tint's pixel shader), then draws over it: the persona's highlight,
// windows that show the copy untinted where each path goes, their white outlines, a preview
// inset, and the labels. All of it is screen-space 2D aimed at the game camera's picture. In the
// eyes, the full-screen filters (the tint, the highlight) cover the whole view; everything after
// the tint is drawn where the game camera's view lies in each eye (its rectangle there), so the
// outlines and labels sit on the paths they point at, flat and facing the player. The windows
// sample the frame copy at screen positions; in each eye they get a crop of that eye's own
// picture at the rectangle instead, so what shows through them is the eye's 3D view.
static std::vector<uint64_t> g_junction_ps;
static bool g_junction_now;   // this frame drew the junction's tint (the junction is showing)
static bool g_junction_shown; // ...the last frame did (logs changes)

struct EyeCrop {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    unsigned frame = ~0u;
    ID3D11Resource* src = nullptr;
};
static EyeCrop g_crop[2];

// `src` (an eye-sized picture) cropped to rectangle `r` of it, black where `r` leaves it.
static ID3D11ShaderResourceView* eye_crop(int eye, ID3D11Resource* src, const int r[4]) {
    D3D11_RESOURCE_DIMENSION dim;
    src->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    D3D11_TEXTURE2D_DESC sd;
    static_cast<ID3D11Texture2D*>(src)->GetDesc(&sd);  // (our call: the real size)
    const int w = r[2] - r[0], h = r[3] - r[1];
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || sd.SampleDesc.Count != 1) return nullptr;
    EyeCrop& c = g_crop[eye];
    if (!c.tex || c.w != UINT(w) || c.h != UINT(h) || c.fmt != sd.Format) {
        if (c.srv) retire(c.srv);
        if (c.rtv) retire(c.rtv);
        if (c.tex) retire(c.tex);
        c = EyeCrop{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(w), d.Height = UINT(h), d.MipLevels = 1, d.ArraySize = 1, d.Format = sd.Format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(o_CreateTexture2D(g_dev, &d, nullptr, &c.tex)) ||
            FAILED(g_dev->CreateShaderResourceView(c.tex, nullptr, &c.srv)) ||
            FAILED(g_dev->CreateRenderTargetView(c.tex, nullptr, &c.rtv))) {
            if (c.srv) c.srv->Release();
            if (c.rtv) c.rtv->Release();
            if (c.tex) c.tex->Release();
            c = EyeCrop{};
            return nullptr;
        }
        c.w = UINT(w), c.h = UINT(h), c.fmt = sd.Format;
    }
    if (c.frame != g_frame || c.src != src) {
        const float black[4] = {0, 0, 0, 1};
        g_ctx->ClearRenderTargetView(c.rtv, black);
        const LONG l = std::max<LONG>(r[0], 0), t = std::max<LONG>(r[1], 0);
        const LONG rr = std::min<LONG>(r[2], LONG(sd.Width)), b = std::min<LONG>(r[3], LONG(sd.Height));
        if (l < rr && t < b) {
            const D3D11_BOX box{UINT(l), UINT(t), 0, UINT(rr), UINT(b), 1};
            g_ctx->CopySubresourceRegion(c.tex, 0, UINT(l - r[0]), UINT(t - r[1]), 0, src, 0, &box);
        }
        c.frame = g_frame, c.src = src;
    }
    return c.srv;
}

// One draw of the junction's overlay into both eyes at the game view's rectangle (see above).
// False when it can't be placed (then it is drawn as any other 2D draw).
template <typename DrawFn>
static bool draw_junction(DrawFn draw, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv,
                          const ViewportScale& scale) {
    if (!scale.active || !scale.nvp || g_gv_tan[0] <= 0 || g_gv_tan[1] <= 0 || !g_orig_w || !g_orig_h) return false;
    uint32_t slots = 0xFFFFFFFF;
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) {
        UINT size = sizeof(slots);
        if (FAILED(ps->GetPrivateData(kTextureSlotsGuid, &size, &slots))) slots = 0xFFFFFFFF;
        ps->Release();
    }
    ID3D11ShaderResourceView* srv[kMaxTextures] = {};
    ID3D11Resource* eye_src[kMaxTextures] = {};  // slots that read an eye-sized picture
    g_ctx->PSGetShaderResources(0, kMaxTextures, srv);
    for (UINT i = 0; i < kMaxTextures; ++i) {
        if (!srv[i] || !(slots & (1u << i))) continue;
        ID3D11Resource* res = nullptr;
        srv[i]->GetResource(&res);
        if (resized_info(res)) eye_src[i] = res;
        res->Release();  // (the game keeps it alive)
    }
    const UINT nvp = scale.nvp, nsr = scale.nsr;
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11RenderTargetView* trtv[kMaxTargets] = {};
        ID3D11DepthStencilView* tdsv = dsv;
        bool ok = true;
        for (UINT i = 0; i < kMaxTargets; ++i)
            if (rtv[i] && !(trtv[i] = eye ? twin_view(rtv[i], true) : rtv[i])) ok = false;
        if (eye && dsv && !(tdsv = twin_view(dsv, true))) ok = false;
        int r[4];
        if (!ok || !vr::frustum_rect_unclamped(g_eyes[eye].head, g_eyes[eye].px, g_eyes[eye].py, g_gv_tan[0],
                                               g_gv_tan[1], int(scale.rw), int(scale.rh), r))
            continue;
        const float sx = float(r[2] - r[0]) / float(g_orig_w), sy = float(r[3] - r[1]) / float(g_orig_h);
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_RECT srs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < nvp; ++i) {
            const D3D11_VIEWPORT& v = scale.vp[i];
            vps[i] = {r[0] + v.TopLeftX * sx, r[1] + v.TopLeftY * sy, v.Width * sx, v.Height * sy, v.MinDepth,
                      v.MaxDepth};
        }
        for (UINT i = 0; i < nsr; ++i) {
            const D3D11_RECT& s = scale.sr[i];
            srs[i] = {std::clamp<LONG>(LONG(r[0] + s.left * sx), 0, LONG(scale.rw)),
                      std::clamp<LONG>(LONG(r[1] + s.top * sy), 0, LONG(scale.rh)),
                      std::clamp<LONG>(LONG(r[0] + s.right * sx), 0, LONG(scale.rw)),
                      std::clamp<LONG>(LONG(r[1] + s.bottom * sy), 0, LONG(scale.rh))};
        }
        g_ctx->RSSetViewports(nvp, vps);
        if (nsr) g_ctx->RSSetScissorRects(nsr, srs);
        ID3D11ShaderResourceView* use[kMaxTextures];
        bool swapped = false;
        for (UINT i = 0; i < kMaxTextures; ++i) {
            use[i] = srv[i];
            if (eye && srv[i])
                if (auto t = twin_view(srv[i], false)) use[i] = t, swapped = true;
            if (!eye_src[i]) continue;
            ID3D11Resource* src = eye_src[i];
            if (eye)
                if (ID3D11Texture2D* t = twin_tex(src, false)) src = t;
            if (auto c = eye_crop(eye, src, r)) use[i] = c, swapped = true;
        }
        if (swapped) g_ctx->PSSetShaderResources(0, kMaxTextures, use);
        g_ctx->OMSetRenderTargets(kMaxTargets, trtv, tdsv);
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[eye]);
        draw();
        if (swapped) g_ctx->PSSetShaderResources(0, kMaxTextures, srv);
    }
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
    scale.rescale();
    for (auto s : srv)
        if (s) s->Release();
    return true;
}

// Junction windows and their outlines reach the edge of the view (draw_junction_projected):
// far corners pushed out to this many times the side edge's length.
constexpr float kJuncReach = 12;

// Raw bytes of `count` vertices from the game's mapped dynamic buffer (under an exception guard).
static bool copy_vertex_bytes(const uint8_t* first, size_t bytes, uint8_t* out) {
    __try {
        memcpy(out, first, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The bound 2D draw's affine map from its screen coordinates to NDC (template constants: model
// view rows at 0, projection at kProjOffset), at depth z: ndc = M * (x, y) + c.
static bool ortho_map(float z, float m[4], float c[2]) {
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return false;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (it == g_cb.end() || it->second.size() < kTemplateCbMin) return false;
    const float* mv = reinterpret_cast<const float*>(it->second.data());
    const float* p = mv + kProjOffset / 4;
    auto ndc = [&](float x, float y, float out[2]) {
        float v[4];
        for (int r = 0; r < 3; ++r) v[r] = mv[r * 4] * x + mv[r * 4 + 1] * y + mv[r * 4 + 2] * z + mv[r * 4 + 3];
        v[3] = 1;
        float clip[4];
        for (int r = 0; r < 4; ++r) clip[r] = p[r * 4] * v[0] + p[r * 4 + 1] * v[1] + p[r * 4 + 2] * v[2] + p[r * 4 + 3] * v[3];
        if (fabsf(clip[3]) < 1e-6f) return false;
        out[0] = clip[0] / clip[3], out[1] = clip[1] / clip[3];
        return true;
    };
    float o[2], ex[2], ey[2];
    if (!ndc(0, 0, o) || !ndc(1, 0, ex) || !ndc(0, 1, ey)) return false;
    m[0] = ex[0] - o[0], m[1] = ey[0] - o[0], m[2] = ex[1] - o[1], m[3] = ey[1] - o[1];
    c[0] = o[0], c[1] = o[1];
    return fabsf(m[0] * m[3] - m[1] * m[2]) > 1e-12f;
}

// Pushes far corners v0 and v3 of a junction quad out along its side edges (v1->v0, v2->v3).
// False when the corners don't look like a window (near edge v1-v2 nearest the screen centre).
static bool extend_window(const float in[8], float out[8]) {
    auto d = [&](int i) { return hypotf(in[i * 2] - 320, in[i * 2 + 1] - 240); };
    if (std::max(d(1), d(2)) >= std::min(d(0), d(3))) return false;
    memcpy(out, in, 8 * sizeof(float));
    for (int k = 0; k < 2; ++k) out[k] = in[2 + k] + kJuncReach * (in[k] - in[2 + k]);              // v0 from v1
    for (int k = 0; k < 2; ++k) out[6 + k] = in[4 + k] + kJuncReach * (in[6 + k] - in[4 + k]);      // v3 from v2
    return true;
}

// The junction's letterbox: an untextured indexed draw of quads that each span the screen's width
// along its top or bottom edge (two bars, 64 of 480 lines each). Meant for the flat screen; in the
// eyes it would be a black strip across the middle of the view, so it is left out there.
static bool is_letterbox_bars() {
    if (g_draw_first >= 0 || g_draw_base < 0 || g_draw_count == 0 || g_draw_count % 6 || g_draw_count > 24) return false;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return false;
    auto mapped = g_vb_mapped.find(vb);
    const uint8_t* base = mapped == g_vb_mapped.end() ? nullptr : mapped->second;
    vb->Release();
    const UINT n = g_draw_count / 6 * 4;
    uint8_t raw[16 * 64];
    if (!base || stride < 12 || stride > 64 || n > 16 ||
        !copy_vertex_bytes(base + offset + size_t(g_draw_base) * stride, size_t(stride) * n, raw))
        return false;
    float xy[16 * 2];
    for (UINT i = 0; i < n; ++i) memcpy(&xy[i * 2], raw + size_t(i) * stride, 8);
    float z;
    memcpy(&z, raw + 8, sizeof(z));
    float m[4], c[2];
    if (!ortho_map(z, m, c)) return false;
    for (UINT q = 0; q < n / 4; ++q) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (UINT i = q * 4; i < q * 4 + 4; ++i) {
            const float nx = m[0] * xy[i * 2] + m[1] * xy[i * 2 + 1] + c[0];
            const float ny = m[2] * xy[i * 2] + m[3] * xy[i * 2 + 1] + c[1];
            x0 = std::min(x0, nx), x1 = std::max(x1, nx), y0 = std::min(y0, ny), y1 = std::max(y1, ny);
        }
        const bool wide = x0 <= -0.99f && x1 >= 0.99f;
        const bool at_edge = y0 <= -0.99f || y1 >= 0.99f;
        if (!wide || !at_edge || y1 - y0 > 0.5f) return false;
    }
    return true;
}

// One 2D draw of the junction's overlay, into both eyes as if the game's picture were a flat
// sign at infinity in front of the game camera (vr::game_ndc_to_eye_clip): its vertices are
// mapped on the CPU (straight lines stay straight, the sign foreshortens as the head turns, what
// lies behind the eye is clipped off) and drawn with the game's own shaders over the whole eye.
// Windows and their outlines also reach the edge of the view: each window is a quad whose near
// corners (v1, v2) sit by the persona and whose far corners (v0, v3) lie just past the game's
// screen; its outline is an 8-vertex strip of corner pairs (v0..v3, each with a copy offset by
// the line's width). Their far corners are pushed out along the side edges. A window samples
// the frame copy at its screen position: in each eye it reads that eye's own pixel there, so its
// interior is the eye's 3D view; anything else that samples the copy (the preview inset) gets a
// crop of the eye's copy where the game camera's view lies. False when the draw isn't one this
// understands (then draw_junction).
static ID3D11Buffer* g_junc_vb;  // dynamic: the mapped vertices
constexpr UINT kJuncVbBytes = 64 * 1024;
static float g_junc_window[8];    // the last window's corners (game screen coordinates)
static unsigned g_junc_window_draw = ~0u;  // ...and the draw it came from

struct JVert {
    float u, v;         // the game picture's NDC (full frame, y up)
    float cx, cy, cw;   // this eye's clip space
    float z;            // the draw's own z
    uint8_t raw[64];    // the vertex as the game wrote it
};

static bool draw_junction_projected(bool eye_image, const ViewportScale& scale, ID3D11RenderTargetView* const* rtv,
                                    ID3D11DepthStencilView* dsv) {
    if (!scale.active || !scale.nvp || g_gv_tan[0] <= 0 || g_gv_tan[1] <= 0 || !g_orig_w || !g_orig_h) return false;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    const bool indexed = g_draw_first < 0;
    if (indexed && !(g_draw_count == 6 && g_draw_base >= 0 && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)) return false;
    if (!indexed && (g_draw_count == 0 || g_draw_count > 64)) return false;
    const bool lines = topo == D3D11_PRIMITIVE_TOPOLOGY_LINELIST || topo == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    if (!lines && topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
        return false;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return false;
    auto mapped = g_vb_mapped.find(vb);
    const uint8_t* base = mapped == g_vb_mapped.end() ? nullptr : mapped->second;
    const UINT n = indexed ? 4 : g_draw_count;
    static uint8_t src[64 * 64];
    if (!base || stride < 12 || stride > 64 ||
        !copy_vertex_bytes(base + offset + size_t(indexed ? g_draw_base : g_draw_first) * stride, size_t(stride) * n, src)) {
        vb->Release();
        return false;
    }
    auto at = [&](UINT i) { return reinterpret_cast<float*>(src + size_t(i) * stride); };
    // Primitives (vertex indices): an indexed quad's two triangles, strips unrolled (winding kept).
    std::vector<std::array<UINT, 3>> tris;
    std::vector<std::array<UINT, 2>> segs;
    if (indexed) {
        tris = {{0, 1, 2}, {0, 2, 3}};
    } else if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        for (UINT i = 0; i + 2 < n; i += 3) tris.push_back({i, i + 1, i + 2});
    } else if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
        for (UINT i = 0; i + 2 < n; ++i) tris.push_back(i % 2 ? std::array<UINT, 3>{i + 1, i, i + 2} : std::array<UINT, 3>{i, i + 1, i + 2});
    } else if (topo == D3D11_PRIMITIVE_TOPOLOGY_LINELIST) {
        for (UINT i = 0; i + 1 < n; i += 2) segs.push_back({i, i + 1});
    } else {
        for (UINT i = 0; i + 1 < n; ++i) segs.push_back({i, i + 1});
    }
    const bool full_vp = scale.vp[0].Width >= 0.95f * g_orig_w && scale.vp[0].Height >= 0.95f * g_orig_h;
    // Positions (game screen coordinates), windows and outlines pushed out.
    std::vector<float> xy(size_t(n) * 2);
    for (UINT i = 0; i < n; ++i) xy[i * 2] = at(i)[0], xy[i * 2 + 1] = at(i)[1];
    const bool window = indexed && eye_image && full_vp && stride >= 24;
    bool outline = false;
    if (window) {
        float corners[8], ext[8];
        for (UINT i = 0; i < 8; ++i) corners[i] = xy[i];
        bool screen_uv = true;  // texture coordinates = screen position (the copy is the whole screen)
        for (UINT i = 0; i < 4; ++i)
            if (fabsf(at(i)[4] - at(i)[0] / 640) > 0.02f || fabsf(at(i)[5] - at(i)[1] / 480) > 0.02f) screen_uv = false;
        if (screen_uv && extend_window(corners, ext)) {
            memcpy(g_junc_window, corners, sizeof(corners));
            g_junc_window_draw = g_draws;
            for (UINT i = 0; i < 8; ++i) xy[i] = ext[i];
        }
    } else if (!indexed && n == 8 && full_vp && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP &&
               g_junc_window_draw + 1 == g_draws) {
        outline = true;
        for (UINT i = 0; i < 4; ++i)
            if (fabsf(xy[i * 4] - g_junc_window[i * 2]) > 0.01f || fabsf(xy[i * 4 + 1] - g_junc_window[i * 2 + 1]) > 0.01f)
                outline = false;
        float ext[8];
        if (outline && extend_window(g_junc_window, ext))
            for (UINT i = 0; i < 8; ++i) {
                const UINT corner = i / 2;
                xy[i * 2] = ext[corner * 2] + (at(i)[0] - g_junc_window[corner * 2]);
                xy[i * 2 + 1] = ext[corner * 2 + 1] + (at(i)[1] - g_junc_window[corner * 2 + 1]);
            }
    }
    float m[4], c[2];
    if (!ortho_map(at(0)[2], m, c)) {
        vb->Release();
        return false;
    }
    const float det = m[0] * m[3] - m[1] * m[2];
    // The draw's NDC (over its viewport) -> the game picture's NDC (over the whole frame).
    const D3D11_VIEWPORT& gvp = scale.vp[0];
    const float su = gvp.Width / g_orig_w, ou = (2 * gvp.TopLeftX + gvp.Width) / g_orig_w - 1;
    const float sv = gvp.Height / g_orig_h, ov = 1 - (2 * gvp.TopLeftY + gvp.Height) / g_orig_h;
    // A sub-viewport's scissor (the preview inset) as a rectangle of the picture's NDC.
    bool scissor = false;
    float s_u0 = -1, s_u1 = 1, s_v0 = -1, s_v1 = 1;
    if (!full_vp && scale.nsr) {
        const D3D11_RECT& s = scale.sr[0];
        s_u0 = 2.f * s.left / g_orig_w - 1, s_u1 = 2.f * s.right / g_orig_w - 1;
        s_v0 = 1 - 2.f * s.bottom / g_orig_h, s_v1 = 1 - 2.f * s.top / g_orig_h;
        scissor = true;
    }
    // Vertex fields to blend along clipped edges: floats, except a packed colour at offset 12.
    const int color_slot = (stride == 16 || stride == 24 || stride == 28 || stride == 32) ? 3 : -1;
    auto lerp = [&](const JVert& a, const JVert& b, float t) {
        JVert o;
        o.u = a.u + (b.u - a.u) * t, o.v = a.v + (b.v - a.v) * t;
        o.cx = a.cx + (b.cx - a.cx) * t, o.cy = a.cy + (b.cy - a.cy) * t, o.cw = a.cw + (b.cw - a.cw) * t;
        o.z = a.z + (b.z - a.z) * t;
        for (UINT k = 0; k < stride / 4; ++k) {
            float fa, fb;
            memcpy(&fa, a.raw + k * 4, 4);
            memcpy(&fb, b.raw + k * 4, 4);
            const float f = int(k) == color_slot ? fa : fa + (fb - fa) * t;
            memcpy(o.raw + k * 4, &f, 4);
        }
        return o;
    };
    // Clips a polygon against plane f(v) >= 0 (f linear along edges).
    auto clip_poly = [&](std::vector<JVert>& poly, auto&& f) {
        std::vector<JVert> out;
        for (size_t i = 0; i < poly.size(); ++i) {
            const JVert& a = poly[i];
            const JVert& b = poly[(i + 1) % poly.size()];
            const float fa = f(a), fb = f(b);
            if (fa >= 0) out.push_back(a);
            if ((fa >= 0) != (fb >= 0)) out.push_back(lerp(a, b, fa / (fa - fb)));
        }
        poly.swap(out);
    };
    // Save what changes.
    uint32_t slots = 0xFFFFFFFF;
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) {
        UINT size = sizeof(slots);
        if (FAILED(ps->GetPrivateData(kTextureSlotsGuid, &size, &slots))) slots = 0xFFFFFFFF;
        ps->Release();
    }
    ID3D11ShaderResourceView* srv[kMaxTextures] = {};
    ID3D11Resource* eye_src[kMaxTextures] = {};
    g_ctx->PSGetShaderResources(0, kMaxTextures, srv);
    for (UINT i = 0; i < kMaxTextures; ++i) {
        if (!srv[i] || !(slots & (1u << i))) continue;
        ID3D11Resource* res = nullptr;
        srv[i]->GetResource(&res);
        if (resized_info(res)) eye_src[i] = res;
        res->Release();
    }
    if (!g_junc_vb) {
        D3D11_BUFFER_DESC bd{kJuncVbBytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_junc_vb))) {
            for (auto s : srv)
                if (s) s->Release();
            vb->Release();
            return false;
        }
    }
    const float W = float(scale.rw), H = float(scale.rh);
    constexpr float kNear = 1e-3f;
    bool drew = false;
    std::vector<uint8_t> out;
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11RenderTargetView* trtv[kMaxTargets] = {};
        ID3D11DepthStencilView* tdsv = dsv;
        bool ok = true;
        for (UINT i = 0; i < kMaxTargets; ++i)
            if (rtv[i] && !(trtv[i] = eye ? twin_view(rtv[i], true) : rtv[i])) ok = false;
        if (eye && dsv && !(tdsv = twin_view(dsv, true))) ok = false;
        if (!ok) continue;
        std::vector<JVert> verts(n);
        for (UINT i = 0; i < n; ++i) {
            JVert& jv = verts[i];
            const float dx = m[0] * xy[i * 2] + m[1] * xy[i * 2 + 1] + c[0];
            const float dy = m[2] * xy[i * 2] + m[3] * xy[i * 2 + 1] + c[1];
            jv.u = dx * su + ou, jv.v = dy * sv + ov;
            float clip[3];
            vr::game_ndc_to_eye_clip(g_eyes[eye].head, g_eyes[eye].px, g_eyes[eye].py, g_gv_tan[0], g_gv_tan[1], jv.u,
                                     jv.v, clip);
            jv.cx = clip[0], jv.cy = clip[1], jv.cw = clip[2];
            jv.z = at(i)[2];
            memcpy(jv.raw, src + size_t(i) * stride, stride);
        }
        out.clear();
        auto emit = [&](const JVert& jv) {
            uint8_t v[64];
            memcpy(v, jv.raw, stride);
            float* f = reinterpret_cast<float*>(v);
            const float nx = jv.cx / jv.cw, ny = jv.cy / jv.cw;  // this eye's NDC (the viewport is the whole eye)
            const float tx = nx - c[0], ty = ny - c[1];
            f[0] = (m[3] * tx - m[1] * ty) / det;
            f[1] = (-m[2] * tx + m[0] * ty) / det;
            f[2] = jv.z;
            if (window) f[4] = (nx + 1) / 2, f[5] = (1 - ny) / 2;  // the eye's own pixel
            out.insert(out.end(), v, v + stride);
        };
        auto fit = [&](std::vector<JVert>& poly) {
            if (scissor) {
                clip_poly(poly, [&](const JVert& p) { return p.u - s_u0; });
                clip_poly(poly, [&](const JVert& p) { return s_u1 - p.u; });
                clip_poly(poly, [&](const JVert& p) { return p.v - s_v0; });
                clip_poly(poly, [&](const JVert& p) { return s_v1 - p.v; });
            }
            clip_poly(poly, [&](const JVert& p) { return p.cw - kNear; });
        };
        for (const auto& t : tris) {
            std::vector<JVert> poly = {verts[t[0]], verts[t[1]], verts[t[2]]};
            fit(poly);
            for (size_t k = 1; k + 1 < poly.size(); ++k) emit(poly[0]), emit(poly[k]), emit(poly[k + 1]);
        }
        for (const auto& s : segs) {
            JVert a = verts[s[0]], b = verts[s[1]];
            auto cut = [&](auto&& f) {  // keeps the part of segment a-b where f >= 0
                const float fa = f(a), fb = f(b);
                if (fa < 0 && fb < 0) return false;
                if (fa < 0) a = lerp(a, b, fa / (fa - fb));
                else if (fb < 0) b = lerp(a, b, fa / (fa - fb));
                return true;
            };
            if (scissor && !(cut([&](const JVert& p) { return p.u - s_u0; }) && cut([&](const JVert& p) { return s_u1 - p.u; }) &&
                             cut([&](const JVert& p) { return p.v - s_v0; }) && cut([&](const JVert& p) { return s_v1 - p.v; })))
                continue;
            if (!cut([&](const JVert& p) { return p.cw - kNear; })) continue;
            emit(a), emit(b);
        }
        if (out.empty() || out.size() > kJuncVbBytes) {
            drew = true;  // (all of it behind the eye or outside the inset: nothing to draw in this eye)
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(g_junc_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, out.data(), out.size());
        g_ctx->Unmap(g_junc_vb, 0);
        const UINT zero = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_junc_vb, &stride, &zero);
        g_ctx->IASetPrimitiveTopology(lines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT full{0, 0, W, H, gvp.MinDepth, gvp.MaxDepth};
        const D3D11_RECT all{0, 0, LONG(scale.rw), LONG(scale.rh)};
        g_ctx->RSSetViewports(1, &full);
        g_ctx->RSSetScissorRects(1, &all);
        ID3D11ShaderResourceView* use[kMaxTextures];
        for (UINT i = 0; i < kMaxTextures; ++i) {
            use[i] = srv[i];
            if (eye_src[i]) {
                ID3D11Resource* src_eye = eye_src[i];
                if (eye)
                    if (ID3D11Texture2D* t = twin_tex(src_eye, false)) src_eye = t;  // the right eye's copy
                int r[4];
                if (window) {
                    if (eye)
                        if (auto t = twin_view(srv[i], false)) use[i] = t;
                } else if (vr::frustum_rect_unclamped(g_eyes[eye].head, g_eyes[eye].px, g_eyes[eye].py, g_gv_tan[0],
                                                      g_gv_tan[1], int(scale.rw), int(scale.rh), r)) {
                    // Anything else that reads the copy over the whole frame (the preview inset):
                    // this eye's picture where the game camera's view lies in it.
                    if (auto crop = eye_crop(eye, src_eye, r)) use[i] = crop;
                } else if (eye) {
                    if (auto t = twin_view(srv[i], false)) use[i] = t;
                }
            } else if (eye && srv[i]) {
                if (auto t = twin_view(srv[i], false)) use[i] = t;
            }
        }
        g_ctx->PSSetShaderResources(0, kMaxTextures, use);
        g_ctx->OMSetRenderTargets(kMaxTargets, trtv, tdsv);
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[eye]);
        g_ctx->Draw(UINT(out.size() / stride), 0);
        drew = true;
    }
    g_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g_ctx->IASetPrimitiveTopology(topo);
    g_ctx->PSSetShaderResources(0, kMaxTextures, srv);
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
    scale.rescale();
    for (auto s : srv)
        if (s) s->Release();
    vb->Release();
    return drew;
}

// ---- The static between scenes, around the player ----
// (docs/superpowers/specs/2026-11-20-static-surround-design.md.) After a junction or a door the
// game fills the screen with blue noise ([vr] static_shaders: its pixel shader), a 4-vertex strip
// over the whole 640x480 screen sampling a 640x480 noise texture through a small UV window that
// jumps every frame. Then it splits: two such strips, one from the top edge and one from the
// bottom, shrink towards the edges until they are bars of 64 lines each. In the eyes the noise is
// a sphere around the player (4 m, [vr] static_radius), fixed in the world from the moment the
// static starts: the whole sphere while it fills the screen, and as it splits, a zone above and
// one below whose edges lie at the elevation the game's bar edges show in its view. The static
// coming in over 3D frames (a full-screen noise over the picture in strips) stays as it was.
static std::vector<uint64_t> g_static_ps;
static bool g_static_on = true;          // [vr] static_surround
static float g_static_radius_m = 4;      // [vr] static_radius
static bool g_static_anchored;           // a transition is showing: the surround's anchor is set
static vr::Pose g_static_anchor;         // ...where (game camera space, game units)
static float g_static_tan[2];            // ...the game view's tangents it uses (frozen at the anchor)
static float g_static_last_tan[2];       // the last 3D frame's game-view tangents before it
static unsigned g_static_idle;           // frames since the last static draw
static unsigned g_static_logged;         // static draws logged since the anchor was set
static bool g_static_full_now;           // this frame drew the full static on the sphere (a 2D frame for the eyes)
static bool g_static_full_shown;         // ...the last frame did (logs changes)
static ID3D11Buffer* g_static_vb;
constexpr UINT kStaticVbBytes = 512 * 1024;
constexpr float kStaticCellDeg = 10;     // the sphere's cells (the noise is mapped per vertex, affinely)

struct SVert {
    float cx, cy, cw;  // this eye's clip space
    float u, v;        // the noise texture's coordinates
};

// The bound draw as the static's noise: its zone of the sphere as elevations (radians), and the
// noise window as an affine map from screen position to texture coordinates. False when it isn't.
struct StaticDraw {
    float ndc_lo, ndc_hi; // its extent's NDC y (the picture's -1..1, up)
    bool top, bottom;     // reaches the top / bottom edge
    bool full;            // covers the whole screen
    float uv[6];          // u = uv[0] x + uv[1] y + uv[2], v = uv[3] x + uv[4] y + uv[5]
    float z;
    UINT stride;
    uint8_t raw[64];      // the first vertex as the game wrote it (colour and any other fields)
};

static bool static_draw(StaticDraw& s) {
    if (!g_static_on || g_static_ps.empty() || g_draw_first < 0 || g_draw_count != 4 || !ps_in(g_static_ps))
        return false;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    g_ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    D3D11_TEXTURE2D_DESC td{};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
    res->Release();
    if (td.Width != 640 || td.Height != 480) return false;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return false;
    auto mapped = g_vb_mapped.find(vb);
    const uint8_t* base = mapped == g_vb_mapped.end() ? nullptr : mapped->second;
    vb->Release();
    uint8_t src[4 * 64];
    if (!base || stride < 24 || stride > 64 ||
        !copy_vertex_bytes(base + offset + size_t(g_draw_first) * stride, size_t(stride) * 4, src))
        return false;
    auto at = [&](int i) { return reinterpret_cast<const float*>(src + size_t(i) * stride); };
    float m[4], c[2];
    if (!ortho_map(at(0)[2], m, c)) return false;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float nx = m[0] * at(i)[0] + m[1] * at(i)[1] + c[0], ny = m[2] * at(i)[0] + m[3] * at(i)[1] + c[1];
        x0 = std::min(x0, nx), x1 = std::max(x1, nx), y0 = std::min(y0, ny), y1 = std::max(y1, ny);
    }
    if (x0 > -0.99f || x1 < 0.99f) return false;  // the full width, always
    s.top = y1 >= 0.99f, s.bottom = y0 <= -0.99f;
    if (!s.top && !s.bottom) return false;
    s.full = s.top && s.bottom;
    s.ndc_lo = y0, s.ndc_hi = y1;
    // Texture coordinates from screen position, through three of the corners.
    const float* a = at(0);
    const float* b = at(1);
    const float* d = at(2);
    const float e1x = b[0] - a[0], e1y = b[1] - a[1], e2x = d[0] - a[0], e2y = d[1] - a[1];
    const float det = e1x * e2y - e1y * e2x;
    if (fabsf(det) < 1e-6f) return false;
    for (int k = 0; k < 2; ++k) {
        const float du1 = b[4 + k] - a[4 + k], du2 = d[4 + k] - a[4 + k];
        const float gx = (du1 * e2y - du2 * e1y) / det, gy = (du2 * e1x - du1 * e2x) / det;
        float* o = s.uv + k * 3;
        o[0] = gx, o[1] = gy, o[2] = a[4 + k] - gx * a[0] - gy * a[1];
    }
    // The noise is a small window of its texture (spans 0.03-0.15 of it over the screen); the
    // map's full-screen artwork, also a 640x480 texture with this shader, spans all of it.
    if (fabsf(s.uv[0] * 640) >= 0.5f || fabsf(s.uv[4] * 480) >= 0.5f) return false;
    // A tile spans a whole screen (y 0..480); a bar's window, extended that far, may leave the
    // texture: slid back inside it (it is noise, so where the window sits doesn't matter).
    const float span = s.uv[4] * 480;
    const float lo = std::min(s.uv[5], s.uv[5] + span), hi = std::max(s.uv[5], s.uv[5] + span);
    if (hi - lo <= 1) {
        if (hi > 1) s.uv[5] -= hi - 1;
        else if (lo < 0) s.uv[5] -= lo;
    }
    s.z = a[2];
    s.stride = stride;
    memcpy(s.raw, src, stride);
    return true;
}

// Draws the bound static draw (`s`) on the sphere into both eyes with the game's own shaders,
// texture and states. False when it can't (then it is drawn as before).
static bool draw_static_surround(const StaticDraw& s, const ViewportScale& scale, ID3D11RenderTargetView* const* rtv,
                                 ID3D11DepthStencilView* dsv) {
    if (!scale.active || !scale.nvp || !g_static_anchored || g_static_tan[0] <= 0 || g_static_tan[1] <= 0) return false;
    float m[4], c[2];
    if (!ortho_map(s.z, m, c)) return false;
    const float det = m[0] * m[3] - m[1] * m[2];
    if (!g_static_vb) {
        D3D11_BUFFER_DESC bd{kStaticVbBytes, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_static_vb))) return false;
    }
    // Tiles: one game screen of noise each, the angle the screen spans in the frozen game view.
    const float pi = 3.14159265f;
    // (An even count, so the mirrored columns also meet without a seam behind the player.)
    const int n_az = std::max(2, 2 * int(lroundf(pi / (2 * atanf(g_static_tan[0])))));
    const int n_el = std::max(1, int(lroundf(pi / (2 * atanf(g_static_tan[1])))));
    const float tw = 2 * pi / n_az, th = pi / n_el;
    const float cell = kStaticCellDeg * pi / 180;
    const int k_az = std::max(1, int(ceilf(tw / cell))), k_el = std::max(1, int(ceilf(th / cell)));
    const float radius = g_static_radius_m / vr::unit();
    const float el_hi = s.top ? pi / 2 : vr::band_elevation(s.ndc_hi, g_static_tan[1]);
    const float el_lo = s.bottom ? -pi / 2 : vr::band_elevation(s.ndc_lo, g_static_tan[1]);
    const UINT stride = s.stride;
    constexpr float kNear = 1e-3f;

    ID3D11Buffer* vb = nullptr;
    UINT vb_stride = 0, vb_offset = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    ID3D11ShaderResourceView* srv[kMaxTextures] = {};
    g_ctx->PSGetShaderResources(0, kMaxTextures, srv);

    const float W = float(scale.rw), H = float(scale.rh);
    const D3D11_VIEWPORT& gvp = scale.vp[0];
    bool drew = false;
    std::vector<uint8_t> out;
    out.reserve(256 * 1024);
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11RenderTargetView* trtv[kMaxTargets] = {};
        ID3D11DepthStencilView* tdsv = dsv;
        bool ok = true;
        for (UINT i = 0; i < kMaxTargets; ++i)
            if (rtv[i] && !(trtv[i] = eye ? twin_view(rtv[i], true) : rtv[i])) ok = false;
        if (eye && dsv && !(tdsv = twin_view(dsv, true))) ok = false;
        if (!ok) continue;
        const vr::EyeConsts& e = g_eyes[eye];
        auto corner = [&](float az, float el, float tx, float ty) {
            float p[3], clip[3];
            vr::sphere_point(g_static_anchor, radius, az, el, p);
            vr::point_to_eye_clip(e.head, e.px, e.py, p, clip);
            SVert v;
            v.cx = clip[0], v.cy = clip[1], v.cw = clip[2];
            v.u = s.uv[0] * tx + s.uv[1] * ty + s.uv[2];
            v.v = s.uv[3] * tx + s.uv[4] * ty + s.uv[5];
            return v;
        };
        auto lerp = [](const SVert& a, const SVert& b, float t) {
            return SVert{a.cx + (b.cx - a.cx) * t, a.cy + (b.cy - a.cy) * t, a.cw + (b.cw - a.cw) * t,
                         a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
        };
        auto emit = [&](const SVert& sv) {
            uint8_t v[64];
            memcpy(v, s.raw, stride);
            float* f = reinterpret_cast<float*>(v);
            const float nx = sv.cx / sv.cw, ny = sv.cy / sv.cw;  // this eye's NDC (the viewport is the whole eye)
            const float tx = nx - c[0], ty = ny - c[1];
            f[0] = (m[3] * tx - m[1] * ty) / det;
            f[1] = (-m[2] * tx + m[0] * ty) / det;
            f[2] = s.z;
            f[4] = sv.u, f[5] = sv.v;
            out.insert(out.end(), v, v + stride);
        };
        auto triangle = [&](const SVert& a, const SVert& b, const SVert& d) {
            std::vector<SVert> poly = {a, b, d}, kept;
            for (size_t i = 0; i < poly.size(); ++i) {  // keep w >= kNear
                const SVert& p = poly[i];
                const SVert& q = poly[(i + 1) % poly.size()];
                const float fp = p.cw - kNear, fq = q.cw - kNear;
                if (fp >= 0) kept.push_back(p);
                if ((fp >= 0) != (fq >= 0)) kept.push_back(lerp(p, q, fp / (fp - fq)));
            }
            if (kept.size() < 3) return;
            bool left = true, right = true, below = true, above = true;  // wholly off one side of the eye
            for (const SVert& p : kept) {
                left = left && p.cx < -p.cw, right = right && p.cx > p.cw;
                below = below && p.cy < -p.cw, above = above && p.cy > p.cw;
            }
            if (left || right || below || above) return;
            for (size_t k = 1; k + 1 < kept.size(); ++k) emit(kept[0]), emit(kept[k]), emit(kept[k + 1]);
        };
        out.clear();
        // Rows of tiles with one centred on the horizon (the mirror lines away from eye level);
        // the rows at the poles are cut off there.
        const int k_max = int(ceilf((pi / 2 - th / 2) / th));
        for (int k = k_max; k >= -k_max - 1; --k) {
            const float tile_top = th / 2 + k * th;
            const int row = ((k % 2) + 2) % 2;
            for (int j = 0; j < k_el; ++j) {
                float el_a = tile_top - j * th / k_el, el_b = tile_top - (j + 1) * th / k_el;  // a above b
                el_a = std::min(el_a, el_hi), el_b = std::max(el_b, el_lo);
                if (el_a <= el_b) continue;
                // Alternate tiles are mirrored, so neighbours meet on the same texels (no seams).
                float ty_a = (tile_top - el_a) / th * 480, ty_b = (tile_top - el_b) / th * 480;
                if (row % 2) ty_a = 480 - ty_a, ty_b = 480 - ty_b;
                for (int col = 0; col < n_az; ++col) {
                    const float tile_left = -tw / 2 + col * tw;
                    for (int i = 0; i < k_az; ++i) {
                        const float az_a = tile_left + i * tw / k_az, az_b = tile_left + (i + 1) * tw / k_az;
                        float tx_a = float(i) / k_az * 640, tx_b = float(i + 1) / k_az * 640;
                        if (col % 2) tx_a = 640 - tx_a, tx_b = 640 - tx_b;
                        const SVert p00 = corner(az_a, el_a, tx_a, ty_a), p10 = corner(az_b, el_a, tx_b, ty_a);
                        const SVert p01 = corner(az_a, el_b, tx_a, ty_b), p11 = corner(az_b, el_b, tx_b, ty_b);
                        triangle(p00, p10, p11);
                        triangle(p00, p11, p01);
                    }
                }
            }
        }
        if (out.empty()) {
            drew = true;  // (all of it out of this eye's view)
            continue;
        }
        if (out.size() > kStaticVbBytes) continue;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(g_ctx->Map(g_static_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
        memcpy(ms.pData, out.data(), out.size());
        g_ctx->Unmap(g_static_vb, 0);
        const UINT zero = 0;
        g_ctx->IASetVertexBuffers(0, 1, &g_static_vb, &stride, &zero);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT full{0, 0, W, H, gvp.MinDepth, gvp.MaxDepth};
        const D3D11_RECT all{0, 0, LONG(scale.rw), LONG(scale.rh)};
        g_ctx->RSSetViewports(1, &full);
        g_ctx->RSSetScissorRects(1, &all);
        ID3D11ShaderResourceView* use[kMaxTextures];
        for (UINT i = 0; i < kMaxTextures; ++i) {
            use[i] = srv[i];
            if (eye && srv[i])
                if (auto t = twin_view(srv[i], false)) use[i] = t;
        }
        g_ctx->PSSetShaderResources(0, kMaxTextures, use);
        g_ctx->OMSetRenderTargets(kMaxTargets, trtv, tdsv);
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[eye]);
        g_ctx->Draw(UINT(out.size() / stride), 0);
        drew = true;
    }
    g_ctx->IASetVertexBuffers(0, 1, &vb, &vb_stride, &vb_offset);
    g_ctx->IASetPrimitiveTopology(topo);
    g_ctx->PSSetShaderResources(0, kMaxTextures, srv);
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
    g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
    scale.rescale();
    for (auto t : srv)
        if (t) t->Release();
    if (vb) vb->Release();
    return drew;
}

// Per frame, at the hand-off: the anchor's lifetime and the tangents it will freeze.
static void static_surround_frame_end() {
    if (g_static_full_now != g_static_full_shown)
        logf("stereo: static surround %s", g_static_full_now ? "on (full static in the eyes)" : "off");
    g_static_full_shown = g_static_full_now;
    g_static_full_now = false;
    if (g_static_anchored && ++g_static_idle >= 30) {
        g_static_anchored = false;
        logf("stereo: static surround released");
    }
    if (!g_static_anchored && g_frame_3d && g_gv_tan[0] > 0 && g_gv_tan[1] > 0)
        g_static_last_tan[0] = g_gv_tan[0], g_static_last_tan[1] = g_gv_tan[1];
}

static void log_hud_draw(const char* what = "hud") {
    ID3D11PixelShader* ps = nullptr;
    g_ctx->PSGetShader(&ps, nullptr, nullptr);
    uint32_t slots = 0xFFFFFFFF;
    if (ps) {
        UINT size = sizeof(slots);
        if (FAILED(ps->GetPrivateData(kTextureSlotsGuid, &size, &slots))) slots = 0xFFFFFFFF;
    }
    D3D11_PRIMITIVE_TOPOLOGY topo;
    g_ctx->IAGetPrimitiveTopology(&topo);
    std::string s;
    appendf(s, "hudlog frame %u %s: ps=%016llx topo=%d slots=0x%x", g_frame, what, shader_hash(ps), int(topo), slots);
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    g_ctx->RSGetViewports(&nvp, &vp);
    if (nvp) appendf(s, " vp=%.0f,%.0f %.0fx%.0f", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    if (ps) ps->Release();
    ID3D11ShaderResourceView* srv[kMaxTextures] = {};
    g_ctx->PSGetShaderResources(0, kMaxTextures, srv);
    for (UINT i = 0; i < kMaxTextures; ++i) {
        if (!srv[i]) continue;
        ID3D11Resource* res = nullptr;
        srv[i]->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d;
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            appendf(s, " t%u=%p(%ux%u f%d usage %d bind 0x%x%s%s%s)", i, (void*)res, d.Width, d.Height, d.Format,
                    int(d.Usage), d.BindFlags, g_twins.count(res) ? " twin" : "",
                    resized_info(res) ? " resized" : "", g_gv_tex.count(res) ? " gv-src" : "");
        }
        res->Release();
        srv[i]->Release();
    }
    logf("%s", s.c_str());
}

// Vertex positions (x, y at the start of each vertex) from mapped buffer memory; false if the
// memory can't be read (the mapping is the game's; reading it after Unmap is tolerated by the
// drivers but not promised, hence the guard).
static bool copy_xy(const uint8_t* first, UINT stride, UINT count, float* xy) {
    __try {
        for (UINT v = 0; v < count; ++v) memcpy(&xy[v * 2], first + size_t(v) * stride, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr UINT kCoverMaxVertices = 1024;  // larger 2D draws aren't measured (overlay_cover)

// Share of the screen the bound 2D draw's extent covers (a non-indexed draw of up to
// kCoverMaxVertices from a mapped dynamic buffer, placed by its template constants: quads, the
// scanline effect's lines), or -1 when that can't be told.
static float cover_of(INT first, UINT count);
static float overlay_cover() {
    if (g_draw_first < 0 || g_draw_count == 0 || g_draw_count > kCoverMaxVertices) return -1;
    return cover_of(g_draw_first, g_draw_count);
}

// The same for a 6-index quad too (its four vertices from the base vertex). Used only where a
// known extent is wanted for indexed quads (the junction screen); overlay_cover keeps treating
// indexed draws as unknown.
static float quad_cover() {
    if (g_draw_first >= 0) return overlay_cover();
    if (g_draw_count != 6 || g_draw_base < 0) return -1;
    return cover_of(g_draw_base, 4);
}

static float cover_of(INT first, UINT count) {
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    g_ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return -1;
    auto mapped = g_vb_mapped.find(vb);
    const uint8_t* base = mapped == g_vb_mapped.end() ? nullptr : mapped->second;
    vb->Release();
    ID3D11Buffer* cb0 = nullptr;
    g_ctx->VSGetConstantBuffers(0, 1, &cb0);
    if (!cb0) return -1;
    auto it = g_cb.find(cb0);
    cb0->Release();
    if (!base || stride < 8 || it == g_cb.end() || it->second.size() < kTemplateCbMin) return -1;
    static float xy[kCoverMaxVertices * 2];  // (render thread only)
    if (!copy_xy(base + offset + size_t(first) * stride, stride, count, xy)) return -1;
    const float* f = reinterpret_cast<const float*>(it->second.data());
    return vr::ortho_cover(xy, int(count), f, f + kProjOffset / 4);
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
    if (!vr::stereo() && !any_resized()) {
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
    // Flat frames (g_flat_frame): watch for the pattern in every frame.
    if (scene && kind == 1) g_frame_persp = true;
    if (!scene && g_frame_persp && g_scene_tex && rt0 != g_scene_tex && resized_info(rt0) &&
        samples_texture(g_scene_tex))
        g_scene_copied = true;
    if (scene && kind == 1 && g_flat_frame) {
        D3D11_TEXTURE2D_DESC sd;
        g_scene_tex = texture_of(rtv[0], &sd);
    }
    // The right eye: the same draw into the twins of the targets, with the twins of any
    // eye-rendered textures it samples, and the right eye's constants (`cb`).
    auto draw_right = [&](ID3D11Buffer* cb, auto&& fn) {
        ID3D11RenderTargetView* trtv[kMaxTargets] = {};
        for (UINT i = 0; i < kMaxTargets; ++i)
            if (rtv[i] && !(trtv[i] = twin_view(rtv[i], true))) return;
        ID3D11DepthStencilView* tdsv = dsv ? twin_view(dsv, true) : nullptr;
        if (dsv && !tdsv) return;
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
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &cb);
        fn();
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
        if (swapped) g_ctx->PSSetShaderResources(0, kMaxTextures, srv);
        g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
        for (auto s : srv)
            if (s) s->Release();
    };
    if (g_flat_frame) {
        // Both eyes get the game's picture: 3D draws from the game camera (with the FOV it meant,
        // or the draw's own projection for cameras it didn't inflate), 2D draws as issued, text
        // included. Not a 3D frame: no HUD, and the hand-off puts it on the screen panel.
        ID3D11Buffer* cb = kind == 1 ? (draw_proj_inflated() ? g_gv_cb : g_own_cb) : g_eye_cb[0];
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &cb);
        draw();
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
        draw_right(kind == 1 ? cb : g_eye_cb[1], draw);
        if (scene) draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }
    if (scene && kind == 1) {
        g_frame_3d = true;
        D3D11_TEXTURE2D_DESC sd;
        g_scene_tex = texture_of(rtv[0], &sd);
        if (scale.active && scale.nvp &&
            (!g_scene_vp_set || scale.vp[0].Width * scale.vp[0].Height > g_scene_vp.Width * g_scene_vp.Height)) {
            const D3D11_VIEWPORT& v = scale.vp[0];
            g_scene_vp = v;
            g_scene_band = {LONG(v.TopLeftX), LONG(v.TopLeftY), LONG(v.TopLeftX + v.Width), LONG(v.TopLeftY + v.Height)};
            ID3D11RasterizerState* rs = nullptr;
            g_ctx->RSGetState(&rs);
            if (rs) {
                D3D11_RASTERIZER_DESC rd;
                rs->GetDesc(&rd);
                rs->Release();
                if (rd.ScissorEnable && scale.nsr) g_scene_band = scale.sr[0];
            }
            g_scene_vp_set = true;
        }
    }
    // The static between scenes: on a sphere around the player (draw_static_surround). Full-screen
    // noise over a 3D frame only once the full static has anchored the surround (the next scene's
    // first frames, under the noise); before that it is the static coming in, left as it was.
    if (scene && kind == 2 && g_static_on) {
        StaticDraw sd;
        if (static_draw(sd) && (!sd.full || !g_frame_3d || g_static_anchored)) {
            if (!g_static_anchored) {
                g_static_anchor = vr::surround_anchor(g_eyes[0].head, g_eyes[1].head);
                const bool last = g_static_last_tan[0] > 0 && g_static_last_tan[1] > 0;
                g_static_tan[0] = last ? g_static_last_tan[0] : g_gv_tan[0];
                g_static_tan[1] = last ? g_static_last_tan[1] : g_gv_tan[1];
                g_static_anchored = true;
                g_static_logged = 0;
                logf("stereo: static surround anchored (game view tangents x %.4f y %.4f)", g_static_tan[0], g_static_tan[1]);
            }
            g_static_idle = 0;
            static float last_lo, last_hi;
            static bool last_3d;
            if (g_static_logged < 64 &&  // (per transition: the zones as the static splits, when they change)
                (g_static_logged == 0 || sd.ndc_lo != last_lo || sd.ndc_hi != last_hi || g_frame_3d != last_3d)) {
                ++g_static_logged;
                last_lo = sd.ndc_lo, last_hi = sd.ndc_hi, last_3d = g_frame_3d;
                logf("stereo: static draw frame %u %s ndc y %.3f..%.3f%s", g_frame,
                     sd.full ? "full" : sd.top ? "top" : "bottom", sd.ndc_lo, sd.ndc_hi, g_frame_3d ? " (3D frame)" : "");
            }
            if (draw_static_surround(sd, scale, rtv, dsv)) {
                if (sd.full) g_static_full_now = true;
                draw_game_view(draw, rtv, dsv, scale);
                release_targets();
                return;
            }
        }
    }
    const bool eye_image = scene && kind == 2 && samples_eye_image();
    if (eye_image && !g_frame_3d && g_prev_frame_3d) {
        // A screen-fixed backdrop made from last frame's image (drawn before the 3D scene, which
        // normally covers it). With head tracking it shows wherever geometry doesn't: skip it in
        // the eyes (the game's own view keeps it). Only while 3D frames are being drawn: in 2D
        // screens such image draws are the picture itself (the Options TV bends its text this way).
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }
    // The junction screen: from its tint on, the 2D draws that aren't full-screen filters go into
    // the eyes as a flat sign in front of the game camera (draw_junction_projected), not to the
    // HUD panel.
    if (scene && kind == 2 && g_frame_3d) {
        if (!g_junction_now && ps_in(g_junction_ps)) {
            g_junction_now = true;  // (the tint itself: a full-screen filter, drawn below)
        } else if (g_junction_now) {
            const bool full_vp =
                scale.nvp && scale.vp[0].Width >= 0.95f * g_orig_w && scale.vp[0].Height >= 0.95f * g_orig_h;
            if (!(full_vp && quad_cover() >= kFadeCover)) {  // (full-screen filters: below; the inset fills its own viewport)
                // The letterbox bars stay out of the eyes.
                bool done = false;
                if (full_vp && !pixel_shader_textured() && is_letterbox_bars())
                    done = true;
                else
                    done = draw_junction_projected(eye_image, scale, rtv, dsv);
                if (done || draw_junction(draw, rtv, dsv, scale)) {
                    draw_game_view(draw, rtv, dsv, scale);
                    release_targets();
                    return;
                }
            }
        }
    }
    // HUD: textured 2D artwork and point/line overlays (sparkles, reticle lines) after the 3D
    // scene. Untextured triangle quads (dims, fades, clears) and screen-image effects stay per eye.
    // (A textured quad over the whole screen is a full-screen effect, like the static that fades in
    // between scenes: it stays in the eyes, over the whole view, as fades do. kFadeCover.)
    if (scene && kind == 2 && g_frame_3d && !eye_image && (pixel_shader_textured() || points_or_lines()) &&
        !(overlay_cover() >= kFadeCover) && hud_target_like(rtv[0])) {  // once, into the overlay
        // The laser goes in after the whole scene (it doesn't write depth, so later draws would
        // cover it) and before the HUD.
        if (g_hud_log > 0) log_hud_draw();
        if (g_aim_mode) measure_aim_hit(dsv);  // where the hand's ray meets the scene (with or without a laser)
        if (g_aim_mode || g_pointer_mode) draw_laser(rtv[0], dsv);
        // Laser-pointer screens: the game's own 2D beam (a line to the dot) gives way to ours.
        D3D11_PRIMITIVE_TOPOLOGY topo;
        g_ctx->IAGetPrimitiveTopology(&topo);
        if (!(g_pointer_mode && topo == D3D11_PRIMITIVE_TOPOLOGY_LINELIST)) {
            g_ctx->OMSetRenderTargets(1, &g_hud_rtv, nullptr);
            draw_hud([&] { draw_hud_masked(draw); });
        }
        g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }
    if (scene && kind == 2 && g_frame_3d && !eye_image) {
        ++g_menu_quads;  // (kMenuQuads)
        if (g_hud_log > 0) log_hud_draw("menu quad");
        // An untextured 2D quad that isn't a full-screen fade is part of an overlay (a dim, a
        // prompt's box, a frame line, a subtitle bar): into the HUD with the overlay's text, in
        // the game's order, so the overlay stays whole. Fades stay in the eyes (the whole view).
        const float cover = overlay_cover();
        if (cover >= 0 && cover < kFadeCover && hud_target_like(rtv[0])) {
            g_ctx->OMSetRenderTargets(1, &g_hud_rtv, nullptr);
            draw_hud([&] { draw_hud_masked(draw); });
            g_ctx->OMSetRenderTargets(kMaxTargets, rtv, dsv);
            draw_game_view(draw, rtv, dsv, scale);
            release_targets();
            return;
        }
    }
    // In aim mode the first run of 3D draws whose models sit near the camera (kGunRunRadius) is
    // the first-person arm and gun (and the character's body behind the camera, which the grip
    // clip removes). Shaders differ: Dan's arm and revolver use two. Farther draws before the run
    // (Garcian's corridor starts with two at 17 units) don't end it; the first one after it does
    // (that is the world: Kaede's lobby has geometry 17 units out, right after her body).
    bool gun = false;
    if (g_aim_mode && scene && kind == 1 && g_gun_run < 2) {
        float t[3];
        if (model_translation(t) && t[0] * t[0] + t[1] * t[1] + t[2] * t[2] < kGunRunRadius * kGunRunRadius) {
            g_gun_run = 1;
            gun = true;
            find_gun_pivot();
            track_grip();
        } else if (g_gun_run == 1) {
            const bool trace = g_gun_run == 1 && g_aim_trace > 0;
            if (trace) {  // the first world draw: the camera it was drawn with
                ID3D11Buffer* cb0 = nullptr;
                g_ctx->VSGetConstantBuffers(0, 1, &cb0);
                if (cb0) {
                    auto it = g_cb.find(cb0);
                    cb0->Release();
                    if (it != g_cb.end() && it->second.size() >= kTemplateCbMin) {
                        const float* f = reinterpret_cast<const float*>(it->second.data());
                        logf("aimtrace frame %u: drawn %.3f", g_frame, yaw_of(-f[8], -f[10]));
                    }
                }
                --g_aim_trace;
            }
            g_gun_run = 2;
            correct_aim_cam();
            if (trace && g_anchor_set) {  // and the one the eyes and the gradients' reference now follow
                const vr::Pose w = vr::compose(g_anchor, g_drawn_cam);
                logf("aimtrace frame %u: locked %.3f", g_frame, yaw_of(-w.m[0][2], -w.m[2][2]));
            }
        }
    }
    if (gun && g_draw_cb[1]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[1]);
    const bool card = kind == 1 && is_card_shader();
    if (card && g_card_skip) {  // card_mode=skip: view-dependent cards are left out of the eyes
        if (gun && g_draw_cb[0]) g_ctx->VSSetConstantBuffers(kDrawSlot, 1, &g_draw_cb[0]);
        draw_game_view(draw, rtv, dsv, scale);
        release_targets();
        return;
    }

    if (kind == 1 && scene) scale.fill_eye();  // (the game view below keeps the game's framing)
    // 2D in the eyes during a letterboxed cutscene: clipped to the band, so the bars stay out.
    const bool clip_letterbox = kind == 2 && scene && g_letterbox_on;
    auto eye_draw = [&](int eye) {
        if (card)
            draw_scissored(eye, draw);
        else if (clip_letterbox)
            draw_in_rects(&g_letterbox, 1, draw);
        else
            draw();
    };

    // Left eye: as issued.
    eye_draw(0);

    // Right eye: twins for targets, depth and any sampled texture an eye rendered.
    draw_right(g_eye_cb[1], [&] { eye_draw(1); });
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

static void load_shader_list(const wchar_t* key, const wchar_t* fallback, std::vector<uint64_t>& out) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"vr", key, fallback, buf, 512, path_in(L"k7vr.ini").c_str());
    for (wchar_t* p = buf; *p;) {
        wchar_t* end = nullptr;
        const uint64_t h = _wcstoui64(p, &end, 16);
        if (end == p) break;
        out.push_back(h);
        p = end;
        while (*p == L',' || *p == L' ') ++p;
    }
}

static void load_card_list() {
    load_shader_list(L"cards", L"d358d9e91e34870e", g_card_ps);
    load_shader_list(L"junction_shaders", L"1da2b9dfa4382813", g_junction_ps);
    load_shader_list(L"static_shaders", L"f8dfae19c3a8200b", g_static_ps);
    const std::wstring ini = path_in(L"k7vr.ini");
    g_static_on = GetPrivateProfileIntW(L"vr", L"static_surround", 1, ini.c_str()) != 0;
    wchar_t radius[32];
    GetPrivateProfileStringW(L"vr", L"static_radius", L"", radius, 32, ini.c_str());
    if (radius[0] && _wtof(radius) > 0.5) g_static_radius_m = float(_wtof(radius));
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

static void before_draw(const char* kind, UINT count, UINT instances, INT first_vertex = -1) {
    g_draw_count = count;
    g_draw_first = first_vertex;
    if (g_cap.active) capture_draw(g_ctx, kind, count, instances, first_vertex);
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
    g_draw_base = base;
    before_draw("DrawIndexed", count, 1);
    draw_stereo([&] { real_of(c)->DrawIndexed(count, start, base); });
    after_draw();
}

static void STDMETHODCALLTYPE w_Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
    before_draw("Draw", count, 1, INT(start));
    maybe_scan_point(count, start);
    draw_stereo([&] { real_of(c)->Draw(count, start); });
    after_draw();
}

static void STDMETHODCALLTYPE w_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start,
                                                     INT base, UINT start_inst) {
    g_draw_base = -1;  // (quad_cover: not a single quad)
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
    if (SUCCEEDED(hr) && out && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE)) {
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd;
            static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
            if (bd.BindFlags & D3D11_BIND_VERTEX_BUFFER) g_vb_mapped[res] = static_cast<const uint8_t*>(out->pData);
        }
    }
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
    if (g_scene_copied && rtv && g_scene_tex) {  // the scene drawn, copied, and now cleared (g_flat_frame)
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res == g_scene_tex) g_capture_seen = true;
        if (res) res->Release();
    }
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

// A texture the CPU reads (the game's readback staging). Copies into one come from the game view
// even when the eye targets keep the game's size (sim mode without eye_res): the eye images are
// head-tracked, and the game's hit tests (the tutorial silhouette shot) read these.
static bool cpu_readable(ID3D11Resource* res) {
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    return (d.CPUAccessFlags & D3D11_CPU_ACCESS_READ) != 0;
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
    const Resized* rs = resized_info(src);
    const Resized* rd = resized_info(dst);
    if (ID3D11Texture2D* g = vr::stereo() && (rs || cpu_readable(dst)) && !rd ? gv_tex(src, false) : nullptr) {
        real_of(c)->CopySubresourceRegion(dst, dsub, x, y, z, g, ssub, box);  // the game's view, for readbacks
        return;
    }
    // The game's regions are in the sizes it believes in: scale them for resized textures.
    D3D11_BOX scaled;
    if (box && rs) {
        scaled = {UINT(box->left * rs->sx + 0.5f), UINT(box->top * rs->sy + 0.5f), box->front,
                  UINT(box->right * rs->sx + 0.5f), UINT(box->bottom * rs->sy + 0.5f), box->back};
        box = &scaled;
    }
    if (rd) x = UINT(x * rd->sx + 0.5f), y = UINT(y * rd->sy + 0.5f);
    if (rs && !rd || !rs && rd) {
        static int logged;
        if (logged++ < 5) logf("copy between a resized and an original-size texture (no game view): approximate");
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
    const Resized* rs = resized_info(src);
    const Resized* rd = resized_info(dst);
    if (ID3D11Texture2D* g = vr::stereo() && (rs || cpu_readable(dst)) && !rd ? gv_tex(src, false) : nullptr) {
        real_of(c)->CopyResource(dst, g);  // the game's view, for readbacks
        return;
    }
    if (!rs != !rd) {
        static int logged;
        if (logged++ < 8) {
            std::string s;
            describe_resource(s, "src", src);
            describe_resource(s, "dst", dst);
            logf("CopyResource between a resized and an original-size texture: skipped%s", s.c_str());
        }
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
// A texture read back as 8-bit BGRA rows, alpha 255 (alpha_only: the alpha as grey). False for
// formats it doesn't handle (logged).
static bool read_bgra(ID3D11Texture2D* tex, bool alpha_only, std::vector<uint8_t>& px, UINT* width, UINT* height) {
    bool read = false;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    *width = d.Width;
    *height = d.Height;
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
            px.assign(size_t(d.Width) * d.Height * 4, 0);
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
            read = ok;
            if (!ok) logf("readback: format %d not handled", d.Format);
        }
        staging->Release();
    }
    if (resolved) resolved->Release();
    return read;
}

static void save_texture(ID3D11Texture2D* tex, const std::wstring& path, bool alpha_only = false) {
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    if (read_bgra(tex, alpha_only, px, &w, &h)) save_png(path, w, h, w * 4, px.data());
}

// Mean brightness 0..1 (every 4th pixel each way), or -1 when it can't be read.
static float mean_level(ID3D11Texture2D* tex) {
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    if (!tex || !read_bgra(tex, false, px, &w, &h)) return -1;
    double sum = 0;
    unsigned n = 0;
    for (UINT y = 0; y < h; y += 4)
        for (UINT x = 0; x < w; x += 4, ++n) {
            const uint8_t* p = &px[(size_t(y) * w + x) * 4];
            sum += p[0] + p[1] + p[2];
        }
    return n ? float(sum / (n * 3.0 * 255.0)) : -1;
}

static bool same_size(ID3D11Texture2D* a, ID3D11Texture2D* b, std::string* why, const char* what) {
    D3D11_TEXTURE2D_DESC da, db;
    a->GetDesc(&da);
    b->GetDesc(&db);
    if (da.Width == db.Width && da.Height == db.Height) return true;
    appendf(*why, "; %s %ux%u, its source %ux%u", what, da.Width, da.Height, db.Width, db.Height);
    return false;
}

// `expect KIND MS` (control.cpp): how the next MS of frames went to the headset, as the
// headset gets them (sim mode makes the same copies). KIND 3d: eye images; menu: the game view
// on the screen panel over a 3D scene (see kMenuQuads); 2d: a 2D frame on the screen panel;
// any: the other checks only. FAIL when under 90% of the frames were KIND, when the hand-off
// kept making textures (a picture that changes shape every frame), or when, halfway through,
// what the headset gets is black, isn't its source's size, or the screen panel's copy doesn't
// match the screen image.
static SRWLOCK g_expect_lock = SRWLOCK_INIT;
static std::string g_expect_next;  // from the pad thread or this one; taken at the end of a frame
static unsigned g_expect_next_ms;
static struct {
    std::string kind;  // empty: not checking
    ULONGLONG start = 0, end = 0;
    unsigned frames = 0, n3d = 0, nmenu = 0, n2d = 0, made0 = 0;
    bool looked = false;
    std::string image;     // what the halfway look measured
    std::string problems;  // and found wrong ("; ..." each)
} g_expect;

void request_expect(const std::string& kind, unsigned ms) {
    AcquireSRWLockExclusive(&g_expect_lock);
    g_expect_next = kind;
    g_expect_next_ms = ms;
    ReleaseSRWLockExclusive(&g_expect_lock);
}

static unsigned hand_off_textures() { return vr::textures_made() + g_eye_targets_made; }

static void expect_finish() {
    auto& e = g_expect;
    const unsigned made = hand_off_textures() - e.made0;
    const unsigned of_kind = e.kind == "3d" ? e.n3d : e.kind == "menu" ? e.nmenu : e.kind == "2d" ? e.n2d : e.frames;
    std::string why;
    if (!e.frames) why += "; no frames";
    else if (of_kind * 10 < e.frames * 9) appendf(why, "; only %u of %u frames were %s", of_kind, e.frames, e.kind.c_str());
    if (made > 2) appendf(why, "; %u textures made (the hand-off remade them)", made);
    if (!e.looked) why += "; no frame looked at";
    why += e.problems;
    logf("expect %s: %s%s (%u frames: 3D %u, menu %u, 2D %u; %u textures made; %s)", e.kind.c_str(),
         why.empty() ? "pass" : "FAIL: ", why.empty() ? "" : why.c_str() + 2, e.frames, e.n3d, e.nmenu, e.n2d, made,
         e.image.c_str());
    e.kind.clear();
}

// A picture that was never drawn into reads as all zeros; the darkest real screen seen (PRESS
// START: red text on black) averages 0.003.
constexpr float kBlack = 0.0005f;

// Halfway through: what this frame handed to the headset.
static void expect_look(bool eyes, ID3D11Texture2D* left, ID3D11Texture2D* screen) {
    auto& e = g_expect;
    e.looked = true;
    e.image.clear();
    std::string& why = e.problems;
    why.clear();
    if (eyes) {
        ID3D11Texture2D* l = vr::submitted(0);
        ID3D11Texture2D* r = vr::submitted(1);
        if (!l || !r) {
            e.image = "no eye images", why = "; the eyes weren't submitted";
            return;
        }
        const float ml = mean_level(l), mr = mean_level(r);
        appendf(e.image, "eyes %.3f/%.3f", ml, mr);
        if (ml < kBlack || mr < kBlack) why += "; an eye is black";
        same_size(l, left, &why, "left eye");
        if (vr::panel_texture(1)) why += "; the screen panel shows too";
    } else {
        ID3D11Texture2D* sent = vr::submitted(2);
        ID3D11Texture2D* panel = vr::panel_texture(1);
        if (!sent || !panel) {
            e.image = "no screen image";
            why = !sent ? "; no screen image was prepared" : "; the screen panel is hidden";
            return;
        }
        const float ms = mean_level(sent), mp = mean_level(panel);
        appendf(e.image, "screen %.3f, panel %.3f", ms, mp);
        if (mp < kBlack) why += "; the screen panel is black";
        if (fabsf(mp - ms) > 0.02f) why += "; the screen panel doesn't match the screen image";
        same_size(sent, screen, &why, "screen image");
        same_size(panel, sent, &why, "screen panel");
        if (vr::submitted(0)) why += "; the eyes show too";
    }
}

// Once per frame after the hand-off. kind: 0 3D, 1 menu, 2 2D.
static void expect_frame(int kind, ID3D11Texture2D* left, ID3D11Texture2D* screen) {
    auto& e = g_expect;
    std::string next;
    unsigned next_ms = 0;
    AcquireSRWLockExclusive(&g_expect_lock);
    next.swap(g_expect_next);
    next_ms = g_expect_next_ms;
    ReleaseSRWLockExclusive(&g_expect_lock);
    const ULONGLONG now = GetTickCount64();
    if (!next.empty()) {
        if (!e.kind.empty()) expect_finish();  // cut short by the next one
        e = {};
        e.kind = next;
        e.start = now;
        e.end = now + next_ms;
        e.made0 = hand_off_textures();
    }
    if (e.kind.empty()) return;
    ++e.frames;
    (kind == 0 ? e.n3d : kind == 1 ? e.nmenu : e.n2d)++;
    if (!e.looked && now >= e.start + (e.end - e.start) / 2) expect_look(kind == 0, left, screen);
    if (now >= e.end) expect_finish();
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
    static const bool uncapped = GetEnvironmentVariableW(L"K7VR_UNCAPPED", nullptr, 0) > 0;
    if (IsIconic(d.OutputWindow) && last.QuadPart && !uncapped) {
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
    if (g_hud_log > 0) --g_hud_log;
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
        {  // how many presented frames had a 3D scene, and how many 2D ones came with no draws at all
            static unsigned n3d, nmenu, n2d, n2d_empty;
            static int most_quads;  // the most untextured 2D quads a 3D frame drew (see kMenuQuads)
            static ULONGLONG t0 = GetTickCount64();
            if (g_frame_3d && g_menu_quads > most_quads) most_quads = g_menu_quads;
            if (g_frame_3d && g_menu_quads >= kMenuQuads) ++nmenu;
            else if (g_frame_3d) ++n3d;
            else if (g_draws_last == 0) ++n2d_empty;
            else ++n2d;
            if (GetTickCount64() - t0 >= 5000) {
                logf("frames: 3D %u, menu %u, 2D %u, 2D without draws %u; most 2D quads %d; textures made %u", n3d,
                     nmenu, n2d, n2d_empty, most_quads, hand_off_textures());
                n3d = nmenu = n2d = n2d_empty = 0;
                most_quads = 0;
                t0 = GetTickCount64();
            }
        }
        ID3D11Texture2D* left = nullptr;
        ID3D11Texture2D* right = nullptr;
        ID3D11Texture2D* screen = nullptr;
        // Menu screens over the 3D scene (see kMenuQuads): the game view on the screen panel.
        ID3D11Texture2D* menu =
            g_menu_quads >= kMenuQuads && g_frame_3d && g_scene_tex ? gv_tex(g_scene_tex, false) : nullptr;
        if (bool(menu) != g_menu_shown) {
            g_menu_shown = menu != nullptr;
            logf("stereo: menu screen %s (%d 2D quads)", g_menu_shown ? "on" : "off", g_menu_quads);
        }
        if (menu) {
            screen = menu;
        } else if ((g_frame_3d || g_static_full_now) && g_final[0]) {  // (the full static: in the eyes, no HUD)
            left = g_final[0];
            right = g_final[1] ? g_final[1] : g_final[0];
        } else {
            screen = g_final[0];
        }
        ID3D11Texture2D* hud = g_frame_3d && !menu ? g_hud_tex : nullptr;
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
            if (g_scene_tex && (left || menu)) {
                swprintf(name, 64, L"pair_%03u_scene_L.png", g_pair_index);
                save_texture(g_scene_tex, path_in(name));
                if (ID3D11Texture2D* t = twin_tex(g_scene_tex, false)) {
                    swprintf(name, 64, L"pair_%03u_scene_R.png", g_pair_index);
                    save_texture(t, path_in(name));
                }
                if (ID3D11Texture2D* t = gv_tex(g_scene_tex, false)) {  // what the game's readbacks see
                    swprintf(name, 64, L"pair_%03u_game.png", g_pair_index);
                    save_texture(t, path_in(name));
                }
            }
            if (hud) {
                swprintf(name, 64, L"pair_%03u_hud.png", g_pair_index);
                save_texture(hud, path_in(name));
                swprintf(name, 64, L"pair_%03u_hud_alpha.png", g_pair_index);
                save_texture(hud, path_in(name), true);
            }
            for (int p = 0; p < 2; ++p)
                if (ID3D11Texture2D* t = vr::panel_texture(p)) {
                    swprintf(name, 64, p ? L"pair_%03u_panel_screen.png" : L"pair_%03u_panel_hud.png", g_pair_index);
                    save_texture(t, path_in(name));
                    swprintf(name, 64, p ? L"pair_%03u_panel_screen_alpha.png" : L"pair_%03u_panel_hud_alpha.png",
                             g_pair_index);
                    save_texture(t, path_in(name), true);
                }
            if (screen) {
                swprintf(name, 64, L"pair_%03u_screen.png", g_pair_index);
                save_texture(vr::submitted(2) ? vr::submitted(2) : screen, path_in(name));
            }
            // (tools\catalog.py places the panels with the eye's half-angle tangents, and crops the
            // left eye to the game view's rectangle to compare it with pair_NNN_game.png.)
            std::string view_rect;
            int r[4];
            D3D11_TEXTURE2D_DESC ld;
            if (left && vr::submitted(0) && g_gv_tan[0] > 0) {
                // Unclamped (it may reach past the eye image), with the tangents the game view
                // was drawn with this frame.
                vr::submitted(0)->GetDesc(&ld);
                if (vr::frustum_rect_unclamped(g_eyes[0].head, g_eyes[0].px, g_eyes[0].py, g_gv_tan[0], g_gv_tan[1],
                                               int(ld.Width), int(ld.Height), r))
                    appendf(view_rect, "; game view in left eye %d %d %d %d", r[0], r[1], r[2], r[3]);
                else
                    view_rect += "; game view in left eye unbounded";
            }
            // The camera behind this frame: the FOV the game meant (fov_inflate) and the vertical
            // tangent of the last perspective projection it wrote (the inflated one when inflating).
            if (g_frame_3d)
                appendf(view_rect, "; game fov %.1f, game proj tan y %.4f, gv tan y %.4f", fov_inflate_intended(),
                        g_last_proj[5] ? 1 / g_last_proj[5] : 0.f, g_gv_tan[1]);
            if (g_frame_3d && g_scene_vp_set)
                appendf(view_rect, "; game viewport %.0f %.0f %.0f %.0f band %ld %ld %ld %ld of %u %u", g_scene_vp.TopLeftX,
                        g_scene_vp.TopLeftY, g_scene_vp.Width, g_scene_vp.Height, g_scene_band.left, g_scene_band.top,
                        g_scene_band.right, g_scene_band.bottom, g_orig_w, g_orig_h);
            logf("pair %u written (%s frame%s; eye tangents x %.4f y %.4f%s)", g_pair_index++,
                 menu ? "menu" : g_frame_3d ? "3D" : "2D", g_final[1] || !left ? "" : ", right eye had no twin",
                 g_eyes[0].px[0] ? 1 / g_eyes[0].px[0] : 0.f, g_eyes[0].py[1] ? 1 / g_eyes[0].py[1] : 0.f,
                 view_rect.c_str());
        }
        expect_frame(menu ? 1 : left ? 0 : 2, left, screen);
        if (g_sweep) {  // (tools\catalog.py reads these lines and files)
            g_sweep = false;
            const unsigned n = g_sweep_index++;
            if (!left || !vr::submitted(0)) {
                logf("sweep %03u yaw %g pitch %g: not 3D", n, g_sweep_yaw, g_sweep_pitch);
            } else {
                wchar_t name[64];
                swprintf(name, 64, L"sweep_%03u_L.png", n);
                save_texture(vr::submitted(0), path_in(name));
                logf("sweep %03u yaw %g pitch %g: draws %u", n, g_sweep_yaw, g_sweep_pitch, g_draws_last);
            }
        }
        if (g_capture_seen != g_flat_frame)
            logf("stereo: flat frames %s", g_capture_seen ? "on (a 3D picture copied, then the scene cleared)" : "off");
        g_flat_frame = g_capture_seen;
        g_frame_persp = g_scene_copied = g_capture_seen = false;
        if (g_junction_now != g_junction_shown) logf("stereo: junction screen %s", g_junction_now ? "on" : "off");
        g_junction_shown = g_junction_now;
        g_junction_now = false;
        static_surround_frame_end();
        g_prev_frame_3d = g_frame_3d;
        g_frame_3d = false;
        g_scene_vp_set = false;
        g_menu_quads = 0;
        g_final[0] = g_final[1] = nullptr;
        g_scene_tex = nullptr;
        if (g_hud_rtv) {
            const float clear[4] = {0, 0, 0, 0};
            g_ctx->ClearRenderTargetView(g_hud_rtv, clear);
        }
    } else if (g_pair || g_sweep) {
        if (g_pair) logf("pair: stereo is off");
        if (g_sweep) logf("sweep: stereo is off");
        g_pair = g_sweep = false;
    }
    release_retired();  // the frame has been handed off: nothing holds the dropped twins and views now

    Requests req = control_poll();
    static bool f10_down;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !f10_down) req.capture = true;
    f10_down = f10;
    if (req.shot) g_cap.shot = true;
    if (req.pair) g_pair = true;
    if (req.sweep) {
        g_sweep = true;
        g_sweep_yaw = req.sweep_yaw;
        g_sweep_pitch = req.sweep_pitch;
    }
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

// ---- The game's swap chain ----
//
// The hooks sit in private copies of the vtables of the game's own DXGI factory and swap chain,
// never in DXGI's shared vtables. The Steam overlay hooks every new swap chain by reading the
// methods out of its vtable and detouring them, keeping one "original" per method. When the
// shared vtable already held the mod's hooks (patched through a throwaway swap chain, before the
// game made its own), the overlay took the mod's hook as its original while the mod's original
// led into the overlay: the two called each other until the stack overflowed, at the game's
// first ResizeBuffers or Present, before its window opened. With private copies the overlay
// (or anything else that hooks the same way) only ever finds DXGI's own methods.

// Gives `obj` a private copy of its vtable (as many slots as the newest interface level in
// `iids` it answers to as the same object, `counts` holding each level's length).
static void** private_vtable(IUnknown* obj, const IID* const* iids, const int* counts, int levels,
                             std::vector<void*>& copy) {
    int slots = counts[0];
    for (int i = 1; i < levels; ++i) {
        IUnknown* q = nullptr;
        if (SUCCEEDED(obj->QueryInterface(*iids[i], reinterpret_cast<void**>(&q)))) {
            if (q == obj) slots = counts[i];
            q->Release();
        }
    }
    void** real = *reinterpret_cast<void***>(obj);
    copy.assign(real, real + slots);
    *reinterpret_cast<void***>(obj) = copy.data();
    return copy.data();
}

template <typename T>
static void hook_slot(void** table, int slot, T hook_fn, T* orig) {
    *reinterpret_cast<void**>(orig) = table[slot];
    table[slot] = reinterpret_cast<void*>(hook_fn);
}

static IDXGISwapChain* g_game_sc;

static void hook_game_swapchain(IDXGISwapChain* sc) {
    static std::vector<void*> table;  // lives as long as the swap chain may
    if (sc == g_game_sc) return;
    if (g_game_sc) {
        logf("swap chain: the game made another one (%p); the hooks stay on the first", static_cast<void*>(sc));
        return;
    }
    static const IID* const iids[] = {&__uuidof(IDXGISwapChain), &__uuidof(IDXGISwapChain1),
                                      &__uuidof(IDXGISwapChain2), &__uuidof(IDXGISwapChain3),
                                      &__uuidof(IDXGISwapChain4)};
    void** t = private_vtable(sc, iids, vt::sc_slot_counts, 5, table);
    hook_slot(t, vt::sc_Present, &hk_Present, &o_Present);
    hook_slot(t, vt::sc_ResizeBuffers, &hk_ResizeBuffers, &o_ResizeBuffers);
    if (int(table.size()) > vt::sc1_Present1) hook_slot(t, vt::sc1_Present1, &hk_Present1, &o_Present1);
    g_game_sc = sc;
    logf("swap chain %p hooked (Present slot %d, Present1 slot %d, %d slots copied)", static_cast<void*>(sc),
         vt::sc_Present, vt::sc1_Present1, int(table.size()));
}

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
static CreateSwapChain_t o_CreateSwapChain;
static HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                                    IDXGISwapChain** out) {
    const HRESULT hr = o_CreateSwapChain(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) hook_game_swapchain(*out);
    return hr;
}

using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
static CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd;
static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND wnd,
                                                           const DXGI_SWAP_CHAIN_DESC1* d,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                           IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = o_CreateSwapChainForHwnd(f, dev, wnd, d, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) hook_game_swapchain(*out);
    return hr;
}

// The game makes its swap chain with the factory behind its device (it imports no DXGI
// function), so that factory object gets the hooked swap chain creation.
static void hook_swapchain(ID3D11Device* dev) {
    static std::vector<void*> table;
    IDXGIDevice* dxdev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxdev)))) return;
    if (SUCCEEDED(dxdev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        if (table.empty()) {
            static const IID* const iids[] = {&__uuidof(IDXGIFactory),  &__uuidof(IDXGIFactory1),
                                              &__uuidof(IDXGIFactory2), &__uuidof(IDXGIFactory3),
                                              &__uuidof(IDXGIFactory4), &__uuidof(IDXGIFactory5),
                                              &__uuidof(IDXGIFactory6), &__uuidof(IDXGIFactory7)};
            void** t = private_vtable(factory, iids, vt::factory_slot_counts, 8, table);
            hook_slot(t, vt::factory_CreateSwapChain, &hk_CreateSwapChain, &o_CreateSwapChain);
            if (int(table.size()) > vt::factory2_CreateSwapChainForHwnd)
                hook_slot(t, vt::factory2_CreateSwapChainForHwnd, &hk_CreateSwapChainForHwnd,
                          &o_CreateSwapChainForHwnd);
            logf("DXGI factory %p: swap chain creation hooked (%d slots copied)", static_cast<void*>(factory),
                 int(table.size()));
        }
    } else {
        logf("swap chain hook: no DXGI factory behind the device");
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
    dev->CreateBuffer(&eye, &init, &g_own_cb);
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
