#pragma once
// A stand-in for the game's immediate context. d3d11 gives each context its own
// function table and swaps draw entries in it as state changes, so patching that
// table doesn't stick. Instead the game holds this object, whose every vtable slot
// is a thunk that substitutes the real context and jumps through the real
// context's *current* table. wrap_set() replaces a slot with a C++ hook.
#include "common.h"

namespace k7 {

struct WrappedContext {
    void** vtbl;
    ID3D11DeviceContext* real;
};

ID3D11DeviceContext* wrap_context(ID3D11DeviceContext* real);
void wrap_set(int slot, void* hook);

inline ID3D11DeviceContext* real_of(ID3D11DeviceContext* wrapped) {
    return reinterpret_cast<WrappedContext*>(wrapped)->real;
}

}  // namespace k7
