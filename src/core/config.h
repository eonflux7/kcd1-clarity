#pragma once
#include <windows.h>

#include <string>

namespace kc {

// kcd_clarity.ini next to the DLL. Missing keys get defaults; the file is written back on load
// so every setting is visible to the user.
struct Config {
    int mode = 0;              // [general] mode: 0 = engine TAA, 1 = DLSS
    UINT toggle_key = VK_F8;   // [ui] toggle_key, virtual-key code: settings menu
    UINT debug_key = VK_F3;    // [ui] debug_key: debug window
    bool show_on_start = false;
    bool show_fps = false;     // small FPS readout while the overlay is closed
    int dlss_quality = 0;      // [dlss] quality: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int dlss_preset = 0;       // [dlss] preset: NGX render preset, 0 = DLSS default, 10-13 = J-M
    bool mip_bias = true;      // [dlss] mip_bias: texture LOD bias log2(render scale) + mip_bias_offset
    float mip_bias_offset = 0; // [dlss] mip_bias_offset
    bool velocity16 = true;    // [dlss] velocity16: object motion vectors at 16 bits (restart to change)
    bool coarse_shadow_noise = true;  // [dlss] coarse_shadow_noise: lower shadow noise frequency below scale 0.5
    bool temporal_shadow_noise = true;  // [dlss] temporal_shadow_noise: shadow filter noise rotated every frame
    bool edge_clamp = true;    // [dlss] edge_clamp: replicate the sub-rect's edge outside it (no light leaks)
    bool ui_depth_alignment = false; // [dlss] align depth before native-resolution inventory menu composition
    bool d3d_debug = false;    // [debug] d3d_debug: D3D11 debug layer, messages to the mod log (slow)
    // [debug] black-screen bisection, also in the F3 window
    bool skip_ngx = false;      // skip_ngx: run the DLSS path without NGX (works under RenderDoc)
    bool engine_after = false;  // engine_after: let the engine TAA draw run after ours
    bool force_halton = true;   // force_halton: r_AntialiasingTAAPattern 8 while DLSS is on
    bool state_swap = false;    // state_swap: SwapDeviceContextState instead of a manual state backup
};

// Render scale for a [dlss] quality value.
float render_scale_for(int quality);

Config& config();
void config_load(const std::wstring& path);
void config_save();

}  // namespace kc
