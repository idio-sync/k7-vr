// D3DCompile hook: the port builds its shaders at runtime from Shaders\*Template.hlsl.
// Every source is dumped to k7vr\shaders\<target>_<dxbc hash>.hlsl, the name the
// frame captures use. A file at k7vr\override\<same name> is compiled instead.
#include "common.h"

#include <d3dcommon.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace k7 {

using D3DCompile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
static D3DCompile_t o_D3DCompile;
static std::mutex g_mx;
static unsigned g_compiles;

uint64_t dxbc_hash(const void* bytecode, size_t size) {
    uint64_t h = 0;
    if (size >= 12 && memcmp(bytecode, "DXBC", 4) == 0) memcpy(&h, static_cast<const uint8_t*>(bytecode) + 4, 8);
    return h;
}

// Texture coordinates generated from eye-space positions (a position matrix, GX_PNMTX0-9 =
// posTexMatMem[0..29]) are camera-relative: the game colours walls and floors through screen
// projections of them (post-transform matrices that map the game camera's view to 0..1, sampling
// vertical gradient textures). In aim mode the hand turns the game camera, so with k7_flags.y set
// those positions are first moved into a reference camera's eye space (k7_ref, the camera when
// aiming began) and the gradients stay where they were. The first-person arm and gun (k7_draw.x)
// keep the game's own.
static void patch_texgen(std::string& src) {
    const char* kSource = "inTexCoord = float4(_in.inPosition, 1);";
    const char* kZ = "outTexCoord.z = dot(inTexCoord, posTexMatMem[";
    for (size_t at = src.find(kSource); at != std::string::npos; at = src.find(kSource, at + 1)) {
        const size_t z = src.find(kZ, at);
        const size_t next = src.find("_out.outTexCoord", at);
        if (z == std::string::npos || (next != std::string::npos && z > next)) continue;  // 2x4 matrix: no z
        const int index = atoi(src.c_str() + z + strlen(kZ));
        if (index >= 30) continue;  // a texture matrix or the identity: object space
        const size_t end = src.find(';', z);
        if (end == std::string::npos) continue;
        src.insert(end + 1,
                   "\n\tif (k7_flags.y != 0 && k7_draw.x == 0) outTexCoord.xyz = float3(dot(k7_ref[0], "
                   "float4(outTexCoord.xyz, 1)), dot(k7_ref[1], float4(outTexCoord.xyz, 1)), dot(k7_ref[2], "
                   "float4(outTexCoord.xyz, 1)));  // k7vr texgen");
    }
}

// Stereo patch for the GX vertex template: a per-eye head transform (3x4, eye-space rotation
// plus eye offset; slot b12, set by hooks_d3d.cpp), applied to perspective draws only
// (projMatrix row 3 = 0,0,-1,0; orthographic HUD draws have 0,0,0,1 and stay put).
// With k7_flags.x set, perspective draws use k7_px/k7_py as their clip x/y rows (the headset's
// projection, or the intended FOV while the game culls with an inflated one); the game's z and
// w rows stay, so its depth conventions are untouched. Layout matches vr::EyeConsts.
// Returns false if the source isn't the template.
static bool patch_stereo(std::string& src) {
    const char* kConsts = "cbuffer VertexShaderConsts";
    const char* kProject = "_out.outPosition.x = dot(outPos, projMatrix[0]);";
    const char* kProjectW = "_out.outPosition.w = dot(outPos, projMatrix[3]);";
    size_t w = src.find(kProjectW);
    if (w == std::string::npos) return false;
    src.insert(w + strlen(kProjectW),
               "\n\tif (projMatrix[3].w == 0 && k7_flags.x != 0) { _out.outPosition.x = dot(outPos, k7_px); "
               "_out.outPosition.y = dot(outPos, k7_py); }  // k7vr stereo");
    size_t consts = src.find(kConsts);
    size_t project = src.find(kProject);
    const size_t out_struct = src.find("struct VS_OUTPUT");
    const size_t out_end = out_struct == std::string::npos ? out_struct : src.find("};", out_struct);
    if (consts == std::string::npos || project == std::string::npos || out_end == std::string::npos || out_end > consts)
        return false;
    // Per-draw transform first (slot b13: the first-person arm and gun), then the eye's head transform.
    // The arm/gun draw is also clipped at a plane behind the grip (k7_draw.y: eye-space z in the
    // game's aim view), so the forearm doesn't swing into view when the hand turns.
    src.insert(project,
               "_out.k7clip = (projMatrix[3].w == 0 && k7_draw.x != 0) ? k7_draw.y - outPos.z : 1;  // k7vr per-draw\n\t"
               "if (projMatrix[3].w == 0 && k7_draw.x != 0) outPos.xyz = float3(dot(k7_obj[0], outPos), "
               "dot(k7_obj[1], outPos), dot(k7_obj[2], outPos));  // k7vr per-draw\n\t"
               "if (projMatrix[3].w == 0) outPos.xyz = float3(dot(k7_head[0], outPos), dot(k7_head[1], outPos), "
               "dot(k7_head[2], outPos));  // k7vr stereo\n\t");
    patch_texgen(src);
    src.insert(consts,
               "cbuffer K7Stereo : register(b12) { float4 k7_head[3]; float4 k7_px; float4 k7_py; float4 k7_flags; "
               "float4 k7_ref[3]; };  // k7vr stereo\n"
               "cbuffer K7Draw : register(b13) { float4 k7_obj[3]; float4 k7_draw; };  // k7vr per-draw\n\n");
    src.insert(out_end, "\tfloat k7clip : SV_ClipDistance0;  // k7vr per-draw\n");
    return true;
}

