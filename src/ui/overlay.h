#pragma once
#include <dxgi.h>
#include <d3d11.h>

namespace kc::overlay {
// Called from the Present hook on the render thread (before the real Present).
void on_present(IDXGISwapChain* swapchain);
// OptiScaler's hidden DX11 presenter copies into a shared texture before Present.
// Draw the panel on that back buffer before the copy, using the visible game window for input.
void before_copy(ID3D11DeviceContext* ctx, ID3D11Resource* destination, ID3D11Resource* source);
// Called before the real ResizeBuffers: drops our reference to the back buffer.
void on_resize_begin();
}
