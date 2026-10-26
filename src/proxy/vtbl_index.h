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

}  // namespace k7::vt
