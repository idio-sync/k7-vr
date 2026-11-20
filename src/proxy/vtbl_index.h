#pragma once
// Vtable slot numbers, computed from the C interface structs in vtbl_index.cpp.

namespace k7::vt {

extern const int dev_CreateTexture2D;
extern const int tex2d_GetDesc;
extern const int dev_CreateVertexShader;
extern const int dev_CreateGeometryShader;
extern const int dev_CreatePixelShader;
extern const int dev_CreateDeferredContext;
extern const int ctx_ExecuteCommandList;
extern const int dev_GetImmediateContext;
extern const int ctx4_slot_count;

extern const int ctx_DrawIndexed;
extern const int ctx_Draw;
extern const int ctx_Map;
extern const int ctx_Unmap;
extern const int ctx_DrawIndexedInstanced;
extern const int ctx_DrawInstanced;
extern const int ctx_ClearRenderTargetView;
extern const int ctx_ClearDepthStencilView;
extern const int ctx_CopySubresourceRegion;
extern const int ctx_CopyResource;
extern const int ctx_UpdateSubresource;

extern const int sc_Present;
extern const int sc_ResizeBuffers;
extern const int sc1_Present1;
extern const int factory_CreateSwapChain;
extern const int factory2_CreateSwapChainForHwnd;

// Vtable lengths of each interface level, oldest first (IDXGISwapChain..4, IDXGIFactory..7):
// a private copy of an object's vtable copies as many slots as the newest level it answers to.
extern const int sc_slot_counts[5];
extern const int factory_slot_counts[8];

}  // namespace k7::vt
