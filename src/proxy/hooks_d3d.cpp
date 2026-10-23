// D3D11 probe: hooks the game's device, immediate context (through the ctx_wrap stand-in)
// and swap chain. Each frame it counts draws; on request it writes the next frame's full
// draw list (targets, viewports, shaders, the GX projection from constant buffer 0) and a PNG.
#include "common.h"

#include <dxgi1_2.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ctx_wrap.h"
#include "vtbl_index.h"

namespace k7 {

// Private data on shader objects: their DXBC hash.
static const GUID kShaderHashGuid = {0x6c1d3e0a, 0x4b7f, 0x4c55, {0x9a, 0x10, 0x2e, 0x77, 0x31, 0x5d, 0x0b, 0x01}};

// GX vertex template's cbuffer: projMatrix follows 64 + 32 + 64 float4 registers.
constexpr UINT kProjOffset = (64 + 32 + 64) * 16;
constexpr UINT kTemplateCbMin = kProjOffset + 64;

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;      // the real immediate context
static ID3D11DeviceContext* g_wrapped;  // what the game holds

// ---- Constant buffer shadows (contents as last written by the CPU) ----

static std::unordered_map<ID3D11Resource*, std::vector<uint8_t>> g_cb;
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
    s += '\n';
}

// ---- Frame image: the game's own frame, as sampled by its final blit to the window ----
// (The backbuffer is useless while the window is minimized: DXGI shrinks it to 8x8.)

static ID3D11Texture2D* g_backbuffer;   // raw pointer for comparison only, refreshed each frame
static bool g_want_image;                // this frame's image is wanted (shot or capture)
static ID3D11Texture2D* g_image;         // our copy of the final blit's source
static bool g_image_valid;

// ---- Desktop stereo test ----
// The patched vertex template reads an eye offset from slot b12. With a separation set, every
// draw into the scene target (a render target with a same-sized depth buffer) is drawn twice:
// left eye into the game's targets, right eye into mirror targets we own. Clears are mirrored
// too. Post passes and EFB copies only see the left eye; that's fine for a probe.

constexpr UINT kStereoSlot = 12;
static ID3D11Buffer* g_eye_cb[2];  // left, right
static float g_sep;                // eye separation in GX eye-space units; 0 = mono
static bool g_pair;                // save both eyes at the end of this frame
static unsigned g_pair_index;

struct MirrorView {
    ID3D11Texture2D* tex = nullptr;
    ID3D11View* view = nullptr;  // ID3D11RenderTargetView or ID3D11DepthStencilView
};
static std::unordered_map<ID3D11View*, MirrorView> g_mirrors;
static ID3D11RenderTargetView* g_scene_rtv;  // last mirrored scene target (for `pair`)

static float g_yaw, g_pitch;  // free look from the game camera, degrees (+yaw right, +pitch up)

// Eye-space transform per eye: undo the head rotation, then the eye offset.
static void set_eye_offsets() {
    const float d2r = 3.14159265f / 180;
    const float cy = cosf(g_yaw * d2r), sy = sinf(g_yaw * d2r);
    const float cp = cosf(g_pitch * d2r), sp = sinf(g_pitch * d2r);
    for (int eye = 0; eye < 2; ++eye) {
        if (!g_eye_cb[eye]) return;
        const float offset = g_sep <= 0 ? 0 : (eye == 0 ? g_sep / 2 : -g_sep / 2);  // scene moves opposite to the eye
        const float m[12] = {cy,       0,   sy,      offset,  //
                             -sp * sy, cp,  sp * cy, 0,       //
                             -cp * sy, -sp, cp * cy, 0};
        g_ctx->UpdateSubresource(g_eye_cb[eye], 0, nullptr, m, 0, 0);
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

static MirrorView* mirror_of(ID3D11View* view, bool depth) {
    auto it = g_mirrors.find(view);
    if (it != g_mirrors.end()) return &it->second;
    D3D11_TEXTURE2D_DESC d;
    if (!texture_of(view, &d)) return nullptr;
    MirrorView m;
    d.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &m.tex))) return nullptr;
    HRESULT hr;
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd;
        static_cast<ID3D11DepthStencilView*>(view)->GetDesc(&vd);
        hr = g_dev->CreateDepthStencilView(m.tex, &vd, reinterpret_cast<ID3D11DepthStencilView**>(&m.view));
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        static_cast<ID3D11RenderTargetView*>(view)->GetDesc(&vd);
        hr = g_dev->CreateRenderTargetView(m.tex, &vd, reinterpret_cast<ID3D11RenderTargetView**>(&m.view));
    }
    if (FAILED(hr)) {
        m.tex->Release();
        return nullptr;
    }
    logf("stereo: mirror %s %ux%u f%d", depth ? "depth" : "target", d.Width, d.Height, d.Format);
    return &g_mirrors.emplace(view, m).first->second;
}

