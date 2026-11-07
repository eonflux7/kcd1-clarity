#include "core/config.h"
#include "renderer/frame_dump.h"
#include "renderer/ui_depth.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace kc {
void logf(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    puts("");
    va_end(a);
}
namespace frame_dump {
bool capture = false;
unsigned saved = 0;
bool armed() { return capture; }
void save(ID3D11DeviceContext *, ID3D11Resource *, const char *) { ++saved; }
} // namespace frame_dump
} // namespace kc
static void check(bool ok, const char *msg) {
    if (!ok) {
        printf("FAIL %s\n", msg);
        exit(1);
    }
    printf("PASS %s\n", msg);
}
static std::vector<unsigned> read(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)), "staging");
    ctx->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    check(SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)), "readback");
    std::vector<unsigned> result(d.Width * d.Height);
    for (UINT y = 0; y < d.Height; ++y)
        memcpy(result.data() + y * d.Width, static_cast<char *>(m.pData) + y * m.RowPitch, d.Width * 4);
    ctx->Unmap(staging.Get(), 0);
    return result;
}
// Run the same before/after pairing as the draw hook, retaining the texture
// actually bound during that draw for offscreen validation afterwards.
static ComPtr<ID3D11Texture2D> correct(ID3D11DeviceContext *ctx, ID3D11PixelShader *shader,
                                     UINT *flags = nullptr) {
    kc::ui_depth::DrawState scope;
    kc::ui_depth::before_draw(ctx, shader, scope);
    ComPtr<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(0, nullptr, &dsv);
    ComPtr<ID3D11Resource> resource;
    dsv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    check(SUCCEEDED(resource.As(&texture)), "draw depth texture");
    if (flags) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
        dsv->GetDesc(&desc);
        *flags = desc.Flags;
    }
    kc::ui_depth::after_draw(ctx, scope);
    return texture;
}
int wmain(int argc, wchar_t **argv) {
    check(argc == 2, "test INI path supplied");
    const std::wstring ini = argv[1];
    kc::config_load(ini);
    kc::ui_depth::set_enabled(true);
    check(GetPrivateProfileIntW(L"dlss", L"ui_depth_alignment", 0, ini.c_str()) == 1,
          "enable persisted to INI");
    kc::config().ui_depth_alignment = false;
    kc::config_load(ini);
    check(kc::ui_depth::enabled(), "enable survives config reload");
    kc::ui_depth::set_enabled(false);
    check(GetPrivateProfileIntW(L"dlss", L"ui_depth_alignment", 1, ini.c_str()) == 0,
          "disable persisted to INI");
    kc::config().ui_depth_alignment = true;
    kc::config_load(ini);
    check(!kc::ui_depth::enabled(), "disable survives config reload");
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                      D3D11_SDK_VERSION, &dev, &level, &ctx)),
          "WARP device");
    const char ps_source[] =
        R"(cbuffer PER_BATCH:register(b0){row_major float2x4 cBitmapColorTransform;float cPremultiplyAlpha;};float4 main(float4 c:COLOR0):SV_Target{float4 t=c*cBitmapColorTransform[0]+cBitmapColorTransform[1];return cPremultiplyAlpha!=0?t*t.w:t;})";
    ComPtr<ID3DBlob> code, error;
    check(SUCCEEDED(D3DCompile(ps_source, sizeof(ps_source) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error)),
          "UI shader compile");
    ComPtr<ID3D11PixelShader> ps;
    check(SUCCEEDED(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps)),
          "UI shader create");
    kc::ui_depth::on_create_ps(code->GetBufferPointer(), code->GetBufferSize(), ps.Get());
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    const unsigned bg = 0x199999, foreground_depth = 0xcccccc, stencil = 37;
    std::vector<unsigned> initial(24, bg | (stencil << 24));
    initial[1] = foreground_depth | (stencil << 24);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 6;
    d.Height = 4;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R24G8_TYPELESS;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initial.data();
    data.SysMemPitch = 6 * 4;
    ComPtr<ID3D11Texture2D> depth;
    check(SUCCEEDED(dev->CreateTexture2D(&d, &data, &depth)), "depth initial data");
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    check(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), &dv, &dsv)), "DSV");
    std::vector<unsigned> colours(24, 0x7f356478);
    data.pSysMem = colours.data();
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour;
    check(SUCCEEDED(dev->CreateTexture2D(&d, &data, &colour)), "colour target");
    ComPtr<ID3D11RenderTargetView> rtv;
    check(SUCCEEDED(dev->CreateRenderTargetView(colour.Get(), nullptr, &rtv)), "RTV");
    auto *rt = rtv.Get();
    ctx->OMSetRenderTargets(1, &rt, dsv.Get());
    D3D11_VIEWPORT vp = {0, 0, 6, 4, 0, 1};
    ctx->RSSetViewports(1, &vp);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> state;
    check(SUCCEEDED(dev->CreateDepthStencilState(&ds, &state)), "UI depth state");
    ctx->OMSetDepthStencilState(state.Get(), 9);
    kc::ui_depth::set_enabled(false);
    kc::ui_depth::after_taa(3, 2, 6, 4);
    kc::ui_depth::DrawState disabled_scope;
    kc::ui_depth::before_draw(nullptr, ps.Get(), disabled_scope);
    check(!disabled_scope.depth, "OFF fast path performs no context access");
    correct(ctx.Get(), ps.Get());
    check(kc::ui_depth::eligible_frames() == 0, "OFF bypasses UI classification");
    check(read(dev.Get(), ctx.Get(), depth.Get()) == initial, "OFF leaves depth and stencil intact");
    kc::ui_depth::set_enabled(true);
    kc::ui_depth::after_taa(3, 2, 6, 4);
    auto aligned = correct(ctx.Get(), ps.Get());
    check(kc::ui_depth::corrected_frames() == 1, "one depth correction");
    check(aligned.Get() != depth.Get(), "private depth bound only during UI draw");
    auto got = read(dev.Get(), ctx.Get(), aligned.Get());
    check(read(dev.Get(), ctx.Get(), depth.Get()) == initial,
          "later readers see unchanged engine depth and stencil");
    bool values = true, masks = true;
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 6; ++x) {
            unsigned expected = (x / 2 == 1 && y / 2 == 0) ? foreground_depth : bg;
            values &= std::abs(int(got[y * 6 + x] & 0xffffff) - int(expected)) <= 1;
            masks &= (got[y * 6 + x] >> 24) == stencil;
        }
    check(values, "nearest 2x depth alignment at all pixels");
    check(masks, "all stencil bits preserved");
    check(read(dev.Get(), ctx.Get(), colour.Get()) == colours, "all RGBA pixels unchanged");
    ComPtr<ID3D11PixelShader> restored_ps;
    ctx->PSGetShader(&restored_ps, nullptr, nullptr);
    check(restored_ps.Get() == ps.Get(), "pixel shader restored");
    ComPtr<ID3D11DepthStencilState> restored_state;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&restored_state, &ref);
    check(restored_state.Get() == state.Get() && ref == 9, "depth state and reference restored");
    ID3D11RenderTargetView *current_rt = nullptr;
    ComPtr<ID3D11DepthStencilView> current_ds;
    ctx->OMGetRenderTargets(1, &current_rt, &current_ds);
    check(current_rt == rtv.Get() && current_ds.Get() == dsv.Get(), "render and depth targets restored");
    if (current_rt)
        current_rt->Release();
    auto repeated = correct(ctx.Get(), ps.Get());
    check(kc::ui_depth::corrected_frames() == 1 && repeated.Get() == aligned.Get() &&
              read(dev.Get(), ctx.Get(), repeated.Get()) == got,
          "same private depth reused without double-resampling");
    // Regression: the real menu alternates solid-colour and textured Scaleform
    // draws. A corrected solid pass followed by an uncorrected bitmap pass still
    // leaves the small portrait silhouette in the final menu.
    const char bitmap_source[] =
        R"(cbuffer PER_BATCH:register(b0){row_major float2x4 cBitmapColorTransform;float cPremultiplyAlpha;};Texture2D texMap0:register(t0);SamplerState bitmapSampler:register(s0);float4 main(float4 pos:SV_Position,float4 c:COLOR0):SV_Target{float4 t=texMap0.Sample(bitmapSampler,float2(0.5,0.5))*c*cBitmapColorTransform[0]+cBitmapColorTransform[1];return cPremultiplyAlpha!=0?t*t.w:t;})";
    ComPtr<ID3DBlob> bitmap_code;
    check(SUCCEEDED(D3DCompile(bitmap_source, sizeof(bitmap_source) - 1, nullptr, nullptr, nullptr,
                               "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bitmap_code, &error)),
          "bitmap UI shader compile");
    ComPtr<ID3D11PixelShader> bitmap_ps;
    check(SUCCEEDED(dev->CreatePixelShader(bitmap_code->GetBufferPointer(), bitmap_code->GetBufferSize(),
                                         nullptr, &bitmap_ps)), "bitmap UI shader create");
    kc::ui_depth::on_create_ps(bitmap_code->GetBufferPointer(), bitmap_code->GetBufferSize(), bitmap_ps.Get());
    ctx->PSSetShader(bitmap_ps.Get(), nullptr, 0);
    auto bitmap_depth = correct(ctx.Get(), bitmap_ps.Get());
    check(bitmap_depth.Get() == aligned.Get() && kc::ui_depth::corrected_frames() == 1,
          "bitmap UI draw reuses the solid pass aligned depth");
    const char menu_vs[] =
        R"(struct O{float4 p:SV_Position;float4 c:COLOR0;};O main(uint id:SV_VertexID){O o;float2 uv=float2((id<<1)&2,id&2);o.p=float4(uv*float2(2,-2)+float2(-1,1),0.5,1);o.c=1;return o;})";
    ComPtr<ID3DBlob> menu_vs_code;
    check(SUCCEEDED(D3DCompile(menu_vs, sizeof(menu_vs) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &menu_vs_code, &error)), "menu VS compile");
    ComPtr<ID3D11VertexShader> menu_vertex;
    check(SUCCEEDED(dev->CreateVertexShader(menu_vs_code->GetBufferPointer(), menu_vs_code->GetBufferSize(),
                                          nullptr, &menu_vertex)), "menu VS create");
    const float transform[12] = {1, 1, 1, 1};
    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(transform);
    cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cb_data{};
    cb_data.pSysMem = transform;
    ComPtr<ID3D11Buffer> menu_params;
    check(SUCCEEDED(dev->CreateBuffer(&cb_desc, &cb_data, &menu_params)), "bitmap UI parameters");
    D3D11_TEXTURE2D_DESC bitmap_desc{};
    bitmap_desc.Width = bitmap_desc.Height = bitmap_desc.MipLevels = bitmap_desc.ArraySize = 1;
    bitmap_desc.SampleDesc.Count = 1;
    bitmap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bitmap_desc.Usage = D3D11_USAGE_IMMUTABLE;
    bitmap_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const unsigned menu_green = 0xff00ff00;
    D3D11_SUBRESOURCE_DATA bitmap_data{};
    bitmap_data.pSysMem = &menu_green;
    bitmap_data.SysMemPitch = sizeof(menu_green);
    ComPtr<ID3D11Texture2D> bitmap_texture;
    check(SUCCEEDED(dev->CreateTexture2D(&bitmap_desc, &bitmap_data, &bitmap_texture)), "bitmap texture");
    ComPtr<ID3D11ShaderResourceView> bitmap_srv;
    check(SUCCEEDED(dev->CreateShaderResourceView(bitmap_texture.Get(), nullptr, &bitmap_srv)), "bitmap SRV");
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> bitmap_sampler;
    check(SUCCEEDED(dev->CreateSamplerState(&sampler_desc, &bitmap_sampler)), "bitmap sampler");
    auto *menu_cb = menu_params.Get();
    auto *menu_srv = bitmap_srv.Get();
    auto *menu_sampler = bitmap_sampler.Get();
    ctx->VSSetShader(menu_vertex.Get(), nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetConstantBuffers(0, 1, &menu_cb);
    ctx->PSSetShaderResources(0, 1, &menu_srv);
    ctx->PSSetSamplers(0, 1, &menu_sampler);
    kc::ui_depth::DrawState bitmap_scope;
    kc::ui_depth::before_draw(ctx.Get(), bitmap_ps.Get(), bitmap_scope);
    ctx->Draw(3, 0);
    kc::ui_depth::after_draw(ctx.Get(), bitmap_scope);
    auto bitmap_result = read(dev.Get(), ctx.Get(), colour.Get());
    bool composition_ok = true;
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 6; ++x)
            composition_ok &= bitmap_result[y * 6 + x] ==
                ((x / 2 == 1 && y / 2 == 0) ? colours[y * 6 + x] : menu_green);
    check(composition_ok, "textured menu preserves the enlarged silhouette, not the small one");
    check(read(dev.Get(), ctx.Get(), depth.Get()) == initial, "bitmap draw leaves engine depth unchanged");
    ctx->UpdateSubresource(colour.Get(), 0, nullptr, colours.data(), 6 * 4, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    ComPtr<ID3D11DepthStencilView> read_only;
    check(SUCCEEDED(dev->CreateDepthStencilView(depth.Get(), &dv, &read_only)), "read-only engine DSV");
    ctx->OMSetRenderTargets(1, &rt, read_only.Get());
    UINT private_flags = 0;
    auto readonly_aligned = correct(ctx.Get(), ps.Get(), &private_flags);
    check(private_flags == dv.Flags && readonly_aligned.Get() == aligned.Get(),
          "private DSV retains read-only depth and stencil flags");
    dv.Flags = 0;
    ctx->OMSetRenderTargets(1, &rt, dsv.Get());
    kc::ui_depth::after_taa(6, 4, 6, 4);
    auto native_depth = correct(ctx.Get(), ps.Get());
    check(kc::ui_depth::corrected_frames() == 1 && native_depth.Get() == depth.Get() &&
              read(dev.Get(), ctx.Get(), depth.Get()) == initial,
          "100 percent mode bypasses correction");
    for (UINT render_size : {8u, 7u, 6u, 4u}) {
        const UINT native = 12;
        std::vector<unsigned> input(native * native, bg | (stencil << 24));
        input[2 * native + 2] = foreground_depth | (stencil << 24);
        D3D11_TEXTURE2D_DESC test_desc{};
        test_desc.Width = test_desc.Height = native;
        test_desc.MipLevels = test_desc.ArraySize = test_desc.SampleDesc.Count = 1;
        test_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        test_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_SUBRESOURCE_DATA test_data{};
        test_data.pSysMem = input.data();
        test_data.SysMemPitch = native * 4;
        ComPtr<ID3D11Texture2D> test_depth;
        check(SUCCEEDED(dev->CreateTexture2D(&test_desc, &test_data, &test_depth)), "ratio depth create");
        ComPtr<ID3D11DepthStencilView> test_dsv;
        check(SUCCEEDED(dev->CreateDepthStencilView(test_depth.Get(), &dv, &test_dsv)), "ratio DSV create");
        test_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        test_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        std::vector<unsigned> test_colours(native * native, 0x7f356478);
        test_data.pSysMem = test_colours.data();
        ComPtr<ID3D11Texture2D> test_colour;
        check(SUCCEEDED(dev->CreateTexture2D(&test_desc, &test_data, &test_colour)), "ratio colour create");
        ComPtr<ID3D11RenderTargetView> test_rtv;
        check(SUCCEEDED(dev->CreateRenderTargetView(test_colour.Get(), nullptr, &test_rtv)),
              "ratio RTV create");
        auto *test_rt = test_rtv.Get();
        ctx->OMSetRenderTargets(1, &test_rt, test_dsv.Get());
        D3D11_VIEWPORT test_vp = {0, 0, float(native), float(native), 0, 1};
        ctx->RSSetViewports(1, &test_vp);
        kc::ui_depth::after_taa(render_size, render_size, native, native);
        auto test_aligned = correct(ctx.Get(), ps.Get());
        auto result = read(dev.Get(), ctx.Get(), test_aligned.Get());
        bool ratio_ok = true;
        for (UINT y = 0; y < native; ++y)
            for (UINT x = 0; x < native; ++x) {
                UINT sx = UINT((float(x) + 0.5f) * (float(render_size) / native));
                UINT sy = UINT((float(y) + 0.5f) * (float(render_size) / native));
                auto expected = input[sy * native + sx];
                auto actual = result[y * native + x];
                ratio_ok &= std::abs(int(actual & 0xffffff) - int(expected & 0xffffff)) <= 1 &&
                            (actual >> 24) == stencil;
            }
        char label[96]{};
        sprintf_s(label, "ratio %u/%u depth mapping and stencil protection", render_size, native);
        check(ratio_ok, label);
        check(read(dev.Get(), ctx.Get(), test_depth.Get()) == input, "ratio engine depth never overwritten");
        check(read(dev.Get(), ctx.Get(), test_colour.Get()) == test_colours, "ratio RGBA unchanged");
    }
    const auto corrected = kc::ui_depth::corrected_frames();
    kc::ui_depth::set_enabled(false);
    kc::frame_dump::capture = true;
    kc::ui_depth::after_taa(4, 4, 12, 12);
    auto captured = correct(ctx.Get(), ps.Get());
    auto duplicate_capture = correct(ctx.Get(), ps.Get());
    check(kc::frame_dump::saved == 1 && kc::ui_depth::corrected_frames() == corrected &&
              captured.Get() == duplicate_capture.Get(), "OFF with armed dump exports once without correction");
    kc::frame_dump::capture = false;
    return 0;
}
