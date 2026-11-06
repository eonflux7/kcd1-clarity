#pragma once
#include <cstdint>
#include <d3d11.h>
#include <wrl/client.h>
namespace kc::ui_depth {
void on_create_ps(const void *code, SIZE_T size, ID3D11PixelShader *shader);
void after_taa(UINT rw, UINT rh, UINT w, UINT h);
// Each draw owns its binding override. Pair under the hook's reentry guard,
// including early returns: the engine's depth resource is never modified.
struct DrawState {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> colour;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
};
void before_draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *shader, DrawState &state);
void after_draw(ID3D11DeviceContext *ctx, const DrawState &state);
bool enabled();
void set_enabled(bool enabled);
uint64_t eligible_frames();
uint64_t corrected_frames();
} // namespace kc::ui_depth