// Returns true and fills the mirror views when the current targets are a scene target pair.
static bool scene_mirrors(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv, MirrorView** mrt,
                          MirrorView** mds) {
    g_ctx->OMGetRenderTargets(1, rtv, dsv);
    bool ok = false;
    if (*rtv && *dsv) {
        D3D11_TEXTURE2D_DESC rd, dd;
        if (texture_of(*rtv, &rd) && texture_of(*dsv, &dd) && rd.Width == dd.Width && rd.Height == dd.Height) {
            *mrt = mirror_of(*rtv, false);
            *mds = mirror_of(*dsv, true);
            ok = *mrt && *mds;
        }
    }
    return ok;
}

// Runs `draw` for the left eye, then again for the right eye into the mirror targets.
template <typename DrawFn>
static void draw_stereo(DrawFn draw) {
    draw();
    if (g_sep <= 0) return;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    MirrorView* mrt = nullptr;
    MirrorView* mds = nullptr;
    if (scene_mirrors(&rtv, &dsv, &mrt, &mds)) {
        auto mirror_rtv = static_cast<ID3D11RenderTargetView*>(mrt->view);
        g_ctx->OMSetRenderTargets(1, &mirror_rtv, static_cast<ID3D11DepthStencilView*>(mds->view));
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[1]);
        draw();
        g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);
        g_ctx->OMSetRenderTargets(1, &rtv, dsv);
        g_scene_rtv = rtv;
    }
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
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

// ---- Context hooks (installed in the stand-in's table; `c` is the stand-in) ----

static void STDMETHODCALLTYPE w_DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base) {
    before_draw("DrawIndexed", count, 1);
    draw_stereo([&] { real_of(c)->DrawIndexed(count, start, base); });
}

static void STDMETHODCALLTYPE w_Draw(ID3D11DeviceContext* c, UINT count, UINT start) {
    before_draw("Draw", count, 1);
    draw_stereo([&] { real_of(c)->Draw(count, start); });
}

static void STDMETHODCALLTYPE w_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start,
                                                     INT base, UINT start_inst) {
    before_draw("DrawIndexedInstanced", count, inst);
    draw_stereo([&] { real_of(c)->DrawIndexedInstanced(count, inst, start, base, start_inst); });
}

static void STDMETHODCALLTYPE w_DrawInstanced(ID3D11DeviceContext* c, UINT count, UINT inst, UINT start,
                                              UINT start_inst) {
    before_draw("DrawInstanced", count, inst);
    draw_stereo([&] { real_of(c)->DrawInstanced(count, inst, start, start_inst); });
}

static HRESULT STDMETHODCALLTYPE w_Map(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type,
                                       UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    HRESULT hr = real_of(c)->Map(res, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && type != D3D11_MAP_READ && constant_buffer_size(res)) g_cb_mapped[res] = out->pData;
    return hr;
}

