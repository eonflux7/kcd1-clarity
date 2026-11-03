#pragma once
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <cstdint>

// Only declared SRV slots can consume padding. Unknown bytecode stays conservative.
namespace kc::resource_slots {
inline constexpr uint32_t all = ~uint32_t(0);
inline uint32_t reflect(const void* code, SIZE_T size) {
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return all;
    D3D11_SHADER_DESC d{};
    uint32_t mask = 0;
    if (FAILED(r->GetDesc(&d))) {
        r->Release();
        return all;
    }
    for (UINT i = 0; i < d.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        if (FAILED(r->GetResourceBindingDesc(i, &b))) {
            mask = all;
            break;
        }
        if (b.Type != D3D_SIT_TEXTURE && b.Type != D3D_SIT_TBUFFER && b.Type != D3D_SIT_STRUCTURED &&
            b.Type != D3D_SIT_BYTEADDRESS)
            continue;
        if (!b.BindCount || b.BindCount == UINT(-1)) {
            mask = all;
            break;
        }
        for (UINT slot = b.BindPoint; slot < 32 && slot - b.BindPoint < b.BindCount; ++slot)
            mask |= uint32_t(1) << slot;
    }
    r->Release();
    return mask;
}
inline UINT count(uint32_t mask) {
    UINT n = 0;
    while (mask) {
        ++n;
        mask >>= 1;
    }
    return n;
}
}  // namespace kc::resource_slots
