#include "renderer/subrect_fixes.h"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "renderer/dxbc_patch.h"
#include "renderer/caustics_fix.h"
#include "renderer/taa_dlss.h"

namespace kc::subrect_fixes {
using Microsoft::WRL::ComPtr;
namespace {
std::mutex g_mutex;
struct Entry {
    Kind kind;
    ComPtr<ID3D11PixelShader> original, replacement;
};
std::map<ID3D11PixelShader*, Entry> g_ps;
std::map<ID3D11VertexShader*, ComPtr<ID3D11VertexShader>> g_bloom_vs;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11Buffer> g_params;
// Process-lifetime GPU cache: do not call D3D Release from DLL detach under the loader lock.
CausticsDepthPatch* g_caustics = nullptr;

bool hash_is(const void* code, SIZE_T size, const char* hash) {
    const auto* p = static_cast<const unsigned char*>(code);
    if (!p || size < 32 || std::memcmp(p, "DXBC", 4))
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i)
        if (hex[p[4 + i] >> 4] != hash[i * 2] || hex[p[4 + i] & 15] != hash[i * 2 + 1])
            return false;
    return true;
}
// Match the reflection producer by used reflection bindings, independent of material permutations.
bool water_reflection(const void* code, SIZE_T size) {
    const auto* bytes = static_cast<const char*>(code);
    constexpr char name[] = "ReflSamplerPrev";
    bool candidate = false;
    for (SIZE_T i = 0; i + sizeof(name) <= size; ++i)
        if (!std::memcmp(bytes+i, name, sizeof(name))) { candidate = true; break; }
    if (!candidate) return false;
    ComPtr<ID3D11ShaderReflection> r;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(r.GetAddressOf())))) return false;
    D3D11_SHADER_DESC desc{}; r->GetDesc(&desc);
    // CryEngine retains identical names for the sampler and texture entries.
    for (const char* texture : {"ReflSamplerPrev", "sceneDepthSamplerScaled"}) {
        bool found = false;
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC d{};
            if (SUCCEEDED(r->GetResourceBindingDesc(i, &d)) && d.Type == D3D_SIT_TEXTURE &&
                d.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && !std::strcmp(d.Name, texture)) found = true;
        }
        if (!found) return false;
    }
    for (UINT i = 0; i < desc.ConstantBuffers; ++i) {
        auto* var = r->GetConstantBufferByIndex(i)->GetVariableByName("mViewProjPrev");
        D3D11_SHADER_VARIABLE_DESC d{};
        if (SUCCEEDED(var->GetDesc(&d)) && (d.uFlags & D3D_SVF_USED)) return true;
    }
    return false;
}
constexpr char kStencil[] = R"(
Texture2D<uint2> StencilTex : register(t4);
float4 main(float4 p : SV_Position) : SV_Target {
    int stencil = int(StencilTex.Load(int3(int2(p.xy), 0)).y) & 127;
    return float(max(stencil - 64, 1)) / 255.0;
})";
} // namespace

void on_create_ps(ID3D11Device* device, const void* code, SIZE_T size, ID3D11PixelShader* shader) {
    Kind kind = Kind::None;
    if (hash_is(code, size, "999180901f1932b132f6eef394ee28f2"))
        kind = Kind::Stencil;
    else if (hash_is(code, size, "5e3a6ad3509e433d1d3c8db283922c29") ||
             hash_is(code, size, "e108b18fdae5607bc3144b28e6d0c1f4"))
        kind = Kind::Contact;
    else if (hash_is(code, size, "605729c33fe3e25d2ab7d10740ac8bab") ||
             hash_is(code, size, "84962ebe5751261dcade7dc5f97345dc"))
        kind = Kind::Bloom;
    else if (hash_is(code, size, "4006e5a4dbb3eca402502c4ae3cdf9e7"))
        kind = Kind::Caustics;
    else if (hash_is(code, size, "7f6e903383ed45db5fe3d976914710a2"))
        kind = Kind::HairOpaqueContact;
    else if (hash_is(code, size, "c19c94272450fac6642d7abfde00d1b3"))
        kind = Kind::HairContact;
    if (kind == Kind::None && water_reflection(code, size))
        kind = Kind::WaterReflection;
    if (kind == Kind::None)
        return;
    Entry entry{};
    entry.kind = kind;
    entry.original = shader;
    HRESULT hr = S_OK;
    if (kind == Kind::Stencil) {
        ComPtr<ID3DBlob> blob, error;
        hr = D3DCompile(kStencil, sizeof(kStencil) - 1, "subrect_stencil", nullptr, nullptr, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
        if (SUCCEEDED(hr))
            hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                           &entry.replacement);
    } else if (kind == Kind::Contact) {
        std::vector<uint8_t> patched;
        if (!dxbc::patch_contact_depth_uv(code, size, patched))
            hr = E_FAIL;
        else
            hr = device->CreatePixelShader(patched.data(), patched.size(), nullptr, &entry.replacement);
    }
    if (kind == Kind::HairOpaqueContact || kind == Kind::HairContact) {
        std::vector<uint8_t> patched;
        if (!dxbc::patch_forward_depth_origins(code, size, patched))
            hr = E_FAIL;
        else
            hr = device->CreatePixelShader(patched.data(), patched.size(), nullptr, &entry.replacement);
    }
    if (FAILED(hr)) {
        logf("subrect: shader correction setup failed 0x%08lx", hr);
        return;
    }
    std::lock_guard lock(g_mutex);
    g_ps.emplace(shader, std::move(entry));
}