static bool read_file(const std::wstring& path, std::vector<char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

static HRESULT WINAPI hk_D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO* defines,
                                    ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                    ID3DBlob** code, ID3DBlob** errors) {
    static const bool stereo = GetPrivateProfileIntW(L"probe", L"stereo_patch", 1, path_in(L"k7vr.ini").c_str()) != 0;
    std::string patched;
    if (stereo && target && strncmp(target, "vs_", 3) == 0) {
        patched.assign(static_cast<const char*>(src), len);
        if (patch_stereo(patched)) {
            src = patched.data();
            len = patched.size();
        }
    }
    HRESULT hr = o_D3DCompile(src, len, name, defines, include, entry, target, flags1, flags2, code, errors);
    if (FAILED(hr) || !code || !*code) {
        logf("D3DCompile %s failed 0x%08x: %s", target ? target : "?", hr,
             errors && *errors ? static_cast<const char*>((*errors)->GetBufferPointer()) : "");
        return hr;
    }
    uint64_t h = dxbc_hash((*code)->GetBufferPointer(), (*code)->GetBufferSize());
    wchar_t file[96];
    swprintf(file, 96, L"%hs_%016llx.hlsl", target ? target : "unknown", h);

    std::lock_guard lock(g_mx);
    ++g_compiles;
    // Dumped only when k7vr\shaders exists (the test copy; a normal install doesn't have it).
    std::wstring dump = g_dir + L"\\shaders\\" + file;
    if (GetFileAttributesW(dump.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(dump.c_str(), L"wb")) {
            fwrite(src, 1, len, f);
            fclose(f);
        }
    }

    std::vector<char> over;
    if (read_file(g_dir + L"\\override\\" + file, over)) {
        ID3DBlob* new_code = nullptr;
        ID3DBlob* new_errors = nullptr;
        HRESULT hr2 = o_D3DCompile(over.data(), over.size(), name, defines, include, entry, target, flags1, flags2,
                                   &new_code, &new_errors);
        if (SUCCEEDED(hr2) && new_code) {
            (*code)->Release();
            *code = new_code;
            logf("override %ls compiled", file);
        } else {
            logf("override %ls FAILED: %s", file,
                 new_errors ? static_cast<const char*>(new_errors->GetBufferPointer()) : "?");
        }
        if (new_errors) new_errors->Release();
    }
    return hr;
}

ID3DBlob* compile_hlsl(const char* src, const char* target, const char* name) {
    D3DCompile_t compile = o_D3DCompile;
    if (!compile) {  // the hook isn't installed (or not yet): the compiler directly
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        compile = dll ? reinterpret_cast<D3DCompile_t>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    }
    if (!compile) {
        logf("%s: no D3DCompile", name);
        return nullptr;
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        logf("%s: compile failed: %s", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (code) code->Release();
        code = nullptr;
    }
    if (errors) errors->Release();
    return code;
}

void install_compile_hook() {
    patch_import(GetModuleHandleW(nullptr), "D3DCOMPILER_43.dll", "D3DCompile", reinterpret_cast<void*>(&hk_D3DCompile),
                 reinterpret_cast<void**>(&o_D3DCompile));
}

}  // namespace k7
