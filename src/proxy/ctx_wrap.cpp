#include "ctx_wrap.h"

#include <d3d11_4.h>

#include "vtbl_index.h"

namespace k7 {

static WrappedContext g_wrapped;
static void** g_table;

// x86 __stdcall thunk for slot i: [esp+4] holds `this` (the stand-in).
//   mov eax, [esp+4]      8B 44 24 04
//   mov eax, [eax+4]      8B 40 04       ; WrappedContext::real
//   mov [esp+4], eax      89 44 24 04
//   mov eax, [eax]        8B 00          ; real context's current table
//   jmp [eax + i*4]       FF A0 imm32
constexpr size_t kThunkSize = 24;

static HRESULT STDMETHODCALLTYPE w_QueryInterface(ID3D11DeviceContext* self, REFIID iid, void** out) {
    ID3D11DeviceContext* real = real_of(self);
    HRESULT hr = real->QueryInterface(iid, out);
    if (SUCCEEDED(hr) && out && *out) {
        bool context_iid = iid == __uuidof(IUnknown) || iid == __uuidof(ID3D11DeviceChild) ||
                           iid == __uuidof(ID3D11DeviceContext) || iid == __uuidof(ID3D11DeviceContext1) ||
                           iid == __uuidof(ID3D11DeviceContext2) || iid == __uuidof(ID3D11DeviceContext3) ||
                           iid == __uuidof(ID3D11DeviceContext4);
        if (context_iid) {
            if (*out == real)
                *out = self;  // keep the game on the stand-in; the real object holds the reference
            else
                logf("context QueryInterface returned a different pointer (%p) - calls through it bypass the probe",
                     *out);
        }
    }
    return hr;
}

ID3D11DeviceContext* wrap_context(ID3D11DeviceContext* real) {
    if (!g_table) {
        const int n = vt::ctx4_slot_count;
        auto code = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, n * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        g_table = new void*[n];
        for (int i = 0; i < n; ++i) {
            uint8_t* t = code + i * kThunkSize;
            const uint8_t head[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x40, 0x04, 0x89, 0x44, 0x24, 0x04, 0x8B, 0x00, 0xFF, 0xA0};
            memcpy(t, head, sizeof(head));
            int32_t offset = i * 4;
            memcpy(t + sizeof(head), &offset, 4);
            g_table[i] = t;
        }
        FlushInstructionCache(GetCurrentProcess(), code, n * kThunkSize);
        g_table[0] = reinterpret_cast<void*>(&w_QueryInterface);
        logf("context wrapper: %d slots", n);
    }
    g_wrapped.vtbl = g_table;
    g_wrapped.real = real;
    return reinterpret_cast<ID3D11DeviceContext*>(&g_wrapped);
}

void wrap_set(int slot, void* hook) { g_table[slot] = hook; }

}  // namespace k7
