#pragma once
#include <d3d11.h>

#include <cstdint>

// Constant-buffer fixes for draws in the downscaled sub-rect path (kcd-re db/notes/20-resolution.md). For an
// affected draw we bind a GPU copy of the shader's constant buffer with one float2 replaced, then put the engine's
// buffer back.
//
// Fog (pixel shader): GetLinearDepth(Texture2D, ScreenTC) (Common.cfi:262) loads at ScreenTC * PS_ScreenSize.xy,
// but ScreenTC is already scaled by HPosScale and PS_ScreenSize.xy is the scaled viewport: depth read at 0.667x the
// position (zoomed-in ghost, blocky edges). Fix: PS_ScreenSize.xy = zMap size.
// DOF composite (gather DOF, the pause-menu blur) has the same bug in its bicubic layer fetch: same fix, with the
// size of its full-res depth (_tex0).
//
// SSDO (DeferredShading SSDO PS) and the SSS blurs (both directions): rebuild view positions as
// (uv * ViewSpaceParams.xy + ViewSpaceParams.zw) * depth with ViewSpaceParams set for UVs 0..1, but in the sub-rect
// the UVs only span 0..s: the positions, and the occlusion directions from them, were skewed towards the top left.
// SSDO then barely occluded the (blue) sky ambient, which grew with lower scales and depended on the view. Their
// sample radii (SSDOParams, SSSBlurDir) are UV units, so 1/s too wide in screen terms. Fix (on the GPU, like the SVO
// one): ViewSpaceParams.xy /= s, the radii *= s.
//
// SVO GI temporal passes (the 1/6-res one and the upsample): prev = project(SVO_ReprojectionMatrix * (uv, 1, w)),
// a matrix for UVs 0..1. With sub-rect UVs the history was fetched from the wrong place whenever the camera moved:
// ghosting dark edges, noisy GI in motion, leaks that stuck until a resolution change reset the history. Fix: the
// matrix remapped to sub-rect UVs (rows 0, 1 * s, columns x, y * 1/s).
//
// Self-scaling vertex shaders (multiply the input UV by g_VS_HPosScale, e.g. the tonemap VS: o3 = v1 * HPosScale
// for the scene, o1 = v1 for vignetting) already handle the sub-rect with the engine's unscaled triangle. With the
// triangle hook's scaled UVs the tonemap read the top-left 2/3 of the sub-rect (1.5x zoom). Fix: the Draw hook skips
// such a draw and the triangle is redrawn unscaled (game::redo_fullscreen_tri_unscaled).
namespace kc::screen_size_fix {

// Auto exposure (HDRSampleLumInitial): left alone. The engine already draws its quad with sub-rect UVs (0..s) and
// the VS multiplies them by g_VS_HPosScale (1 for this pass); patching that to s as well read the top-left s^2 only.
// SVO GI cone trace: for far points it marches in screen space, projecting world points with g_mViewProj to a
// full-screen UV (clip * 0.5 + 0.5) and reading the depth buffer there. Below scale 1 that reads outside the current
// frame's sub-rect, the march reports occlusion and the pixel skips the cone trace: distant GI went ~3x darker. Fix:
// bind a copy of its constant buffer whose g_mViewProj maps clip x/y into the sub-rect (done on the GPU).
// Both SSDO and that trace also read depth past the sub-rect's right/bottom edge (SSDO's minimum tap radius is 0.1
// of the screen), where the linear depth targets hold 1.0 = sky: unoccluded sky ambient, a blue glow along the right
// edge under the canopy. Before those draws the outside region of their depth inputs gets the last valid column/row.
// SSDO noise tiling (DirOccPassVS/PS): the sample-rotation index is frac(uv * g_ScreenSize.xy * 0.25), a 4x4 pixel
// tile that the SSDO blur (2-texel steps) averages out, and the max radius alternates with int(uv * PS_ScreenSize.xy)
// & 1. Both sizes are the scaled viewport while the UVs span 0..s, so the tile grew to 4/s pixels (8 at
// Performance): the blur no longer cancelled it and vertical stripes (the index mostly varies along x) showed on
// occluded walls, sharpened further by DLSS. Fix: during the SSDO draw the VS's g_ScreenSize.xy and the PS's
// PS_ScreenSize.xy are divided by s (zw, the half-texel sizes, multiplied).
struct Shader {
    enum Kind { FogPS, LumInitialPS, ViewProjPS, SelfScalingVS, DofPS, DofGatherPS, DofVS, ViewSpacePS, ReprojPS, TiledShadingCS, ScreenSizeVS } kind = FogPS;
    void* shader = nullptr;  // ID3D11PixelShader* / ID3D11VertexShader*, identity only
    UINT cb_slot = ~0u;      // constant buffer holding the value to replace
    UINT offset = 0;         // byte offset of it
    UINT zmap_slot = ~0u;    // pixel shaders: zMap texture (its size is the full size)
    UINT radius_offset = ~0u;  // ViewSpacePS: SSDOParams / SSSBlurDir (UV-unit radii), same buffer as ViewSpaceParams
    bool radius_xy_only = false;  // SSSBlurDir: only .xy are radii
    UINT size_slot = ~0u, size_offset = ~0u;  // SSDO PS: PS_ScreenSize (ScreenSizeVS: g_ScreenSize is cb_slot/offset)
    UINT tiled[3] = {~0u, ~0u, ~0u};  // TiledShadingCS: FrustumTR, FrustumBL, ProjParams (offset: FrustumTL)
};

// From CreatePixelShader / CreateVertexShader: true (and *out filled) for a shader that needs a fix.
bool reflect_ps(const void* code, SIZE_T size, Shader* out);
bool reflect_vs(const void* code, SIZE_T size, Shader* out);
bool reflect_cs(const void* code, SIZE_T size, Shader* out);

struct Status {
    bool enabled = true;
    bool ssdo_noise = true;  // SSDO noise tiling fix (needs enabled too)
    uint32_t ps_seen = 0, vs_seen = 0;
    uint64_t ps_fixed = 0, vs_fixed = 0, shrunk = 0, non_screen = 0, bloom_fixed = 0,
             dof_shrunk = 0, edge_padded = 0, ssdo_noise_fixed = 0;
};
Status& status();

using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
// Render thread, from the Draw hook, for a full-screen triangle whose UVs we scaled. The sub-rect invariant is "every
// screen buffer holds the current frame in its top-left (scale x scale) part":
//  - the bound VS scales the UVs itself (see above): Redraw with the engine's unscaled triangle;
//  - the viewport covers a whole screen-proportional target (SVO GI's 1/6-res passes, 426x240 on 426x240): Shrink
//    the viewport to the sub-rect. Keeping 1:1 UVs there preserved the layout, but the WPOS camera rays then spread
//    over the whole target while the depth covered the sub-rect: wrong world positions, GI "cloud" on near foliage;
//  - the viewport covers a whole non-screen target (2:1 sky textures, LUTs), or part of one whose t0 is not
//    screen-shaped either (texToTex reduction chains): Redraw unscaled;
//  - otherwise (the viewport already is the sub-rect): Draw as is.
enum class TriAction { Draw, Redraw, Shrink };
TriAction scaled_tri_action(ID3D11DeviceContext* ctx, const Shader* vs);
// Render thread, from the Draw hook while a fixed PS is bound (vs: the bound VS's entry, or nullptr).
void draw(ID3D11DeviceContext* ctx, const Shader& ps, const Shader* vs, UINT count, UINT start, DrawFn original);

// Bloom: the quarter-res scene (the luminance pass's source) holds the frame in its sub-rect, but the bloom blur
// passes run with g_VS_HPosScale.xy = 1 and the tonemap reads the bloom with unscaled UVs (o1 = v1), so it showed the
// sub-rect's bloom stretched 1/scale: a bright haze (the sunlit path's bloom) over the screen centre that shrank
// towards the top left with the scale. Fix: the first blur pass (a self-scaling VS reading that texture at t0) gets
// g_VS_HPosScale.xy = downscale, so it stretches the sub-rect over its whole target and the rest of the chain and the
// tonemap see a full-layout bloom. Returns false (nothing drawn) for any other draw.
// Gather DOF (MotionBlur.cfx: prep, TileMinCoC chain, DofPS gather; the pause-menu blur). The engine draws these
// with its own source region = the sub-rect (UVs 0..s) but full viewports, so every pass stretched its input by 1/s
// and the chain compounded into a 4x zoom at s = 0.5. Fix: shrink the viewport to the sub-rect like our own scaled
// triangles (every DOF buffer then holds the frame in its top-left part), and give DofPS a PS_ScreenSize.xy scaled
// by s so its CoC radius stays the same fraction of the image. The composite already draws into the sub-rect.
// Returns false (nothing drawn) when the draw is not one of these.
bool draw_dof(ID3D11DeviceContext* ctx, const Shader* ps, const Shader* vs, UINT count, UINT start, DrawFn original);

bool draw_bloom_source(ID3D11DeviceContext* ctx, const Shader& vs, UINT count, UINT start, DrawFn original);

// Tiled deferred shading (TiledShading.cfx, the main lighting CS): runs over the sub-rect, but its per-pixel view
// ray is FrustumTL + (pixel / ScreenSize) * (TR - TL, BL - TL) with ScreenSize the full target, so below scale 1 every
// ray pointed into the top-left part of the view: wrong view vectors for sun specular and environment reflections
// (a blue sky sheen on trunks and foliage, growing as the scale drops). Its light-culling tile planes use the full
// tile grid (ScreenSize / 8) around ProjParams too. Its GI fetch ((pixel + 0.5) / ScreenSize) is right for the
// sub-rect, so ScreenSize stays. Fix: TR' = TL + (TR - TL) / s.x, BL' = TL + (BL - TL) / s.y, and ProjParams scaled
// and shifted to the sub-rect's tile grid.
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
void dispatch(ID3D11DeviceContext* ctx, const Shader& cs, UINT x, UINT y, UINT z, DispatchFn original);

}  // namespace kc::screen_size_fix
