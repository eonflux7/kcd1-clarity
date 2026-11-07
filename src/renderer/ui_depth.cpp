#include "renderer/ui_depth.h"
#include "core/config.h"
#include "core/log.h"
#include "renderer/frame_dump.h"
#include "renderer/state_backup.h"
#include <atomic>
#include <cstring>
#include <d3dcompiler.h>
#include <map>
#include <mutex>
#include <wrl/client.h>

namespace kc::ui_depth {
using Microsoft::WRL::ComPtr;
namespace {
std::mutex g_mutex;
// Lifetime-bounded by the few matching UI variants; no D3D releases under DLL detach's loader lock.
auto &g_ui = *new std::map<ID3D11PixelShader *, ComPtr<ID3D11PixelShader>>;
std::atomic_uint64_t g_eligible{0}, g_corrected{0};
UINT g_rw = 0, g_rh = 0, g_w = 0, g_h = 0;
uint64_t g_serial = 0, g_done = 0;
uintptr_t g_source = 0;
bool g_ready = false;
ULONGLONG g_time = 0;

struct Resources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> depth_ps, export_ps;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11DepthStencilState> write_depth, no_depth;
    ComPtr<ID3D11BlendState> opaque;
    ComPtr<ID3D11Texture2D> copy, corrected, export_texture;
    ComPtr<ID3D11DepthStencilView> corrected_write, corrected_read[4];
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> export_rtv;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
auto &g = *new Resources;

constexpr char kVS[] = R"(
float4 main(uint id:SV_VertexID):SV_Position {
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0,1);
})";
constexpr char kDepth[] = R"(
Texture2D<float> Source:register(t0);
cbuffer Params:register(b0) {float2 Scale;uint2 Size;};
float main(float4 pos:SV_Position):SV_Depth {
    uint2 p=min(uint2(pos.xy*Scale),Size-1);
    return Source.Load(int3(p,0));
})";
constexpr char kExport[] = R"(
Texture2D<float> Source:register(t0);
float main(float4 pos:SV_Position):SV_Target {return Source.Load(int3(int2(pos.xy),0));}
)";

bool setup(ID3D11Device *device, const D3D11_TEXTURE2D_DESC &source) {
    if (g.device.Get() != device) {
        g = Resources{};
        g.device = device;
    }
    HRESULT hr = S_OK;
    if (!g.depth_ps) {
        ComPtr<ID3DBlob> vs, ps, out, error;
        hr = D3DCompile(kVS, sizeof(kVS) - 1, "ui_depth_vs", nullptr, nullptr, "main", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &error);
        if (SUCCEEDED(hr))
            hr = D3DCompile(kDepth, sizeof(kDepth) - 1, "ui_depth_ps", nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &error);
        if (SUCCEEDED(hr))
            hr = D3DCompile(kExport, sizeof(kExport) - 1, "ui_depth_export", nullptr, nullptr, "main",
                            "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &error);
        if (SUCCEEDED(hr))
            hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.vs);
        if (SUCCEEDED(hr))
            hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g.depth_ps);
        if (SUCCEEDED(hr))
            hr = device->CreatePixelShader(out->GetBufferPointer(), out->GetBufferSize(), nullptr,
                                           &g.export_ps);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(hr))
            hr = device->CreateBuffer(&bd, nullptr, &g.params);
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (SUCCEEDED(hr))
            hr = device->CreateDepthStencilState(&ds, &g.write_depth);
        ds.DepthEnable = FALSE;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (SUCCEEDED(hr))
            hr = device->CreateDepthStencilState(&ds, &g.no_depth);
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = 15;
        if (SUCCEEDED(hr))
            hr = device->CreateBlendState(&blend, &g.opaque);
        if (FAILED(hr)) {
            logf("ui_depth1: pipeline setup failed 0x%08lx %s", hr,
                 error ? static_cast<const char *>(error->GetBufferPointer()) : "");
            g = Resources{};
            return false;
        }
    }
    DXGI_FORMAT format, view, depth_format;
    switch (source.Format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        format = DXGI_FORMAT_R24G8_TYPELESS;
        view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depth_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        format = DXGI_FORMAT_R32G8X24_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        format = DXGI_FORMAT_R32_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT;
        depth_format = DXGI_FORMAT_D32_FLOAT;
        break;
    default:
        return false;
    }
    if (g.copy && g.corrected && g.corrected_write &&
        g.w == source.Width && g.h == source.Height && g.format == format)
        return true;
    g.copy.Reset();
    g.corrected.Reset();
    g.corrected_write.Reset();
    for (auto &read : g.corrected_read)
        read.Reset();
    g.srv.Reset();
    g.export_rtv.Reset();
    g.export_texture.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = source.Width;
    td.Height = source.Height;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = format;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device->CreateTexture2D(&td, nullptr, &g.copy);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = view;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(g.copy.Get(), &sv, &g.srv);
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&td, nullptr, &g.corrected);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = depth_format;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(hr))
        hr = device->CreateDepthStencilView(g.corrected.Get(), &dv, &g.corrected_write);
    if (FAILED(hr)) {
        g.copy.Reset();
        g.srv.Reset();
        logf("ui_depth1: depth snapshot setup failed 0x%08lx", hr);
        return false;
    }
    g.w = td.Width;
    g.h = td.Height;
    g.format = format;
    return true;
}

