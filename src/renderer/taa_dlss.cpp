#include "renderer/taa_dlss.h"

#include <windows.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstring>
#include <optional>

#include "core/log.h"
#include "game/renderer.h"
#include "game/symbols.h"
#include "renderer/frame_dump.h"
#include "renderer/ui_depth.h"
#include "renderer/mip_bias.h"
#include "renderer/shadow_noise.h"
#include "renderer/state_backup.h"
#include "renderer/velocity_mask.h"
#include "upscaler/dlss.h"

namespace kc::taa_dlss {

static Settings g_settings;
static Status g_status;
static bool g_reset = true;

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }
void request_reset() { g_reset = true; }

// ---------------------------------------------------------------------------------------------------------
// Shaders

// Mirrors PostAA_PS (PostAA.cfx, 2TX path) and DecodeMotionVector (Common.cfi:306).
static const char kPrepareCS[] = R"(
Texture2D<float4> InColour : register(t0);
Texture2D<float>  InDepth  : register(t1);
Texture2D<float2> InVel    : register(t2);
Texture2D<float>  VelDepth : register(t3);  // depth right after the moving objects (velocity_mask)
cbuffer Engine : register(b0) { float4 E[256]; };  // the engine's constant buffer with mReprojection
cbuffer Params : register(b1) {
    uint  ReprojReg;   // float4 register of mReprojection in Engine
    uint  RowMajor;
    float2 InvTarget;  // 1 / target texture size: the engine's baseTC is normalized to the whole texture
    uint2 RenderSize;
    float2 MvSign;
    float2 Scale;      // render size / target size: the sub-rect the scene occupies
    uint  VelMask;     // VelDepth is valid: object velocity only where the depth is still the moving object's
};
RWTexture2D<float4> OutColour : register(u0);
RWTexture2D<float>  OutDepth  : register(u1);
RWTexture2D<float2> OutMv     : register(u2);

float2 DecodeMotionVector(float2 e) {
    e = (e - 127.0 / 255.0) * 2.0;
    return (e * e) * float2(e.x > 0.0 ? 1.0 : -1.0, e.y > 0.0 ? 1.0 : -1.0);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
    if (any(id >= RenderSize))
        return;
    int3 p = int3(id, 0);
    float depth = InDepth.Load(p);
    float2 tc = (float2(id) + 0.5) * InvTarget;
    // mReprojection is built for full-view UVs (CPostAA::Render has no HPosScale term), so camera motion is
    // reprojected at tc / Scale and brought back to sub-rect units. Object velocities are already in sub-rect
    // units (HPosToScreenTC scales them).
    float2 uv = tc / Scale;
    // Sky / far plane: the reprojection's w goes to 0 there. The engine's TAA shrugs off the resulting Inf/NaN
    // (treated as offscreen); DLSS spreads it through its history, so keep the depth off the exact limits and
    // drop any non-finite or absurd vector.
    float4 v = float4(uv, clamp(depth, 1e-6, 1.0 - 1e-6), 1.0);
    float4 h;
    if (RowMajor)
        h = v.x * E[ReprojReg] + v.y * E[ReprojReg + 1] + v.z * E[ReprojReg + 2] + v.w * E[ReprojReg + 3];
    else
        h = float4(dot(v, E[ReprojReg]), dot(v, E[ReprojReg + 1]), dot(v, E[ReprojReg + 2]), dot(v, E[ReprojReg + 3]));
    float2 obj = InVel.Load(p);
    if (VelMask && VelDepth.Load(p) != InDepth.Load(p))
        obj = 0.0;  // covered by something drawn later (static geometry never writes velocity)
    float2 mv = (obj.x == 0.0) ? (h.xy / h.w - uv) * Scale : DecodeMotionVector(obj);
    if (any(isnan(mv)) || any(isinf(mv)) || abs(h.w) < 1e-8 || any(abs(mv) > 2.0))
        mv = 0.0;
    OutMv[id] = mv * MvSign;
    OutDepth[id] = clamp(depth, 1e-6, 1.0 - 1e-6);  // reversed Z: the sky is exactly 0
    OutColour[id] = InColour.Load(p);
}
)";

