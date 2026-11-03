#pragma once
#include <d3d11.h>

namespace kc::subrect_fixes {
enum class Kind { None, Stencil, Contact, Bloom, Caustics, WaterReflection, HairOpaqueContact, HairContact };
struct Shader {
    Kind kind = Kind::None;
    ID3D11PixelShader* replacement = nullptr;
};
// Creation callbacks must run inside d3d11::ScopedInside; cache owns shader references.
void on_create_ps(ID3D11Device* device, const void* code, SIZE_T size, ID3D11PixelShader* shader);
void on_create_vs(const void* code, SIZE_T size, ID3D11VertexShader* shader);
Shader find_ps(ID3D11PixelShader* shader);
bool is_bloom_vs(ID3D11VertexShader* shader);
struct DrawState {
    ID3D11PixelShader* original = nullptr;
    ID3D11Buffer* params = nullptr;
    bool contact = false;
    bool caustics = false;
};
// Render thread, inside ScopedInside. The caller restores this state after every draw path.
DrawState before_draw(ID3D11DeviceContext* ctx, Shader shader, ID3D11PixelShader* original);
void after_draw(ID3D11DeviceContext* ctx, DrawState& state);
} // namespace kc::subrect_fixes
