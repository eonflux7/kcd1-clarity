#include "renderer/canary.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "game/renderer.h"
#include "renderer/state_backup.h"
#include "renderer/padding_state.h"
#include "renderer/resource_slots.h"
#include "renderer/taa_dlss.h"

namespace kc::canary {

static Settings g_settings;
static Status g_status;
static Status g_frame;  // counters of the frame in progress
static bool g_armed;
static bool g_taa_this_frame;
static uint64_t g_frame_no;
static game::Vec2 g_native;

// (texture, mip) filled since it was last bound for writing, this frame.
static std::unordered_set<uint64_t> g_filled;
static std::unordered_set<void*> g_textures;
// Descriptors are immutable. Hold references so a released view address cannot
// identify a different input later in the frame. Writes still invalidate g_filled.
struct Input {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    D3D11_TEXTURE2D_DESC texture{};
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    bool screen = false;
};
static std::unordered_map<ID3D11ShaderResourceView*, Input> g_inputs;
// References prevent pointer reuse until Present; a new write removes the exemption.
static std::vector<ID3D11Resource*> g_full_layout;
// Water reflection history survives Present and alternates between two textures.
// Pin identities and retain the last producer's actual rectangle until a new write.
struct BottomTarget { ID3D11Resource* resource; UINT width, height; };
static std::vector<BottomTarget> g_bottom_targets;
static void clear_bottom_targets() {
    for (auto& t : g_bottom_targets) t.resource->Release();
    g_bottom_targets.clear();
}
static void forget_bottom_target(ID3D11Resource* resource) {
    auto it = std::find_if(g_bottom_targets.begin(), g_bottom_targets.end(),
                           [resource](const auto& t) { return t.resource == resource; });
    if (it != g_bottom_targets.end()) {
        it->resource->Release();
        g_bottom_targets.erase(it);
    }
}
void record_bottom_target(ID3D11DeviceContext* ctx) {
    const auto& s = taa_dlss::settings();
    if (s.mode != taa_dlss::Mode::Dlss || s.render_scale >= .999f) return;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rt;
    ctx->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt) return;
    D3D11_RENDER_TARGET_VIEW_DESC vd{}; rt->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource; rt->GetResource(&resource);
    D3D11_RESOURCE_DIMENSION dimension; resource->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC td{}; static_cast<ID3D11Texture2D*>(resource.Get())->GetDesc(&td);
    D3D11_VIEWPORT vp{}; UINT n = 1; ctx->RSGetViewports(&n, &vp);
    auto d = game::current_downscale();
    UINT w = UINT(floorf(float(td.Width) * d.x + .05f));
    UINT h = UINT(floorf(float(td.Height) * d.y + .05f));
    if (n != 1 || td.ArraySize != 1 || td.SampleDesc.Count != 1 || !w || !h ||
        w > td.Width || h >= td.Height || vp.TopLeftX != 0 || vp.TopLeftY != float(td.Height-h) ||
        vp.Width != float(w) || vp.Height != float(h)) return;
    forget_bottom_target(resource.Get());
    g_bottom_targets.push_back({resource.Detach(), w, h});
}