// Full-screen triangle; output re-encoded to sRGB like the end of PostAA_PS. Alpha 0 like PostAA_PS (pixout zeroed).
static const char kWritebackVS[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";
static const char kWritebackPS[] = R"(
Texture2D<float4> Src    : register(t0);  // DLSS output
Texture2D<float4> Colour : register(t1);  // DLSS colour input
Texture2D<float2> Mv     : register(t2);
Texture2D<float>  Depth  : register(t3);
cbuffer View : register(b0) { uint Mode; float MvGain; float2 Scale; };  // Scale: render / target size
bool bad(float4 v) { return any(isnan(v)) || any(isinf(v)); }
float4 main(float4 pos : SV_Position) : SV_Target {
    // The inputs hold the frame in their top-left (Scale) part: debug views stretch it over the target,
    // the colour input bilinearly (stands in for DLSS with Skip NGX, e.g. under RenderDoc). The DLSS output
    // is already full size and is read 1:1.
    float2 src = pos.xy * Scale - 0.5;
    int3 p = int3(pos.xy * Scale, 0);
    float3 c;
    if (Mode == 1) {            // colour input
        int2 i = int2(floor(src));
        float2 f = src - floor(src);
        c = lerp(lerp(Colour.Load(int3(max(i, 0), 0)).rgb, Colour.Load(int3(max(i + int2(1, 0), 0), 0)).rgb, f.x),
                 lerp(Colour.Load(int3(max(i + int2(0, 1), 0), 0)).rgb, Colour.Load(int3(max(i + 1, 0), 0)).rgb, f.x),
                 f.y);
    } else if (Mode == 2) {     // motion vectors: x red, y green (signed, gained), NaN magenta
        float2 m = Mv.Load(p);
        if (bad(m.xyxy)) return float4(1, 0, 1, 0);
        c = float3(saturate(0.5 + m * MvGain), 0.0);
        c = c * c;              // undo the sRGB encode below so mid grey stays 0.5
    } else if (Mode == 3) {     // depth
        c = Depth.Load(p).xxx;
    } else {                    // DLSS output, NaN/Inf magenta
        float4 o = Src.Load(int3(pos.xy, 0));
        if (bad(o)) return float4(1, 0, 1, 0);
        c = o.rgb;
    }
    c = saturate(c);
    c = (c < 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float4(c, 0.0);
}
)";

static ID3D11ComputeShader* g_prepare_cs;
static ID3D11VertexShader* g_writeback_vs;
static ID3D11PixelShader* g_writeback_ps;
static ID3D11Buffer* g_params;
static ID3D11Buffer* g_view_params;
static ID3DDeviceContextState* g_state;  // our private pipeline state (SwapDeviceContextState)
static bool g_shaders_failed;

static ID3DBlob* compile(const char* src, size_t len, const char* target, const char* name) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &errors);
    if (FAILED(hr))
        logf("taa_dlss: compile %s failed 0x%08lx: %s", name, hr,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    if (errors)
        errors->Release();
    return SUCCEEDED(hr) ? code : nullptr;
}