void snapshot(ID3D11DeviceContext *ctx, ID3D11Resource *depth) {
    ID3D11ShaderResourceView *null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->CopyResource(g.copy.Get(), depth);
}

void draw_state(ID3D11DeviceContext *ctx, ID3D11PixelShader *ps) {
    D3D11_VIEWPORT vp = {0, 0, float(g.w), float(g.h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    auto *srv = g.srv.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
}

void export_snapshot(ID3D11DeviceContext *ctx, const char *name) {
    if (!g.export_rtv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = g.w;
        d.Height = g.h;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT hr = g.device->CreateTexture2D(&d, nullptr, &g.export_texture);
        if (SUCCEEDED(hr))
            hr = g.device->CreateRenderTargetView(g.export_texture.Get(), nullptr, &g.export_rtv);
        if (FAILED(hr)) {
            logf("ui_depth1: export setup failed 0x%08lx", hr);
            return;
        }
    }
    auto *rtv = g.export_rtv.Get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetDepthStencilState(g.no_depth.Get(), 0);
    ctx->OMSetBlendState(g.opaque.Get(), nullptr, 0xffffffff);
    draw_state(ctx, g.export_ps.Get());
    ctx->Draw(3, 0);
    frame_dump::save(ctx, g.export_texture.Get(), name);
}
} // namespace

void on_create_ps(const void *code, SIZE_T size, ID3D11PixelShader *shader) {
    // Match the existing screen-size classifier's cheap bytecode prefilter.
    // Ordinary scene shaders do not need a second D3DReflect call for this fix.
    auto contains = [&](const char *name) {
        const SIZE_T length = strlen(name);
        const auto *bytes = static_cast<const char *>(code);
        for (SIZE_T i = 0; i + length <= size; ++i)
            if (bytes[i] == name[0] && memcmp(bytes + i, name, length) == 0)
                return true;
        return false;
    };
    if (!shader || !code || !contains("PER_BATCH") ||
        !contains("cBitmapColorTransform") || !contains("cPremultiplyAlpha"))
        return;
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code, size, IID_PPV_ARGS(&reflection))))
        return;
    D3D11_SHADER_DESC desc{};
    if (FAILED(reflection->GetDesc(&desc)) ||
        (desc.BoundResources != 1 && desc.BoundResources != 3))
        return;
    bool batch = false, texture = false, sampler = false;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding)) ||
            binding.BindPoint != 0 || binding.BindCount != 1)
            return;
        if (binding.Type == D3D_SIT_CBUFFER && !strcmp(binding.Name, "PER_BATCH"))
            batch = true;
        else if (binding.Type == D3D_SIT_TEXTURE && !strcmp(binding.Name, "texMap0") &&
                 binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D)
            texture = true;
        else if (binding.Type == D3D_SIT_SAMPLER)
            sampler = true;
        else
            return;
    }
    // Scaleform paints the menu with solid-colour AND bitmap passes. All must
    // depth-test against the same aligned silhouette; restoring the engine DSV
    // after each draw makes matching only the first, solid pass insufficient.
    if (!batch || texture != sampler || (desc.BoundResources == 3 && !texture))
        return;
    auto *buffer = reflection->GetConstantBufferByName("PER_BATCH");
    D3D11_SHADER_VARIABLE_DESC colour{}, premultiply{};
    if (FAILED(buffer->GetVariableByName("cBitmapColorTransform")->GetDesc(&colour)) ||
        FAILED(buffer->GetVariableByName("cPremultiplyAlpha")->GetDesc(&premultiply)) ||
        colour.StartOffset != 0 || premultiply.StartOffset != 32 || !(colour.uFlags & D3D_SVF_USED) ||
        !(premultiply.uFlags & D3D_SVF_USED))
        return;
    std::lock_guard lock(g_mutex);
    g_ui.emplace(shader, shader);
    logf("ui_depth3: Scaleform %s PS identified %p", texture ? "bitmap" : "solid-colour", shader);
}

void after_taa(UINT rw, UINT rh, UINT w, UINT h) {
    g_rw = rw;
    g_rh = rh;
    g_w = w;
    g_h = h;
    g_time = GetTickCount64();
    ++g_serial;
    g_source = 0;
    g_ready = false;
}
bool enabled() { return config().ui_depth_alignment; }
void set_enabled(bool value) {
    config().ui_depth_alignment = value;
    config_save();
    g_done = 0;
    g_ready = false;
    logf("ui_depth2: UI depth alignment %s (saved)", value ? "ON" : "OFF");
}
uint64_t eligible_frames() { return g_eligible.load(); }
uint64_t corrected_frames() { return g_corrected.load(); }