static void clear_full_layout() {
    for (auto* r : g_full_layout)
        r->Release();
    g_full_layout.clear();
}
static void forget_full_layout(ID3D11Resource* r) {
    auto it = std::find(g_full_layout.begin(), g_full_layout.end(), r);
    if (it != g_full_layout.end()) {
        (*it)->Release();
        g_full_layout.erase(it);
    }
}
static void invalidate_outputs(ID3D11DeviceContext* ctx, bool compute) {
    if (g_full_layout.empty())
        return;
    if (compute) {
        ID3D11UnorderedAccessView* views[8]{};
        ctx->CSGetUnorderedAccessViews(0, 8, views);
        for (auto* v : views)
            if (v) {
                ID3D11Resource* r = nullptr;
                v->GetResource(&r);
                forget_full_layout(r);
                r->Release();
                v->Release();
            }
    } else {
        ID3D11RenderTargetView* views[8]{};
        ctx->OMGetRenderTargets(8, views, nullptr);
        for (auto* v : views)
            if (v) {
                ID3D11Resource* r = nullptr;
                v->GetResource(&r);
                forget_full_layout(r);
                r->Release();
                v->Release();
            }
    }
}
void preserve_full_target(ID3D11DeviceContext* ctx) {
    const auto& s = taa_dlss::settings();
    if (s.mode != taa_dlss::Mode::Dlss || s.render_scale >= .999f)
        return;
    ID3D11RenderTargetView* rt = nullptr;
    ctx->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt)
        return;
    D3D11_RENDER_TARGET_VIEW_DESC vd;
    rt->GetDesc(&vd);
    ID3D11Resource* r = nullptr;
    rt->GetResource(&r);
    rt->Release();
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    D3D11_TEXTURE2D_DESC td{};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
    D3D11_VIEWPORT vp{};
    UINT n = 1;
    ctx->RSGetViewports(&n, &vp);
    if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && vd.Texture2D.MipSlice == 0 &&
        td.MipLevels == 1 && td.ArraySize == 1 && td.SampleDesc.Count == 1 && n == 1 && vp.TopLeftX == 0 &&
        vp.TopLeftY == 0 && vp.Width == td.Width && vp.Height == td.Height &&
        std::find(g_full_layout.begin(), g_full_layout.end(), r) == g_full_layout.end()) {
        if (g_full_layout.size() >= 16)
            clear_full_layout();
        g_full_layout.push_back(r);
        r = nullptr;
    }
    if (r)
        r->Release();
}
// Views we created for filling, kept (with a reference on the texture) until the cache is flushed. nullptr: failed.
static std::map<std::tuple<void*, UINT, DXGI_FORMAT>, ID3D11View*> g_views;
static std::set<std::tuple<UINT, UINT, DXGI_FORMAT, UINT>> g_logged;

static ID3D11DeviceContext* g_ctx1_for;
static ID3D11DeviceContext1* g_ctx1;

Settings& settings() { return g_settings; }
static bool on() { return g_settings.enabled || g_settings.clamp; }
const Status& status() { return g_status; }

static uint64_t key(const void* res, UINT mip) { return (uint64_t(uintptr_t(res)) << 4) | (mip & 15); }

static void flush_views() {
    for (auto& [k, v] : g_views)
        if (v)
            v->Release();
    g_views.clear();
}

void on_present() {
    g_frame.active = g_status.active;
    g_frame.clear_view = g_status.clear_view;
    g_frame.phase = g_status.phase;
    g_frame.clamping = g_status.clamping;
    g_status = g_frame;
    g_frame = {};
    g_filled.clear();
    g_inputs.clear();
    clear_full_layout();
    g_textures.clear();
    const bool had_taa = g_taa_this_frame;
    g_taa_this_frame = false;
    ++g_frame_no;
    game::Vec2 native = game::native_size();
    if (!on() || native.x != g_native.x || native.y != g_native.y) {
        flush_views();
        clear_bottom_targets();
        g_native = native;
    }
    if (!on()) {
        g_armed = false;
        g_status.active = false;
        return;
    }
    const uint64_t period = g_settings.blink_frames > 0 ? uint64_t(g_settings.blink_frames) : 1;
    g_status.clamping = !g_settings.enabled;
    g_status.phase = !g_settings.blink ? g_settings.fill : ((g_frame_no / period) & 1) ? Fill::Inverse : Fill::Mixed;
    game::Vec2 d = game::current_downscale();
    g_armed = had_taa;  // without a TAA draw nothing would end the fills (UI and full-size targets)
    g_status.active = g_armed && (d.x < 0.999f || d.y < 0.999f);
}

void on_taa_draw() {
    g_taa_this_frame = true;
    g_armed = false;
}

static int channels(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 3;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
        return 2;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 4;
    }
}

static bool is_float(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
        return true;
    default:
        return false;
    }
}