static bool ensure_shaders(ID3D11Device* device) {
    if (g_status.shaders_ready || g_shaders_failed)
        return g_status.shaders_ready;
    d3d11::ScopedInside inside;
    ID3DBlob* cs = compile(kPrepareCS, sizeof(kPrepareCS) - 1, "cs_5_0", "kcd_prepare_cs");
    ID3DBlob* vs = compile(kWritebackVS, sizeof(kWritebackVS) - 1, "vs_5_0", "kcd_writeback_vs");
    ID3DBlob* ps = compile(kWritebackPS, sizeof(kWritebackPS) - 1, "ps_5_0", "kcd_writeback_ps");
    bool ok = cs && vs && ps &&
              SUCCEEDED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &g_prepare_cs)) &&
              SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_writeback_vs)) &&
              SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_writeback_ps));
    for (ID3DBlob* b : {cs, vs, ps})
        if (b)
            b->Release();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_params));
    bd.ByteWidth = 16;
    ok = ok && SUCCEEDED(device->CreateBuffer(&bd, nullptr, &g_view_params));

    ID3D11Device1* device1 = nullptr;
    if (ok && SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)))) {
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        ok = SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                         nullptr, &g_state));
        device1->Release();
    } else {
        ok = false;
    }
    g_status.shaders_ready = ok;
    g_shaders_failed = !ok;
    logf("taa_dlss: shaders and state %s", ok ? "ready" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------
// Working textures (target size)

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    void release() {
        if (uav) uav->Release();
        if (srv) srv->Release();
        if (tex) tex->Release();
        *this = Tex{};
    }
};
static Tex g_colour, g_depth, g_mv, g_out;
static UINT g_tex_w, g_tex_h;

static bool make(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, Tex* t) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t->tex)) &&
           SUCCEEDED(device->CreateShaderResourceView(t->tex, nullptr, &t->srv)) &&
           SUCCEEDED(device->CreateUnorderedAccessView(t->tex, nullptr, &t->uav));
}