static void STDMETHODCALLTYPE w_Unmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    auto it = g_cb_mapped.find(res);
    if (it != g_cb_mapped.end()) {
        UINT size = constant_buffer_size(res);
        auto src = static_cast<const uint8_t*>(it->second);
        g_cb[res].assign(src, src + size);
        g_cb_mapped.erase(it);
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
    if (auto it = g_mirrors.find(rtv); it != g_mirrors.end())
        g_ctx->ClearRenderTargetView(static_cast<ID3D11RenderTargetView*>(it->second.view), color);
}

static void STDMETHODCALLTYPE w_ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT d,
                                         UINT8 st) {
    if (g_cap.active) {
        appendf(g_cap.text, "     ClearDSV");
        describe_view(g_cap.text, "ds", dsv);
        appendf(g_cap.text, " flags=%u depth=%g\n", flags, d);
    }
    real_of(c)->ClearDepthStencilView(dsv, flags, d, st);
    if (auto it = g_mirrors.find(dsv); it != g_mirrors.end())
        g_ctx->ClearDepthStencilView(static_cast<ID3D11DepthStencilView*>(it->second.view), flags, d, st);
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
    real_of(c)->CopySubresourceRegion(dst, dsub, x, y, z, src, ssub, box);
}

static void STDMETHODCALLTYPE w_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_cap.active) {
        appendf(g_cap.text, "     CopyResource");
        describe_resource(g_cap.text, "src", src);
        describe_resource(g_cap.text, "dst", dst);
        g_cap.text += '\n';
    }
    real_of(c)->CopyResource(dst, src);
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

static void tag_shader(ID3D11DeviceChild* shader, const void* bc, SIZE_T len) {
    uint64_t h = dxbc_hash(bc, len);
    shader->SetPrivateData(kShaderHashGuid, sizeof(h), &h);
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

static void save_texture(ID3D11Texture2D* tex, const std::wstring& path) {
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
    if (g_pair) {
        g_pair = false;
        auto it = g_scene_rtv ? g_mirrors.find(g_scene_rtv) : g_mirrors.end();
        if (g_sep > 0 && it != g_mirrors.end()) {
            D3D11_TEXTURE2D_DESC d;
            wchar_t name[64];
            swprintf(name, 64, L"pair_%03u_L.png", g_pair_index);
            save_texture(texture_of(g_scene_rtv, &d), path_in(name));
            swprintf(name, 64, L"pair_%03u_R.png", g_pair_index);
            save_texture(it->second.tex, path_in(name));
            logf("pair %u written (separation %g)", g_pair_index++, g_sep);
        } else {
            logf("pair: stereo is off or no scene target was mirrored this frame");
        }
    }

    Requests req = control_poll();
    static bool f10_down;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !f10_down) req.capture = true;
    f10_down = f10;
    if (req.shot) g_cap.shot = true;
    if (req.pair) g_pair = true;
    if (req.view) {
        g_yaw = req.yaw;
        g_pitch = req.pitch;
        logf("view yaw %g pitch %g", g_yaw, g_pitch);
    }
    if (req.stereo >= 0) {
        g_sep = req.stereo;
        logf("stereo separation %g", g_sep);
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
    limit_background_rate(sc);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) end_frame(sc);
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
    hook(dev, vt::dev_CreateDeferredContext, &hk_CreateDeferredContext, &o_CreateDeferredContext);
    hook(dev, vt::dev_GetImmediateContext, &hk_GetImmediateContext, &o_GetImmediateContext);

    D3D11_BUFFER_DESC eye{48, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    const float zero[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    D3D11_SUBRESOURCE_DATA init{zero, 0, 0};
    dev->CreateBuffer(&eye, &init, &g_eye_cb[0]);
    dev->CreateBuffer(&eye, &init, &g_eye_cb[1]);
    g_ctx->VSSetConstantBuffers(kStereoSlot, 1, &g_eye_cb[0]);

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

void install_d3d_hooks() {
    patch_import(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&hk_CreateDevice),
                 reinterpret_cast<void**>(&o_CreateDevice));
}

}  // namespace k7