static void colour_value(Fill fill, DXGI_FORMAT f, float out[4]) {
    if (fill == Fill::NaN) {
        for (int i = 0; i < 4; ++i)
            out[i] = std::numeric_limits<float>::quiet_NaN();
        return;
    }
    const float m = is_float(f) ? 64.0f : 1.0f;
    const bool colour = channels(f) >= 3;
    const bool bright = (fill == Fill::Mixed) == colour;  // Mixed: magenta / 0, Inverse: black / bright
    const float v[4] = {bright ? m : 0, bright && !colour ? m : 0, bright ? m : 0, bright ? 1.0f : 0};
    memcpy(out, v, sizeof(v));
}

// Depth-stencil view format for a depth SRV format; UNKNOWN for stencil-only views and non-depth formats.
static DXGI_FORMAT dsv_format(DXGI_FORMAT srv) {
    switch (srv) {
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_D16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Depth fill: a full-target triangle writing only depth, discarded inside the sub-rect.
static const char kDepthVS[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
static const char kDepthPS[] = R"(
cbuffer Params : register(b0) { uint w; uint h; float depth; float pad; };
float main(float4 pos : SV_Position) : SV_Depth {
    uint2 p = uint2(pos.xy);
    if (p.x < w && p.y < h)
        discard;
    return depth;
}
)";

struct DepthFill {
    bool failed = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11DepthStencilState* state = nullptr;

    static ID3DBlob* compile(const char* src, size_t len, const char* target) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(src, len, "canary_depth", nullptr, nullptr, "main", target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            logf("canary: depth %s compile failed: %s", target,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            code = nullptr;
        }
        if (errors)
            errors->Release();
        return code;
    }

    bool ensure(ID3D11Device* device) {
        if (failed)
            return false;
        if (ps)
            return true;
        ID3DBlob* vc = compile(kDepthVS, sizeof(kDepthVS) - 1, "vs_5_0");
        ID3DBlob* pc = compile(kDepthPS, sizeof(kDepthPS) - 1, "ps_5_0");
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        failed = !vc || !pc ||
                 FAILED(device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs)) ||
                 FAILED(device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps)) ||
                 FAILED(device->CreateBuffer(&bd, nullptr, &params)) ||
                 FAILED(device->CreateDepthStencilState(&dd, &state));
        if (vc)
            vc->Release();
        if (pc)
            pc->Release();
        return !failed;
    }
};

static DepthFill g_depth;

// Clamp fill: like screen_size_fix's EdgePad, for any colour format and mip. The last valid column / row are copied
// to 1 x H / W x 1 temps (native size, one pair per format), then a full-mip triangle replicates them outside the
// sub-rect, scissored to a band beyond the edge.
static const char kClampPS[] = R"(
Texture2D<float4> col : register(t0);  // column w - 1
Texture2D<float4> row : register(t1);  // row h - 1
cbuffer Params : register(b0) { uint w; uint h; uint top; uint pad; };
float4 main(float4 pos : SV_Position) : SV_Target {
    uint2 p = uint2(pos.xy);
    if (p.x < w && p.y >= top && p.y < top + h)
        discard;
    if (p.y >= top && p.y < top + h)
        return col.Load(int3(0, p.y - top, 0));
    return row.Load(int3(min(p.x, w - 1), 0, 0));
}
)";

struct ClampFill {
    struct Temp {
        ID3D11Texture2D* col = nullptr;
        ID3D11Texture2D* row = nullptr;
        ID3D11ShaderResourceView* srv[2] = {};
    };
    bool failed = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11RasterizerState* scissor = nullptr;
    // (texture format, view format, w, h) -> temps; nullptr pair: failed.
    std::map<std::tuple<DXGI_FORMAT, DXGI_FORMAT, UINT, UINT>, Temp> temps;

