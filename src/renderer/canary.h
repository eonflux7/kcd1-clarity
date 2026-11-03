#pragma once
#include <d3d11.h>

#include <cstdint>

// Debug aid for the downscaled sub-rect path (kcd-re db/notes/20-resolution.md): finds passes that read
// outside the sub-rect, the class behind the SSS feedback loop, the bloom haze, the SSDO/SVO right-edge glow.
//
// Full-layout bloom is tracked explicitly and excluded from sub-rect fills.
// Water reflection rectangles are tracked from their producer viewport.
// The invariant for other screen buffers is "every screen buffer holds the current frame in its top-left
// (scale x scale) part"; what lies outside is stale and no correct pass reads it. While the frame is
// downscaled, before every draw and dispatch up to the TAA draw (inclusive), the outside region of each bound
// VS/PS/CS input that is a screen-proportional texture is filled with a marker value, once per texture per
// write (binding it as a render target or UAV makes it dirty again). A pass that reads it carries the marker
// into the image. Fills (colour / 1-2 channel / depth-stencil):
//   Mixed:   magenta (64 for float, 1 for unorm) / 0 / near plane
//   Inverse: black / 64 / far plane
//   NaN:     quiet NaN everywhere (depth: near plane)
// Blink alternates Mixed and Inverse every N frames: the sub-rect must not change, so whatever flickers is a
// bug. Colour targets are filled with ClearView rects, depth-stencil ones with a depth-only draw that
// discards inside. Not covered: copies into a poisoned texture (CopyResource) are not tracked, texture arrays
// and MSAA are skipped.
//
// Clamp (the fix, on unless the canary is): the same tracking, but the outside region gets the sub-rect's
// last column / row replicated (a band of clamp_band x the texture size beyond the edge), so taps past the
// right/bottom edge (blurs: bloom, DOF, SSDO, ...) read what a clamp sampler gives at the texture border at
// 100% instead of stale texels. Colour targets only (depth-stencil textures are left alone).
namespace kc::canary {

enum class Fill { Mixed = 0, Inverse = 1, NaN = 2 };

struct Settings {
    bool enabled = false;
    Fill fill = Fill::Mixed;
    bool blink = false;
    int blink_frames = 30;
    bool clamp = true;         // edge replicate outside the sub-rect when the canary is off
    float clamp_band = 1.0f;   // band width beyond the edge, fraction of the texture size
};

struct Status {
    bool active = false;        // filling this frame
    bool clear_view = true;     // ID3D11DeviceContext1::ClearView usable (else colour targets are skipped)
    uint32_t fills = 0;         // last frame: fills done
    uint32_t textures = 0;      // last frame: distinct textures filled
    uint32_t depth_fills = 0;   // last frame: of the fills, depth-stencil ones
    uint32_t skipped = 0;       // last frame: screen inputs that could not be filled (format, array, MSAA)
    bool clamping = false;      // this frame fills with the edge clamp, not a marker
    Fill phase = Fill::Mixed;   // fill in use this frame
};

Settings& settings();
const Status& status();

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);

// Render thread, every Present: frame bookkeeping (arms the next frame only if this one had a TAA draw, which ends
// the sub-rect part of the frame).
void on_present();
// Render thread, at the engine's TAA draw, after before_draw: no fills for the rest of the frame.
void on_taa_draw();
// Render thread, before an engine draw / dispatch: fill the bound inputs. draw: the unhooked Draw.
void before_draw(ID3D11DeviceContext* ctx, DrawFn draw, uint32_t vs_slots = ~uint32_t(0),
                 uint32_t ps_slots = ~uint32_t(0));
void before_dispatch(ID3D11DeviceContext* ctx, DrawFn draw, uint32_t cs_slots = ~uint32_t(0));
// A recognized bloom draw filled an entire target. Do not pad/poison this resource.
void preserve_full_target(ID3D11DeviceContext* ctx);
// A recognized water reflection draw writes a bottom-left rectangle (including history).
void record_bottom_target(ID3D11DeviceContext* ctx);
// Render thread: these targets are about to be written (OMSetRenderTargets) / were written (dispatch UAVs).
void on_set_targets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
void on_set_uavs(UINT n, ID3D11UnorderedAccessView* const* uavs);
void after_dispatch(ID3D11DeviceContext* ctx);

}  // namespace kc::canary