static bool ensure_textures(ID3D11Device* device, UINT w, UINT h) {
    if (g_status.resources_ready && w == g_tex_w && h == g_tex_h)
        return true;
    for (Tex* t : {&g_colour, &g_depth, &g_mv, &g_out})
        t->release();
    bool ok = make(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_colour) &&
              make(device, w, h, DXGI_FORMAT_R32_FLOAT, &g_depth) &&
              make(device, w, h, DXGI_FORMAT_R16G16_FLOAT, &g_mv) &&
              make(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, &g_out);
    g_tex_w = w;
    g_tex_h = h;
    g_status.resources_ready = ok;
    g_reset = true;
    logf("taa_dlss: working textures %ux%u %s", w, h, ok ? "ready" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------------------------------------------------

// Render scale in use: only below 1 while DLSS is on and can run (or Skip NGX stands in for it), else the engine's
// own chain would show the small sub-rect (it has no upscale on PC).
static float wanted_scale() {
    if (g_settings.mode != Mode::Dlss)
        return 1.0f;
    if (g_settings.skip_ngx)  // no DLSS (e.g. RenderDoc): the write-back stretches the sub-rect instead
        return g_settings.render_scale;
    const dlss::Status& d = dlss::status();
    if (d.init_tried && !d.available)
        return 1.0f;
    return g_settings.render_scale;
}

// Holds an int cvar at a value while `on` and puts the user's value back afterwards.
struct CvarOverride {
    sym::Id id;
    const char* name;
    int saved = -1;
    bool active = false;
    void apply(bool on, int value) {
        int* p = sym::ptr<int>(id);
        if (!p)
            return;
        if (on) {
            if (!active) {
                saved = *p;
                active = true;
                logf("taa_dlss: %s %d -> %d", name, saved, value);
            }
            *p = value;
        } else if (active) {
            *p = saved;
            active = false;
            logf("taa_dlss: %s restored to %d", name, saved);
        }
    }
};

void on_present() {
    float scale = wanted_scale();
    game::set_downscale(scale);
    bool dlss_on = g_settings.mode == Mode::Dlss && (g_settings.skip_ngx || dlss::status().available);
    mip_bias::set_bias(dlss_on && g_settings.mip_bias ? log2f(scale) + g_settings.mip_bias_offset : 0.0f);
    shadow_noise::set_active(dlss_on && g_settings.temporal_shadow_noise);
    // Keep the noise no finer than at scale 0.5 (fine there per user tests): k = ceil(0.5 / scale).
    shadow_noise::set_coarseness(dlss_on && g_settings.coarse_shadow_noise && scale < 0.5f
                                     ? static_cast<int>(ceilf(0.5f / scale - 0.01f)) : 1);
    // PackVelocitiesPS mixes scaled and full-screen UVs in the sub-rect path (zoom blur), kcd-re notes/20-resolution.md.
    static CvarOverride motion_blur{sym::Id::CV_r_MotionBlur, "r_MotionBlur"};
    motion_blur.apply(scale < 1.0f, 0);

    // Put the user's pattern back when we stop forcing, so turning DLSS off really returns to stock.
    static int saved_pattern = -1;
    int* p = sym::ptr<int>(sym::Id::CV_r_AntialiasingTAAPattern);
    if (!p)
        return;
    bool force = g_settings.mode == Mode::Dlss && g_settings.force_halton;
    if (force) {
        if (saved_pattern < 0 && *p != 8) {
            saved_pattern = *p;
            logf("taa_dlss: r_AntialiasingTAAPattern %d -> 8", saved_pattern);
        }
        *p = 8;
    } else if (saved_pattern >= 0) {
        *p = saved_pattern;
        logf("taa_dlss: r_AntialiasingTAAPattern restored to %d", saved_pattern);
        saved_pattern = -1;
    }
}

// Engine bindings at the TAA draw, AddRef'd by the Get calls.
struct Bound {
    ID3D11ShaderResourceView* colour = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* velocity = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_VIEWPORT viewport{};
    ~Bound() {
        for (IUnknown* u : {static_cast<IUnknown*>(colour), static_cast<IUnknown*>(depth),
                            static_cast<IUnknown*>(velocity), static_cast<IUnknown*>(cb), static_cast<IUnknown*>(rtv)})
            if (u)
                u->Release();
    }
};

static bool fail(const char* why) {
    if (strcmp(g_status.last_error, why) != 0)
        logf("taa_dlss: falling back to engine TAA: %s", why);
    g_status.last_error = why;
    g_status.last_ok = false;
    return false;
}

static float read_float(uintptr_t base, uintptr_t offset) {
    float v;
    memcpy(&v, reinterpret_cast<const void*>(base + offset), sizeof(v));
    return v;
}

bool on_taa_draw(ID3D11DeviceContext* ctx, const d3d11::TaaShader& taa) {
    if (g_settings.mode != Mode::Dlss)
        return false;

    Bound b;
    ctx->PSGetShaderResources(taa.slot_colour, 1, &b.colour);
    ctx->PSGetShaderResources(taa.slot_depth, 1, &b.depth);
    ctx->PSGetShaderResources(taa.slot_velocity, 1, &b.velocity);
    ctx->PSGetConstantBuffers(taa.cb_slot, 1, &b.cb);
    ctx->OMGetRenderTargets(1, &b.rtv, nullptr);
    UINT nvp = 1;
    ctx->RSGetViewports(&nvp, &b.viewport);
    if (!b.colour || !b.depth || !b.velocity || !b.cb || !b.rtv || nvp == 0)
        return fail("TAA draw without the expected bindings");

    ID3D11Resource* target = nullptr;
    b.rtv->GetResource(&target);
    D3D11_TEXTURE2D_DESC td;
    static_cast<ID3D11Texture2D*>(target)->GetDesc(&td);
    target->Release();
    // With SetViewportDownscale the scene (and the TAA draw) covers the top-left render_w x render_h of
    // full-size targets; DLSS reads that sub-rect and writes the whole target, which the composite then shows 1:1.
    const UINT render_w = static_cast<UINT>(b.viewport.Width), render_h = static_cast<UINT>(b.viewport.Height);
    if (b.viewport.TopLeftX != 0 || b.viewport.TopLeftY != 0 || render_w == 0 || render_h == 0 ||
        render_w > td.Width || render_h > td.Height)
        return fail("unexpected TAA viewport");

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    bool ready = ensure_shaders(device) && ensure_textures(device, td.Width, td.Height);
    device->Release();
    if (!ready)
        return fail("shader or texture setup failed");

    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))))
        return fail("no ID3D11DeviceContext1");

    // Jitter: RT_RenderScene stores 2*j/(render size) in clip space at gRenDev+0xF20 (kcd-re notes/40-jitter.md).
    float jx = 0, jy = 0;
    if (uintptr_t r = sym::addr(sym::Id::gRenDev)) {
        jx = read_float(r, 0xF20) * render_w * 0.5f;
        jy = read_float(r, 0xF24) * render_h * 0.5f;
    }
    if (g_settings.jitter_flip_x)
        jx = -jx;
    if (g_settings.jitter_flip_y)
        jy = -jy;
    g_status.jitter_x = jx;
    g_status.jitter_y = jy;

    int* reverse = sym::ptr<int>(sym::Id::CV_r_ReverseDepth);
    g_status.reverse_depth = reverse ? *reverse : -1;
    dlss::Settings& ds = dlss::settings();
    ds.is_hdr = true;  // colour is linear (sRGB view of the SMAA output), in [0, 1]
    ds.depth_inverted = g_settings.depth_mode == 2 || (g_settings.depth_mode == 0 && g_status.reverse_depth > 0);

    ID3D11ShaderResourceView* vel_depth = velocity_mask::depth_copy();
    // Params for the prepare pass.
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ctx1->Release();
        return fail("Map failed");
    }
    struct {
        UINT reproj_reg, row_major;
        float inv_target[2];
        UINT render[2];
        float mv_sign[2];
        float scale[2];
        UINT vel_mask;
        float pad;
    } params = {taa.reproj_offset / 16, taa.reproj_row_major ? 1u : 0u,
                {1.0f / td.Width, 1.0f / td.Height}, {render_w, render_h},
                {g_settings.mv_flip_x ? -1.0f : 1.0f, g_settings.mv_flip_y ? -1.0f : 1.0f},
                {static_cast<float>(render_w) / td.Width, static_cast<float>(render_h) / td.Height},
                vel_depth ? 1u : 0u, 0};
    memcpy(m.pData, &params, sizeof(params));
    ctx->Unmap(g_params, 0);
    if (SUCCEEDED(ctx->Map(g_view_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        UINT view_mode = g_settings.skip_ngx && g_settings.debug_view == 0 ? 1u : static_cast<UINT>(g_settings.debug_view);
        struct { UINT mode; float mv_gain; float scale[2]; } view = {
            view_mode, 50.0f, {static_cast<float>(render_w) / td.Width, static_cast<float>(render_h) / td.Height}};
        memcpy(m.pData, &view, sizeof(view));
        ctx->Unmap(g_view_params, 0);
    }

    // Everything from here, NGX included, must leave the engine's bindings (and its cached view of them) as they
    // were. Default: save/restore by hand. SwapDeviceContextState (kept for comparison) also loses the NVAPI depth
    // bounds state the engine's deferred lighting relies on, which blacks out lighting for good.
    ID3DDeviceContextState* game_state = nullptr;
    std::optional<StateBackup> backup;
    if (g_settings.state_swap) {
        ctx1->SwapDeviceContextState(g_state, &game_state);
    } else {
        backup.emplace(ctx);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(nullptr);
    }
    auto restore = [&] {
        if (g_settings.state_swap) {
            ctx1->SwapDeviceContextState(game_state, nullptr);
            if (game_state)
                game_state->Release();
        }
        backup.reset();
        ctx1->Release();
    };

    bool recreated = false;
    if (!g_settings.skip_ngx && !dlss::ensure(ctx, render_w, render_h, td.Width, td.Height, &recreated)) {
        restore();
        return fail("DLSS not available (see log)");
    }

    // 1. Colour/depth copies and full-screen motion vectors.
    ID3D11ShaderResourceView* srvs[4] = {b.colour, b.depth, b.velocity, vel_depth};
    ID3D11UnorderedAccessView* uavs[3] = {g_colour.uav, g_depth.uav, g_mv.uav};
    ID3D11Buffer* cbs[2] = {b.cb, g_params};
    ctx->CSSetShader(g_prepare_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 4, srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->Dispatch((render_w + 7) / 8, (render_h + 7) / 8, 1);
    ID3D11ShaderResourceView* null_srvs[4] = {};
    ID3D11UnorderedAccessView* null_uavs[3] = {};
    ID3D11Buffer* null_cbs[2] = {};
    ctx->CSSetShaderResources(0, 4, null_srvs);
    ctx->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 2, null_cbs);

    // 2. DLSS.
    dlss::EvalInputs in;
    in.colour = g_colour.tex;
    in.depth = g_depth.tex;
    in.motion = g_mv.tex;
    in.output = g_out.tex;
    in.render_width = render_w;
    in.render_height = render_h;
    in.jitter_x = jx;
    in.jitter_y = jy;
    in.mv_scale_x = static_cast<float>(td.Width);  // MVs are in texture UV units
    in.mv_scale_y = static_cast<float>(td.Height);
    in.reset = g_reset || recreated;
    bool ok = g_settings.skip_ngx ? true : dlss::evaluate(ctx, in);
    g_reset = false;

    // 3. Into the engine's TAA output, encoded like PostAA_PS.
    if (ok || g_settings.debug_view != 0) {
        D3D11_VIEWPORT full = {0, 0, static_cast<float>(td.Width), static_cast<float>(td.Height), 0, 1};
        ctx->OMSetRenderTargets(1, &b.rtv, nullptr);
        ctx->RSSetViewports(1, &full);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_writeback_vs, nullptr, 0);
        ctx->PSSetShader(g_writeback_ps, nullptr, 0);
        ID3D11ShaderResourceView* views[4] = {g_out.srv, g_colour.srv, g_mv.srv, g_depth.srv};
        ctx->PSSetShaderResources(0, 4, views);
        ctx->PSSetConstantBuffers(0, 1, &g_view_params);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* null_srvs4[4] = {};
        ID3D11Buffer* null_cb = nullptr;
        ID3D11RenderTargetView* null_rtv = nullptr;
        ctx->PSSetShaderResources(0, 4, null_srvs4);
        ctx->PSSetConstantBuffers(0, 1, &null_cb);
        ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
    }

    restore();

    if (frame_dump::armed()) {
        ID3D11Resource* rt = nullptr;
        b.rtv->GetResource(&rt);
        ID3D11Resource* col = nullptr;
        b.colour->GetResource(&col);
        frame_dump::save(ctx, col, "engine_colour");
        frame_dump::save(ctx, g_colour.tex, "dlss_in_colour");
        frame_dump::save(ctx, g_depth.tex, "dlss_in_depth");
        frame_dump::save(ctx, g_mv.tex, "dlss_in_mv");
        frame_dump::save(ctx, g_out.tex, "dlss_out");
        frame_dump::save(ctx, rt, "taa_target");
        logf("frame_dump: render %ux%u target %ux%u viewport (%.0f,%.0f %.0fx%.0f) jitter (%.3f, %.3f) ok %d",
             render_w, render_h, td.Width, td.Height, b.viewport.TopLeftX, b.viewport.TopLeftY, b.viewport.Width,
             b.viewport.Height, jx, jy, ok);
        if (rt)
            rt->Release();
        if (col)
            col->Release();
    }

    g_status.render_width = render_w;
    g_status.render_height = render_h;
    g_status.target_width = td.Width;
    g_status.target_height = td.Height;
    if (ok)
        ui_depth::after_taa(render_w, render_h, td.Width, td.Height);
    ++g_status.frames;
    if (!ok)
        return fail("DLSS evaluate failed");
    g_status.last_ok = true;
    g_status.last_error = "";
    return !g_settings.engine_after;
}

}  // namespace kc::taa_dlss