    bool ensure(ID3D11Device* device) {
        if (failed)
            return false;
        if (ps)
            return true;
        ID3DBlob* vc = DepthFill::compile(kDepthVS, sizeof(kDepthVS) - 1, "vs_5_0");
        ID3DBlob* pc = DepthFill::compile(kClampPS, sizeof(kClampPS) - 1, "ps_5_0");
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        failed = !vc || !pc ||
                 FAILED(device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs)) ||
                 FAILED(device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps)) ||
                 FAILED(device->CreateBuffer(&bd, nullptr, &params)) ||
                 FAILED(device->CreateRasterizerState(&rd, &scissor));
        if (vc)
            vc->Release();
        if (pc)
            pc->Release();
        if (failed)
            logf("canary: clamp setup failed");
        return !failed;
    }

    Temp* temp(ID3D11Device* device, DXGI_FORMAT format, DXGI_FORMAT view, UINT w, UINT h) {
        auto k = std::make_tuple(format, view, w, h);
        auto it = temps.find(k);
        if (it != temps.end())
            return it->second.col ? &it->second : nullptr;
        Temp& t = temps[k];
        D3D11_TEXTURE2D_DESC d = {1, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        bool ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t.col));
        d.Width = w, d.Height = 1;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t.row)) &&
             SUCCEEDED(device->CreateShaderResourceView(t.col, &sd, &t.srv[0])) &&
             SUCCEEDED(device->CreateShaderResourceView(t.row, &sd, &t.srv[1]));
        if (!ok) {
            logf("canary: clamp temps (format %d, view %d, %ux%u) failed", format, view, w, h);
            for (ID3D11ShaderResourceView*& v : t.srv)
                if (v)
                    v->Release(), v = nullptr;
            if (t.row)
                t.row->Release(), t.row = nullptr;
            if (t.col)
                t.col->Release(), t.col = nullptr;
            return nullptr;
        }
        return &t;
    }

    bool run(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11Resource* res, const D3D11_TEXTURE2D_DESC& td,
             DXGI_FORMAT view, ID3D11RenderTargetView* rtv, UINT mip, UINT mw, UINT mh, UINT sw, UINT sh,
             DrawFn draw, bool bottom) {
        if (sw == 0 || sh == 0 || !ensure(device))
            return false;
        Temp* t = temp(device, td.Format, view, UINT(g_native.x), UINT(g_native.y));
        if (!t)
            return false;
        const UINT sub = D3D11CalcSubresource(mip, 0, td.MipLevels);
        const UINT top = bottom ? mh - sh : 0;
        const UINT edge = bottom ? top : sh - 1;
        D3D11_BOX col = {sw - 1, top, 0, sw, top + sh, 1}, row = {0, edge, 0, sw, edge + 1, 1};
        ctx->CopySubresourceRegion(t->col, 0, 0, 0, 0, res, sub, &col);
        ctx->CopySubresourceRegion(t->row, 0, 0, 0, 0, res, sub, &row);
        const UINT p[4] = {sw, sh, top, 0};
        ctx->UpdateSubresource(params, 0, nullptr, p, 0, 0);
        const float band = g_settings.clamp_band > 0 ? g_settings.clamp_band : 1.0f;
        const UINT bx = std::min(mw, sw + 4 + UINT(ceilf(float(mw) * band)));
        const UINT by = std::min(mh, sh + 4 + UINT(ceilf(float(mh) * band)));
        // One viewport uses scissor slot0 only. Two rectangles leave the bottom
        // strip unwritten; the pixel shader already discards the valid sub-rect.
        const D3D11_RECT rect = {0, bottom ? LONG(mh-by) : 0, LONG(bx), bottom ? LONG(mh) : LONG(by)};
        PaddingStateBackup backup(ctx, device);
        backup.unbind_inputs();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(scissor);
        D3D11_VIEWPORT vp = {0, 0, float(mw), float(mh), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetScissorRects(1, &rect);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 2, t->srv);
        ctx->PSSetConstantBuffers(0, 1, &params);
        draw(ctx, 3, 0);
        return true;
    }
};

static ClampFill g_clamp;

// Integer formats: Load as float4 would not round-trip, and nothing filters them.
static bool clampable(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
        return false;
    default:
        return true;
    }
}

