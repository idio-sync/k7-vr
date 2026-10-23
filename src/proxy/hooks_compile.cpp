// D3DCompile hook: the port builds its shaders at runtime from Shaders\*Template.hlsl.
// Every source is dumped to k7vr\shaders\<target>_<dxbc hash>.hlsl, the name the
// frame captures use. A file at k7vr\override\<same name> is compiled instead.
#include "common.h"

#include <d3dcommon.h>

#include <cstdio>
#include <mutex>
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
    HRESULT hr = o_D3DCompile(src, len, name, defines, include, entry, target, flags1, flags2, code, errors);
    if (FAILED(hr) || !code || !*code) {
        logf("D3DCompile %s failed 0x%08x", target ? target : "?", hr);
        return hr;
    }
    uint64_t h = dxbc_hash((*code)->GetBufferPointer(), (*code)->GetBufferSize());
    wchar_t file[96];
    swprintf(file, 96, L"%hs_%016llx.hlsl", target ? target : "unknown", h);

    std::lock_guard lock(g_mx);
    ++g_compiles;
    std::wstring dump = g_dir + L"\\shaders\\" + file;
    if (GetFileAttributesW(dump.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW((g_dir + L"\\shaders").c_str(), nullptr);
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

void install_compile_hook() {
    patch_import(GetModuleHandleW(nullptr), "D3DCOMPILER_43.dll", "D3DCompile", reinterpret_cast<void*>(&hk_D3DCompile),
                 reinterpret_cast<void**>(&o_D3DCompile));
}

}  // namespace k7