void on_create_vs(const void* code, SIZE_T size, ID3D11VertexShader* shader) {
    if (!hash_is(code, size, "6e7f9495da6310c380b5dfb8a01f1f8b"))
        return;
    std::lock_guard lock(g_mutex);
    g_bloom_vs.emplace(shader, shader);
}
Shader find_ps(ID3D11PixelShader* shader) {
    std::lock_guard lock(g_mutex);
    auto it = g_ps.find(shader);
    return it == g_ps.end() ? Shader{} : Shader{it->second.kind, it->second.replacement.Get()};
}
bool is_bloom_vs(ID3D11VertexShader* shader) {
    std::lock_guard lock(g_mutex);
    return g_bloom_vs.find(shader) != g_bloom_vs.end();
}

DrawState before_draw(ID3D11DeviceContext* ctx, Shader shader, ID3D11PixelShader* original) {
    DrawState old;
    const auto& settings = taa_dlss::settings();
    if (!original || settings.mode != taa_dlss::Mode::Dlss ||
        settings.render_scale >= .999f)
        return old;
    if (shader.kind == Kind::Caustics) {
        if (!g_caustics)
            g_caustics = new CausticsDepthPatch;
        old.params = g_caustics->before_draw(ctx);
        old.caustics = old.params != nullptr;
        return old;
    }
    if (!shader.replacement)
        return old;
    const bool hair = shader.kind == Kind::HairOpaqueContact || shader.kind == Kind::HairContact;
    if (shader.kind == Kind::Contact || hair) {
        ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(hair ? (shader.kind == Kind::HairOpaqueContact ? 4 : 6) : 0, 1, &srv);
        if (!srv)
            return old;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        srv->GetDesc(&vd);
        if (vd.Format != DXGI_FORMAT_R32_FLOAT || vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            vd.Texture2D.MostDetailedMip != 0)
            return old;
        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            return old;
        D3D11_TEXTURE2D_DESC td;
        static_cast<ID3D11Texture2D*>(res.Get())->GetDesc(&td);
        D3D11_VIEWPORT vp{};
        UINT n = 1;
        ctx->RSGetViewports(&n, &vp);
        if (n != 1 || td.SampleDesc.Count != 1 || vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width <= 0 ||
            vp.Height <= 0 || vp.Width > td.Width || vp.Height > td.Height)
            return old;
        if (vp.Width >= td.Width * .999f && vp.Height >= td.Height * .999f)
            return old;
        float params[4] = {hair ? float(td.Width) : vp.Width / td.Width,
                           hair ? float(td.Height) : vp.Height / td.Height, 0, 0};
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        if (g_device.Get() != device.Get()) {
            g_params.Reset();
            g_device = device;
        }
        if (!g_params) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &g_params)))
                return old;
        }
        ctx->UpdateSubresource(g_params.Get(), 0, nullptr, params, 0, 0);
        ctx->PSGetConstantBuffers(13, 1, &old.params);
        ctx->PSSetConstantBuffers(13, 1, g_params.GetAddressOf());
        old.contact = true;
    }
    original->AddRef();
    old.original = original;
    ctx->PSSetShader(shader.replacement, nullptr, 0);
    return old;
}
void after_draw(ID3D11DeviceContext* ctx, DrawState& old) {
    if (old.caustics) {
        CausticsDepthPatch::after_draw(ctx, old.params);
        old = {};
        return;
    }
    if (!old.original)
        return;
    ctx->PSSetShader(old.original, nullptr, 0);
    old.original->Release();
    if (old.contact) {
        ctx->PSSetConstantBuffers(13, 1, &old.params);
        if (old.params)
            old.params->Release();
    }
    old = {};
}
} // namespace kc::subrect_fixes