static ID3D11View* view_for(ID3D11Device* device, ID3D11Resource* res, DXGI_FORMAT format, UINT mip, bool depth) {
    if (g_views.size() > 256)
        flush_views();
    auto k = std::make_tuple(static_cast<void*>(res), mip, format);
    auto it = g_views.find(k);
    if (it != g_views.end())
        return it->second;
    ID3D11View* view = nullptr;
    if (depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d = {};
        d.Format = dsv_format(format);
        d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mip;
        ID3D11DepthStencilView* dsv = nullptr;
        if (d.Format != DXGI_FORMAT_UNKNOWN && SUCCEEDED(device->CreateDepthStencilView(res, &d, &dsv)))
            view = dsv;
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC d = {};
        d.Format = format;
        d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = mip;
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(device->CreateRenderTargetView(res, &d, &rtv)))
            view = rtv;
    }
    g_views[k] = view;
    return view;
}

static ID3D11DeviceContext1* ctx1(ID3D11DeviceContext* ctx, ID3D11Device* device) {
    if (g_ctx1_for == ctx)
        return g_ctx1;
    if (g_ctx1)
        g_ctx1->Release();
    g_ctx1 = nullptr;
    g_ctx1_for = ctx;
    D3D11_FEATURE_DATA_D3D11_OPTIONS opts = {};
    bool supported = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof(opts))) &&
                     opts.ClearView;
    if (supported)
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));
    g_status.clear_view = g_ctx1 != nullptr;
    logf("canary: ClearView %s", g_ctx1 ? "available" : "NOT available, colour targets skipped");
    return g_ctx1;
}

// Fills mip `mip` (mw x mh) of res outside its sub-rect (sw x sh). False if it could not.
static bool fill(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11Resource* res, const D3D11_TEXTURE2D_DESC& td,
                 DXGI_FORMAT format, UINT mip, UINT mw, UINT mh, UINT sw, UINT sh, DrawFn draw, bool bottom) {
    const Fill phase = g_status.phase;
    const bool depth = (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    if (!depth && !(td.BindFlags & D3D11_BIND_RENDER_TARGET))
        return false;
    ID3D11View* view = view_for(device, res, format, mip, depth);
    if (!view)
        return false;
    if (g_status.clamping) {
        if (depth || !clampable(format))
            return false;
        return g_clamp.run(ctx, device, res, td, format, static_cast<ID3D11RenderTargetView*>(view), mip, mw, mh, sw,
                           sh, draw, bottom);
    }
    if (!depth) {
        ID3D11DeviceContext1* c1 = ctx1(ctx, device);
        if (!c1)
            return false;
        D3D11_RECT rects[2];
        UINT n = 0;
        if (sw < mw)
            rects[n++] = {LONG(sw), 0, LONG(mw), LONG(mh)};
        if (sh < mh)
            rects[n++] = bottom ? D3D11_RECT{0, 0, LONG(sw < mw ? sw : mw), LONG(mh-sh)}
                               : D3D11_RECT{0, LONG(sh), LONG(sw < mw ? sw : mw), LONG(mh)};
        float colour[4];
        colour_value(phase, format, colour);
        c1->ClearView(view, colour, rects, n);
        return true;
    }
    if (bottom) return false;  // bottom layouts are registered for colour targets only
    if (!g_depth.ensure(device))
        return false;
    // Near plane for Mixed / NaN, far for Inverse, in the engine's depth convention.
    const bool reverse = taa_dlss::status().reverse_depth > 0;
    const bool near_plane = phase != Fill::Inverse;
    const float value = near_plane == reverse ? 1.0f : 0.0f;
    struct {
        UINT w, h;
        float depth, pad;
    } p = {sw, sh, value, 0};
    ctx->UpdateSubresource(g_depth.params, 0, nullptr, &p, 0, 0);
    {
        StateBackup backup(ctx);
        // The texture is bound as an input: unbind the inputs before binding it for writing.
        ID3D11ShaderResourceView* null_srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        ctx->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, null_srv);
        ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, null_srv);
        ctx->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, null_srv);
        ctx->OMSetRenderTargets(0, nullptr, static_cast<ID3D11DepthStencilView*>(view));
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(g_depth.state, 0);
        ctx->RSSetState(nullptr);
        D3D11_VIEWPORT vp = {0, 0, float(mw), float(mh), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_depth.vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g_depth.ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_depth.params);
        draw(ctx, 3, 0);
    }
    ++g_frame.depth_fills;
    return true;
}

