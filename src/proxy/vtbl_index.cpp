// The C views of the D3D11/DXGI interfaces spell out each vtable as a struct,
// so slot numbers come from offsetof instead of hand-counted constants.
#define CINTERFACE
#define D3D11_NO_HELPERS  // the CD3D11_* helpers need the C++ interfaces
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <cstddef>

#include "vtbl_index.h"

#define SLOT(iface, method) int(offsetof(iface##Vtbl, method) / sizeof(void*))

namespace k7::vt {

const int dev_CreateVertexShader = SLOT(ID3D11Device, CreateVertexShader);
const int dev_CreateGeometryShader = SLOT(ID3D11Device, CreateGeometryShader);
const int dev_CreatePixelShader = SLOT(ID3D11Device, CreatePixelShader);
const int dev_CreateDeferredContext = SLOT(ID3D11Device, CreateDeferredContext);
const int ctx_ExecuteCommandList = SLOT(ID3D11DeviceContext, ExecuteCommandList);
const int dev_GetImmediateContext = SLOT(ID3D11Device, GetImmediateContext);
const int ctx4_slot_count = int(sizeof(ID3D11DeviceContext4Vtbl) / sizeof(void*));

const int ctx_DrawIndexed = SLOT(ID3D11DeviceContext, DrawIndexed);
const int ctx_Draw = SLOT(ID3D11DeviceContext, Draw);
const int ctx_Map = SLOT(ID3D11DeviceContext, Map);
const int ctx_Unmap = SLOT(ID3D11DeviceContext, Unmap);
const int ctx_DrawIndexedInstanced = SLOT(ID3D11DeviceContext, DrawIndexedInstanced);
const int ctx_DrawInstanced = SLOT(ID3D11DeviceContext, DrawInstanced);
const int ctx_ClearRenderTargetView = SLOT(ID3D11DeviceContext, ClearRenderTargetView);
const int ctx_ClearDepthStencilView = SLOT(ID3D11DeviceContext, ClearDepthStencilView);
const int ctx_CopySubresourceRegion = SLOT(ID3D11DeviceContext, CopySubresourceRegion);
const int ctx_CopyResource = SLOT(ID3D11DeviceContext, CopyResource);
const int ctx_UpdateSubresource = SLOT(ID3D11DeviceContext, UpdateSubresource);

const int sc_Present = SLOT(IDXGISwapChain, Present);
const int sc1_Present1 = SLOT(IDXGISwapChain1, Present1);

}  // namespace k7::vt