void before_draw(ID3D11DeviceContext *ctx, ID3D11PixelShader *shader, DrawState &state) {
    const bool capture = frame_dump::armed();
    if (!enabled() && !capture)
        return; // No shader lookup, lock, context query or allocation when off.
    if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        !g_serial || !g_rw || !g_rh || g_rw >= g_w || g_rh >= g_h ||
        GetTickCount64() - g_time > 500)
        return;
    {
        std::lock_guard lock(g_mutex);
        if (!g_ui.count(shader))
            return;
    }
    ComPtr<ID3D11DepthStencilState> ds;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds, &ref);
    if (!ds)
        return;
    D3D11_DEPTH_STENCIL_DESC dd{};
    ds->GetDesc(&dd);
    if (!dd.DepthEnable || dd.DepthFunc != D3D11_COMPARISON_GREATER_EQUAL || dd.StencilEnable ||
        dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO)
        return;
    ID3D11RenderTargetView *raw[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ComPtr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw, &dsv);
    ComPtr<ID3D11RenderTargetView> rtv;
    rtv.Attach(raw[0]);
    bool extra = false;
    for (UINT i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (raw[i]) {
            extra = true;
            raw[i]->Release();
        }
    }
    if (!dsv || !rtv || extra)
        return;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dsv->GetDesc(&dv);
    if (dv.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dv.Texture2D.MipSlice != 0)
        return;
    ComPtr<ID3D11Resource> depth_resource, colour_resource;
    dsv->GetResource(&depth_resource);
    rtv->GetResource(&colour_resource);
    ComPtr<ID3D11Texture2D> depth, colour;
    if (FAILED(depth_resource.As(&depth)) || FAILED(colour_resource.As(&colour)))
        return;
    D3D11_TEXTURE2D_DESC td{}, cd{};
    depth->GetDesc(&td);
    colour->GetDesc(&cd);
    UINT nvp = 1;
    D3D11_VIEWPORT vp{};
    ctx->RSGetViewports(&nvp, &vp);
    if (td.Width != g_w || td.Height != g_h || cd.Width != g_w || cd.Height != g_h ||
        td.SampleDesc.Count != 1 || cd.SampleDesc.Count != 1 || td.ArraySize != 1 || td.MipLevels != 1 ||
        vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width != float(g_w) || vp.Height != float(g_h))
        return;
    ComPtr<IUnknown> identity;
    if (FAILED(depth.As(&identity)))
        return;
    const uintptr_t source = reinterpret_cast<uintptr_t>(identity.Get());
    const auto bind_corrected = [&] {
        if (!enabled() || !g_ready || g_source != source || dv.Flags >= 4)
            return;
        auto &view = g.corrected_read[dv.Flags];
        if (!view && FAILED(g.device->CreateDepthStencilView(g.corrected.Get(), &dv, &view)))
            return;
        state.colour = rtv;
        state.depth = dsv;
        auto *target = rtv.Get();
        ctx->OMSetRenderTargets(1, &target, view.Get());
    };
    if (g_done == g_serial) {
        bind_corrected();
        return;
    }
    g_done = g_serial;
    g_source = source;
    ++g_eligible;
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("ui_depth2: eligible native Scaleform pass, scene %ux%u -> %ux%u, DSV fmt %u flags %u, depth "
             "func %u write %u; correction %s",
             g_rw, g_rh, g_w, g_h, td.Format, dv.Flags, dd.DepthFunc, dd.DepthWriteMask,
             enabled() ? "ON" : "OFF");
    }
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    if (!setup(device.Get(), td))
        return;
    {
        StateBackup backup(ctx);
        snapshot(ctx, depth.Get());
        if (capture)
            export_snapshot(ctx, "ui_depth_before");
        if (enabled()) {
            // Preserve the engine's resource, including every stencil bit. Only a
            // private full-size copy is resampled and bound for qualifying UI draws.
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->CopyResource(g.corrected.Get(), depth.Get());
            struct Params {
                float scale[2];
                UINT size[2];
            } params = {{float(g_rw) / g_w, float(g_rh) / g_h}, {g_rw, g_rh}};
            ctx->UpdateSubresource(g.params.Get(), 0, nullptr, &params, 0, 0);
            auto *cb = g.params.Get();
            ctx->PSSetConstantBuffers(0, 1, &cb);
            ctx->OMSetRenderTargets(0, nullptr, g.corrected_write.Get());
            ctx->OMSetDepthStencilState(g.write_depth.Get(), 0);
            draw_state(ctx, g.depth_ps.Get());
            ctx->Draw(3, 0);
            ++g_corrected;
            g_ready = true;
            static bool fixed_logged = false;
            if (!fixed_logged) {
                fixed_logged = true;
                logf("ui_depth: private menu depth resampled; engine depth, RGB and stencil unchanged");
            }
            if (capture) {
                snapshot(ctx, g.corrected.Get());
                export_snapshot(ctx, "ui_depth_after");
            }
        }
    } // Restore the original context before installing the draw-local DSV.
    bind_corrected();
}

void after_draw(ID3D11DeviceContext *ctx, const DrawState &state) {
    if (state.depth) {
        auto *target = state.colour.Get();
        ctx->OMSetRenderTargets(1, &target, state.depth.Get());
    }
}
} // namespace kc::ui_depth