static void fill_inputs(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* srvs, UINT count, DrawFn draw) {
    game::Vec2 d = game::current_downscale();
    if (!(d.x < 0.999f || d.y < 0.999f) || g_native.x <= 0 || g_native.y <= 0)
        return;
    ID3D11Device* device = nullptr;
    for (UINT i = 0; i < count; ++i) {
        ID3D11ShaderResourceView* srv = srvs[i];
        if (!srv)
            continue;
        auto [it, inserted] = g_inputs.try_emplace(srv);
        Input& input = it->second;
        if (inserted) {
            input.view = srv;
            srv->GetResource(&input.resource);
            D3D11_RESOURCE_DIMENSION dim;
            input.resource->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                static_cast<ID3D11Texture2D*>(input.resource.Get())->GetDesc(&input.texture);
            const auto& td = input.texture;
            input.screen = td.Height > 0 && td.Width <= UINT(g_native.x) &&
                           fabsf(float(td.Width) / float(td.Height) / (g_native.x / g_native.y) - 1.0f) < 0.01f;
            if (input.screen)
                srv->GetDesc(&input.desc);
        }
        if (!input.screen)
            continue;
        ID3D11Resource* res = input.resource.Get();
        if (std::find(g_full_layout.begin(), g_full_layout.end(), res) != g_full_layout.end())
            continue;
        const auto& td = input.texture;
        const auto& vd = input.desc;
        const auto layout = std::find_if(g_bottom_targets.begin(), g_bottom_targets.end(),
                                        [res](const auto& t) { return t.resource == res; });
        const bool bottom = layout != g_bottom_targets.end();
        if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || td.SampleDesc.Count > 1) {
            if (g_filled.insert(key(res, 15)).second)
                ++g_frame.skipped;
            continue;
        }
        if (!device)
            ctx->GetDevice(&device);
        const UINT first = vd.Texture2D.MostDetailedMip;
        const UINT levels = vd.Texture2D.MipLevels == UINT(-1) ? td.MipLevels - first : vd.Texture2D.MipLevels;
        for (UINT m = first; m < first + levels && m < td.MipLevels && m < 15; ++m) {
            const UINT mw = td.Width >> m ? td.Width >> m : 1, mh = td.Height >> m ? td.Height >> m : 1;
            // Canary: round the sub-rect up, missing a column is better than filling a valid one. Clamp: round down,
            // the engine's viewports are whole pixels (426 x 0.33125 = 141.1 -> 141) and column 141 would be stale.
            const float bias = g_status.clamping ? 0.05f : -0.01f;
            const UINT sw = bottom ? std::max(1u, layout->width >> m) : UINT(g_status.clamping ? floorf(float(mw) * d.x + bias) : ceilf(float(mw) * d.x + bias));
            const UINT sh = bottom ? std::max(1u, layout->height >> m) : UINT(g_status.clamping ? floorf(float(mh) * d.y + bias) : ceilf(float(mh) * d.y + bias));
            if ((sw >= mw && sh >= mh) || !g_filled.insert(key(res, m)).second)
                continue;
            if (!fill(ctx, device, res, td, vd.Format, m, mw, mh, sw, sh, draw, bottom)) {
                ++g_frame.skipped;
                continue;
            }
            ++g_frame.fills;
            if (g_textures.insert(res).second)
                ++g_frame.textures;
            if (g_logged.size() < 128 && g_logged.insert({td.Width, td.Height, vd.Format, m}).second)
                logf("canary: filling %ux%u format %d mip %u (sub-rect %ux%u)%s", td.Width, td.Height, vd.Format, m,
                     sw, sh, td.BindFlags & D3D11_BIND_DEPTH_STENCIL ? " depth" : "");
        }
    }
    if (device)
        device->Release();
}

void before_draw(ID3D11DeviceContext* ctx, DrawFn draw, uint32_t vs_slots, uint32_t ps_slots) {
    invalidate_outputs(ctx, false);
    if (!on() || !g_armed || !g_status.active)
        return;
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    constexpr UINT kSlots = 32;  // the engine binds far fewer
    const UINT nv = resource_slots::count(vs_slots), np = resource_slots::count(ps_slots);
    if (nv) ctx->VSGetShaderResources(0, nv, srvs);
    if (np) ctx->PSGetShaderResources(0, np, srvs + kSlots);
    // Release unused bindings before filtering; never mutate the actual context.
    for (UINT i = 0; i < kSlots; ++i) {
        if (!(vs_slots & (uint32_t(1) << i)) && srvs[i]) {
            srvs[i]->Release(); srvs[i] = nullptr;
        }
        if (!(ps_slots & (uint32_t(1) << i)) && srvs[i + kSlots]) {
            srvs[i + kSlots]->Release(); srvs[i + kSlots] = nullptr;
        }
    }
    fill_inputs(ctx, srvs, 2 * kSlots, draw);
    for (UINT i = 0; i < 2 * kSlots; ++i)
        if (srvs[i])
            srvs[i]->Release();
}

void before_dispatch(ID3D11DeviceContext* ctx, DrawFn draw, uint32_t cs_slots) {
    invalidate_outputs(ctx, true);
    if (!on() || !g_armed || !g_status.active)
        return;
    constexpr UINT kSlots = 32;
    ID3D11ShaderResourceView* srvs[kSlots] = {};
    const UINT n = resource_slots::count(cs_slots);
    if (n) ctx->CSGetShaderResources(0, n, srvs);
    for (UINT i = 0; i < n; ++i)
        if (!(cs_slots & (uint32_t(1) << i)) && srvs[i]) {
            srvs[i]->Release(); srvs[i] = nullptr;
        }
    fill_inputs(ctx, srvs, kSlots, draw);
    for (ID3D11ShaderResourceView* s : srvs)
        if (s)
            s->Release();
}

static void dirty(ID3D11View* view, UINT mip) {
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res)
        return;
    g_filled.erase(key(res, mip));
    forget_full_layout(res);
    forget_bottom_target(res);
    res->Release();
}

void on_set_targets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!on() || (g_filled.empty() && g_full_layout.empty() && g_bottom_targets.empty()))
        return;
    for (UINT i = 0; rtvs && i < n && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtvs[i])
            continue;
        D3D11_RENDER_TARGET_VIEW_DESC d;
        rtvs[i]->GetDesc(&d);
        if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
            dirty(rtvs[i], d.Texture2D.MipSlice);
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d;
        dsv->GetDesc(&d);
        // A read-only depth view cannot change the texture.
        if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && !(d.Flags & D3D11_DSV_READ_ONLY_DEPTH))
            dirty(dsv, d.Texture2D.MipSlice);
    }
}

void on_set_uavs(UINT n, ID3D11UnorderedAccessView* const* uavs) {
    if (!on() || (g_filled.empty() && g_full_layout.empty() && g_bottom_targets.empty()))
        return;
    for (UINT i = 0; uavs && i < n; ++i) {
        if (!uavs[i])
            continue;
        D3D11_UNORDERED_ACCESS_VIEW_DESC d;
        uavs[i]->GetDesc(&d);
        if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D)
            dirty(uavs[i], d.Texture2D.MipSlice);
    }
}

void after_dispatch(ID3D11DeviceContext* ctx) {
    if (!on() || (g_filled.empty() && g_full_layout.empty() && g_bottom_targets.empty()))
        return;
    constexpr UINT kSlots = 8;
    ID3D11UnorderedAccessView* uavs[kSlots] = {};
    ctx->CSGetUnorderedAccessViews(0, kSlots, uavs);
    on_set_uavs(kSlots, uavs);
    for (ID3D11UnorderedAccessView* u : uavs)
        if (u)
            u->Release();
}

}  // namespace kc::canary
