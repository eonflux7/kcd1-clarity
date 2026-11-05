#include "renderer/screen_size_fix.h"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "game/renderer.h"
#include "renderer/state_backup.h"

namespace kc::screen_size_fix {

static Status g_status;

Status& status() { return g_status; }

static bool contains(const void* data, SIZE_T size, const char* s) {
    const size_t n = strlen(s);
    const auto* p = static_cast<const char*>(data);
    for (SIZE_T i = 0; i + n <= size; ++i)
        if (p[i] == s[0] && memcmp(p + i, s, n) == 0)
            return true;
    return false;
}

// Slot and offset of a used constant-buffer variable.
static bool find_var(ID3D11ShaderReflection* r, const char* name, UINT* slot, UINT* offset) {
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC cbd;
        cb->GetDesc(&cbd);
        for (UINT v = 0; v < cbd.Variables; ++v) {
            D3D11_SHADER_VARIABLE_DESC vd;
            cb->GetVariableByIndex(v)->GetDesc(&vd);
            if (strcmp(vd.Name, name) != 0 || !(vd.uFlags & D3D_SVF_USED))
                continue;
            D3D11_SHADER_INPUT_BIND_DESC b;
            if (FAILED(r->GetResourceBindingDescByName(cbd.Name, &b)))
                return false;
            *slot = b.BindPoint;
            *offset = vd.StartOffset;
            return true;
        }
    }
    return false;
}

bool reflect_ps(const void* code, SIZE_T size, Shader* out) {
    if (contains(code, size, "SampleLumOffsets0")) {
        out->kind = Shader::LumInitialPS;
        ++g_status.ps_seen;
        logf("screen_size_fix: luminance PS (HDRSampleLumInitial)");
        return true;
    }
    if (contains(code, size, "SvoTreeSettings0") && contains(code, size, "g_mViewProj")) {
        ID3D11ShaderReflection* r = nullptr;
        if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
            return false;
        bool ok = find_var(r, "g_mViewProj", &out->cb_slot, &out->offset) && out->offset % 16 == 0;
        r->Release();
        if (!ok)
            return false;
        out->kind = Shader::ViewProjPS;
        ++g_status.ps_seen;
        logf("screen_size_fix: SVO trace PS: g_mViewProj cb%u +%u", out->cb_slot, out->offset);
        return true;
    }
    if (contains(code, size, "SVO_ReprojectionMatrix")) {
        ID3D11ShaderReflection* r = nullptr;
        if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
            return false;
        bool ok = find_var(r, "SVO_ReprojectionMatrix", &out->cb_slot, &out->offset) && out->offset % 16 == 0;
        r->Release();
        if (ok) {  // else only declared (shared cbuffer): try the other kinds
            out->kind = Shader::ReprojPS;
            ++g_status.ps_seen;
            logf("screen_size_fix: SVO reprojection PS: SVO_ReprojectionMatrix cb%u +%u", out->cb_slot, out->offset);
            return true;
        }
    }
    if (contains(code, size, "ViewSpaceParams") &&
        (contains(code, size, "SSDOParams") || contains(code, size, "SSSBlurDir"))) {
        ID3D11ShaderReflection* r = nullptr;
        if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
            return false;
        UINT radius_slot = ~0u;
        bool ssdo = find_var(r, "SSDOParams", &radius_slot, &out->radius_offset);
        if (!ssdo && !find_var(r, "SSSBlurDir", &radius_slot, &out->radius_offset))
            radius_slot = ~0u;
        bool ok = find_var(r, "ViewSpaceParams", &out->cb_slot, &out->offset) && out->offset % 16 == 0;
        if (ssdo && !(find_var(r, "PS_ScreenSize", &out->size_slot, &out->size_offset) && out->size_offset % 16 == 0))
            out->size_slot = out->size_offset = ~0u;
        r->Release();
        if (ok) {  // else only declared: try the other kinds
            if (radius_slot != out->cb_slot || out->radius_offset % 16 != 0) {
                logf("screen_size_fix: view-space PS: radius not next to ViewSpaceParams, left alone");
                out->radius_offset = ~0u;
            }
            out->radius_xy_only = !ssdo;
            out->kind = Shader::ViewSpacePS;
            ++g_status.ps_seen;
            logf("screen_size_fix: view-space PS (%s): ViewSpaceParams cb%u +%u, radius +%d, PS_ScreenSize cb%d +%d",
                 ssdo ? "SSDO" : "SSS", out->cb_slot, out->offset,
                 out->radius_offset == ~0u ? -1 : int(out->radius_offset), int(out->size_slot), int(out->size_offset));
            return true;
        }
        out->radius_offset = ~0u;
        out->size_slot = out->size_offset = ~0u;
    }
    if (contains(code, size, "vDofParamsFocus") && !contains(code, size, "PS_NearFarClipDist")) {
        // Gather DOF passes other than the prep and the composite (those read PS_NearFarClipDist).
        ID3D11ShaderReflection* r = nullptr;
        if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
            return false;
        bool gather = find_var(r, "PS_ScreenSize", &out->cb_slot, &out->offset);
        r->Release();
        out->kind = gather ? Shader::DofGatherPS : Shader::DofPS;
        ++g_status.ps_seen;
        logf("screen_size_fix: DOF %s PS", gather ? "gather" : "tile");
        return true;
    }
    if (contains(code, size, "vDofParamsFocus0") && contains(code, size, "PS_NearFarClipDist") &&
        !contains(code, size, "PS_ScreenSize")) {
        out->kind = Shader::DofPS;  // gather DOF prep (near/far layers + CoC)
        ++g_status.ps_seen;
        logf("screen_size_fix: DOF prep PS");
        return true;
    }
    // Gather DOF composite (MotionBlur.cfx CompositeDofPS, the pause-menu blur): bicubic-samples the half-res
    // layers with size PS_ScreenSize.xy * 0.5, the scaled viewport instead of the texture. Same fix as the fog,
    // with its full-res depth (_tex0) giving the size. The other DOF passes have no vDofParamsFocus0 +
    // PS_NearFarClipDist + PS_ScreenSize combination (DofPS's CoC scale wants the scaled PS_ScreenSize).
    const bool dof = contains(code, size, "vDofParamsFocus0") && contains(code, size, "PS_NearFarClipDist");
    const char* size_tex = dof ? "_tex0" : "zMap";
    if (!contains(code, size, size_tex) || !contains(code, size, "PS_ScreenSize"))
        return false;
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return false;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b;
        r->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_TEXTURE && b.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && !strcmp(b.Name, size_tex))
            out->zmap_slot = b.BindPoint;
    }
    bool ok = find_var(r, "PS_ScreenSize", &out->cb_slot, &out->offset) && out->zmap_slot != ~0u;
    r->Release();
    if (!ok)
        return false;
    out->kind = Shader::FogPS;
    ++g_status.ps_seen;
    logf("screen_size_fix: %s PS: %s t%u, PS_ScreenSize cb%u +%u", dof ? "DOF composite" : "fog", size_tex,
         out->zmap_slot, out->cb_slot, out->offset);
    return true;
}

bool reflect_cs(const void* code, SIZE_T size, Shader* out) {
    if (!contains(code, size, "FrustumTL") || !contains(code, size, "ProjParams") ||
        !contains(code, size, "TiledLightsCullInfo"))
        return false;
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return false;
    const char* names[3] = {"FrustumTR", "FrustumBL", "ProjParams"};
    bool ok = find_var(r, "FrustumTL", &out->cb_slot, &out->offset) && out->offset % 16 == 0;
    for (int i = 0; ok && i < 3; ++i) {
        UINT slot = ~0u;
        ok = find_var(r, names[i], &slot, &out->tiled[i]) && slot == out->cb_slot && out->tiled[i] % 16 == 0;
    }
    r->Release();
    if (!ok) {
        logf("screen_size_fix: tiled shading CS without the expected constants, left alone");
        return false;
    }
    out->kind = Shader::TiledShadingCS;
    ++g_status.ps_seen;
    logf("screen_size_fix: tiled shading CS: FrustumTL cb%u +%u, TR +%u, BL +%u, ProjParams +%u", out->cb_slot,
         out->offset, out->tiled[0], out->tiled[1], out->tiled[2]);
    return true;
}

bool reflect_vs(const void* code, SIZE_T size, Shader* out) {
    if (contains(code, size, "vDofParamsFocus")) {  // TileMinCoCVS / DownscaleDofVS: texel offsets in the VS
        out->kind = Shader::DofVS;
        ++g_status.vs_seen;
        logf("screen_size_fix: DOF VS");
        return true;
    }
    const bool hpos = contains(code, size, "g_VS_HPosScale"), screen = contains(code, size, "g_ScreenSize");
    if (!hpos && !screen)
        return false;
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return false;
    // Input register of TEXCOORD0 (the CPU triangle's UVs).
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    int tc_reg = -1;
    for (UINT i = 0; i < sd.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p;
        r->GetInputParameterDesc(i, &p);
        if (!_stricmp(p.SemanticName, "TEXCOORD") && p.SemanticIndex == 0)
            tc_reg = static_cast<int>(p.Register);
    }
    UINT hpos_slot = ~0u, hpos_offset = ~0u, size_slot = ~0u, size_offset = ~0u;
    bool found_hpos = tc_reg >= 0 && hpos && find_var(r, "g_VS_HPosScale", &hpos_slot, &hpos_offset);
    bool found_size = tc_reg >= 0 && screen && find_var(r, "g_ScreenSize", &size_slot, &size_offset);
    r->Release();
    if (!found_hpos && !found_size)
        return false;
    ID3DBlob* dis = nullptr;
    if (FAILED(D3DDisassemble(code, size, 0, nullptr, &dis)))
        return false;
    std::string text(static_cast<const char*>(dis->GetBufferPointer()), dis->GetBufferSize());
    dis->Release();
    // Some mul/mad multiplies the input UV (v<tc>.<v_swz>) by the constant (cb<slot>[<reg>].<c_swz>).
    auto multiplies_uv = [&](UINT slot, UINT offset, const char* v_swz, const char* c_swz) {
        char v[16], c[24];
        snprintf(v, sizeof(v), "v%d.%s", tc_reg, v_swz);
        snprintf(c, sizeof(c), "cb%u[%u].%s", slot, offset / 16, c_swz);
        for (size_t pos = 0; pos < text.size();) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            std::string line = text.substr(pos, end - pos);
            size_t first = line.find_first_not_of(' ');
            if (first != std::string::npos && (line.compare(first, 4, "mul ") == 0 || line.compare(first, 4, "mad ") == 0) && line.find(v) != std::string::npos &&
                line.find(c) != std::string::npos)
                return true;
            pos = end + 1;
        }
        return false;
    };
    // Self-scaling = the input UV times g_VS_HPosScale.xy (.xy of a float4 register).
    if (found_hpos && hpos_offset % 16 == 0 && multiplies_uv(hpos_slot, hpos_offset, "xy", "xy")) {
        out->kind = Shader::SelfScalingVS;
        out->cb_slot = hpos_slot;
        out->offset = hpos_offset;
        ++g_status.vs_seen;
        logf("screen_size_fix: self-scaling VS: TEXCOORD0 v%d * g_VS_HPosScale cb%u +%u", tc_reg, out->cb_slot,
             out->offset);
        return true;
    }
    // Pixel-grid VS (DirOccPassVS): the input UV times g_ScreenSize.xy, any swizzle.
    if (found_size && size_offset % 16 == 0 && multiplies_uv(size_slot, size_offset, "", "")) {
        out->kind = Shader::ScreenSizeVS;
        out->cb_slot = size_slot;
        out->offset = size_offset;
        ++g_status.vs_seen;
        logf("screen_size_fix: pixel-grid VS: TEXCOORD0 v%d * g_ScreenSize cb%u +%u", tc_reg, out->cb_slot,
             out->offset);
        return true;
    }
    return false;
}

// One patched constant buffer: a copy of the engine's with 8 bytes replaced.
struct Patch {
    ID3D11Buffer* copy = nullptr;
    UINT copy_size = 0;
    ID3D11Buffer* value = nullptr;  // 16 bytes

    bool ensure(ID3D11Device* device, UINT size) {
        if (!value) {
            D3D11_BUFFER_DESC d = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (FAILED(device->CreateBuffer(&d, nullptr, &value)))
                return false;
        }
        if (copy && copy_size == size)
            return true;
        if (copy)
            copy->Release();
        copy = nullptr;
        D3D11_BUFFER_DESC d = {size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(device->CreateBuffer(&d, nullptr, &copy)))
            return false;
        copy_size = size;
        return true;
    }

    // Returns the patched buffer, or nullptr (engine buffer left alone).
    ID3D11Buffer* make(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT offset, float x, float y) {
        D3D11_BUFFER_DESC bd;
        src->GetDesc(&bd);
        if (offset + 8 > bd.ByteWidth)
            return nullptr;
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        bool ok = ensure(device, bd.ByteWidth);
        device->Release();
        if (!ok)
            return nullptr;
        const float v[4] = {x, y, 0, 0};
        ctx->UpdateSubresource(value, 0, nullptr, v, 0, 0);
        ctx->CopyResource(copy, src);
        D3D11_BOX box = {0, 0, 0, 8, 1, 1};
        ctx->CopySubresourceRegion(copy, 0, offset, 0, 0, value, 0, &box);
        return copy;
    }
};

static Patch g_ps_patch, g_bloom_patch;

// Texture the luminance pass read last (identity only): the quarter-res scene, also the bloom's source.
static void* g_lum_source;

static void* ps_resource(ID3D11DeviceContext* ctx, UINT slot) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(slot, 1, &srv);
    if (!srv)
        return nullptr;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (res)
        res->Release();
    return res;
}

// Size of render target 0 (or of the depth target when there is no colour target); false if none.
static bool target_size(ID3D11DeviceContext* ctx, UINT* w, UINT* h) {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11View* view = rtv ? static_cast<ID3D11View*>(rtv) : static_cast<ID3D11View*>(dsv);
    bool ok = false;
    if (view) {
        ID3D11Resource* res = nullptr;
        view->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC td;
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            *w = td.Width;
            *h = td.Height;
            ok = true;
        }
        res->Release();
    }
    if (rtv)
        rtv->Release();
    if (dsv)
        dsv->Release();
    return ok;
}

// Size of the 2D texture bound at PS slot `slot`; false if none.
static bool ps_texture_size(ID3D11DeviceContext* ctx, UINT slot, UINT* w, UINT* h) {
    ID3D11Resource* res = static_cast<ID3D11Resource*>(ps_resource(ctx, slot));
    if (!res)
        return false;
    D3D11_RESOURCE_DIMENSION dim;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return false;
    D3D11_TEXTURE2D_DESC td;
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
    *w = td.Width;
    *h = td.Height;
    return true;
}

TriAction scaled_tri_action(ID3D11DeviceContext* ctx, const Shader* vs) {
    if (!g_status.enabled || !game::inside_scaled_fullscreen_tri())
        return TriAction::Draw;
    if (vs && vs->kind == Shader::SelfScalingVS) {
        ++g_status.vs_fixed;
        return TriAction::Redraw;
    }
    D3D11_VIEWPORT vp;
    UINT n = 1, w = 0, h = 0;
    ctx->RSGetViewports(&n, &vp);
    if (n == 0 || !target_size(ctx, &w, &h))
        return TriAction::Draw;
    bool covers = vp.TopLeftX == 0 && vp.TopLeftY == 0 && vp.Width >= float(w) - 0.5f && vp.Height >= float(h) - 0.5f;
    // Screen-proportional: same aspect as the native render size (426x240 vs 2560x1440).
    game::Vec2 native = game::native_size();
    float screen = native.y > 0 ? native.x / native.y : 0;
    auto screen_shaped = [&](UINT tw, UINT th) {
        return screen > 0 && th > 0 && fabsf(float(tw) / float(th) / screen - 1.0f) < 0.01f;
    };
    if (!covers) {
        // A partial viewport is usually the sub-rect already, but reduction chains on small non-screen targets
        // (texToTex 128x64 -> 64x32 -> 16x8 ... -> 2x1, both target and source 2:1) draw into part of their target
        // too; their source holds no scene frame, so scaled UVs zoomed into it (A/B diff kcd_frame19102 vs 20786).
        UINT sw = 0, sh = 0;
        if (!screen_shaped(w, h) && ps_texture_size(ctx, 0, &sw, &sh) && !screen_shaped(sw, sh)) {
            ++g_status.non_screen;
            return TriAction::Redraw;
        }
        return TriAction::Draw;
    }
    if (screen_shaped(w, h)) {
        ++g_status.shrunk;
        return TriAction::Shrink;
    }
    ++g_status.non_screen;
    return TriAction::Redraw;
}

// g_mViewProj patch, done on the GPU (the engine's constant buffer is GPU-only; reading it back would stall the
// frame): a compute shader reads the engine buffer as a constant buffer and writes it to a raw UAV buffer with the
// clip x/y rows remapped into the sub-rect (uv' = uv * s): row0' = s.x*row0 + (s.x-1)*row3,
// row1' = s.y*row1 + (1-s.y)*row3 (y is flipped in the UV). Then the UAV buffer is copied into a constant buffer.
static const char kViewProjCS[] = R"(
cbuffer Src : register(b0) { float4 src[4096]; };
cbuffer Params : register(b1) {
    uint reg; uint count; float sx; float sy;
    uint mode; uint3 pad;  // mode 1: up to 3 registers rq[j].x become src[rq.x] * m + src[rq.y] * k + c instead
    uint4 rq[3]; float4 m[3]; float4 k[3]; float4 c[3];
};
RWByteAddressBuffer dst : register(u0);
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    if (i >= count)
        return;
    float4 v = src[i];
    if (mode == 1) {
        for (uint j = 0; j < 3; ++j)
            if (i == rq[j].x)
                v = src[i] * m[j] + src[rq[j].y] * k[j] + c[j];
    } else if (i == reg) {
        v = sx * src[reg] + (sx - 1.0) * src[reg + 3];
    } else if (i == reg + 1) {
        v = sy * src[reg + 1] + (1.0 - sy) * src[reg + 3];
    }
    dst.Store4(i * 16, asuint(v));
}
)";

// One float4 register rewrite: reg(off) = reg(off) * mul + reg(add_off) * add_mul + add (byte offsets, ~0u: unused).
struct RegOp {
    UINT off = ~0u;
    float mul[4] = {1, 1, 1, 1};
    UINT add_off = ~0u;
    float add_mul[4] = {0, 0, 0, 0};
    float add[4] = {0, 0, 0, 0};
};

struct ViewProjPatch {
    struct Params {
        UINT reg, count;
        float sx, sy;
        UINT mode, pad[3];
        UINT rq[3][4];
        float m[3][4], k[3][4], c[3][4];
    };
    bool failed = false;
    ID3D11ComputeShader* cs = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11Buffer* raw = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer* copy = nullptr;
    UINT size = 0;

    bool ensure(ID3D11Device* device, UINT bytes) {
        if (failed)
            return false;
        if (!cs) {
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            if (FAILED(D3DCompile(kViewProjCS, sizeof(kViewProjCS) - 1, "viewproj_cs", nullptr, nullptr, "main",
                                  "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
                logf("screen_size_fix: viewproj CS compile failed: %s",
                     errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors)
                    errors->Release();
                failed = true;
                return false;
            }
            HRESULT hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
            code->Release();
            D3D11_BUFFER_DESC d = {sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (FAILED(hr) || FAILED(device->CreateBuffer(&d, nullptr, &params))) {
                failed = true;
                return false;
            }
        }
        if (size == bytes)
            return true;
        if (uav)
            uav->Release();
        if (raw)
            raw->Release();
        if (copy)
            copy->Release();
        uav = nullptr;
        raw = copy = nullptr;
        size = 0;
        D3D11_BUFFER_DESC rd = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_BUFFER_DESC cd = {bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device->CreateBuffer(&rd, nullptr, &raw)) || FAILED(device->CreateUnorderedAccessView(raw, &ud, &uav)) ||
            FAILED(device->CreateBuffer(&cd, nullptr, &copy))) {
            logf("screen_size_fix: viewproj buffers (%u bytes) failed", bytes);
            failed = true;
            return false;
        }
        size = bytes;
        return true;
    }

    // Returns the patched buffer (g_mViewProj at offset remapped into the sub-rect), or nullptr.
    ID3D11Buffer* make(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT offset, float sx, float sy) {
        Params p = {offset / 16, 0, sx, sy, 0, {}, {{~0u}, {~0u}, {~0u}}, {}, {}, {}};
        return run(ctx, src, offset + 64, p);
    }

    // Returns a copy with up to 3 float4s rewritten as in mode 1 of the CS, or nullptr.
    ID3D11Buffer* rewrite(ID3D11DeviceContext* ctx, ID3D11Buffer* src, const RegOp (&ops)[3]) {
        Params p = {~0u, 0, 1, 1, 1, {}, {}, {}, {}, {}};
        UINT end = 0;
        for (int j = 0; j < 3; ++j) {
            const RegOp& o = ops[j];
            p.rq[j][0] = o.off == ~0u ? ~0u : o.off / 16;
            p.rq[j][1] = o.add_off == ~0u ? p.rq[j][0] : o.add_off / 16;
            memcpy(p.m[j], o.mul, sizeof(p.m[j]));
            memcpy(p.k[j], o.add_mul, sizeof(p.k[j]));
            memcpy(p.c[j], o.add, sizeof(p.c[j]));
            for (UINT e : {o.off, o.add_off})
                if (e != ~0u && e + 16 > end)
                    end = e + 16;
        }
        return run(ctx, src, end, p);
    }

    ID3D11Buffer* run(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT end, Params& p) {
        D3D11_BUFFER_DESC bd;
        src->GetDesc(&bd);
        if (end > bd.ByteWidth || bd.ByteWidth % 16 != 0 || bd.ByteWidth > 4096 * 16)
            return nullptr;
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        bool ok = ensure(device, bd.ByteWidth);
        device->Release();
        if (!ok)
            return nullptr;
        p.count = bd.ByteWidth / 16;
        ctx->UpdateSubresource(params, 0, nullptr, &p, 0, 0);

        // Save the compute bindings we touch.
        ID3D11ComputeShader* old_cs = nullptr;
        ID3D11ClassInstance* inst[256];
        UINT ninst = 256;
        ctx->CSGetShader(&old_cs, inst, &ninst);
        ID3D11Buffer* old_cb[2] = {};
        ctx->CSGetConstantBuffers(0, 2, old_cb);
        ID3D11UnorderedAccessView* old_uav = nullptr;
        ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

        ctx->CSSetShader(cs, nullptr, 0);
        ID3D11Buffer* cbs[2] = {src, params};
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch((p.count + 63) / 64, 1, 1);
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ctx->CopyResource(copy, raw);

        UINT keep = ~0u;
        ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
        ctx->CSSetConstantBuffers(0, 2, old_cb);
        ctx->CSSetShader(old_cs, inst, ninst);
        if (old_cs)
            old_cs->Release();
        for (UINT i = 0; i < ninst; ++i)
            if (inst[i])
                inst[i]->Release();
        for (ID3D11Buffer* b : old_cb)
            if (b)
                b->Release();
        if (old_uav)
            old_uav->Release();
        return copy;
    }
};

static ViewProjPatch g_viewproj_patch, g_viewspace_patch, g_reproj_patch;  // separate: buffer sizes differ
static ViewProjPatch g_ssdo_size_patch, g_ssdo_vs_patch;  // bound at the same time as g_viewspace_patch's

// Edge padding: outside the sub-rect the linear depth targets hold 1.0 (far plane), and the sampler only clamps at
// the texture border. Taps past the sub-rect's right/bottom edge (SSDO's offsets, the SVO trace's screen march) then
// saw sky: unoccluded sky ambient (a blue glow along the right edge under the canopy). We replicate the last valid
// column and row over the outside region, like a clamp at the sub-rect, with a full-target draw that discards inside.
static const char kPadVS[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
static const char kPadPS[] = R"(
Texture2D<float4> col : register(t0);  // 1 x H: column w - 1
Texture2D<float4> row : register(t1);  // W x 1: row h - 1
cbuffer Params : register(b0) { uint w; uint h; uint2 pad; };
float4 main(float4 pos : SV_Position) : SV_Target {
    uint2 p = uint2(pos.xy);
    if (p.x < w && p.y < h)
        discard;
    if (p.y < h)
        return col.Load(int3(0, p.y, 0));
    return row.Load(int3(min(p.x, w - 1), 0, 0));
}
)";

struct EdgePad {
    struct Temp {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
        UINT w = 0, h = 0;
        ID3D11Texture2D* col = nullptr;
        ID3D11Texture2D* row = nullptr;
        ID3D11ShaderResourceView* srv[2] = {};
    };
    bool failed = false;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* params = nullptr;
    Temp temps[4];
    uint64_t padded = 0;

    static ID3DBlob* compile(const char* src, size_t len, const char* target) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(src, len, "edge_pad", nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                              &code, &errors))) {
            logf("screen_size_fix: edge pad %s compile failed: %s", target,
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
        ID3DBlob* vc = compile(kPadVS, sizeof(kPadVS) - 1, "vs_5_0");
        ID3DBlob* pc = compile(kPadPS, sizeof(kPadPS) - 1, "ps_5_0");
        D3D11_BUFFER_DESC d = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        failed = !vc || !pc ||
                 FAILED(device->CreateVertexShader(vc->GetBufferPointer(), vc->GetBufferSize(), nullptr, &vs)) ||
                 FAILED(device->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps)) ||
                 FAILED(device->CreateBuffer(&d, nullptr, &params));
        if (vc)
            vc->Release();
        if (pc)
            pc->Release();
        return !failed;
    }

    Temp* temp(ID3D11Device* device, DXGI_FORMAT format, DXGI_FORMAT view, UINT w, UINT h) {
        Temp* free_slot = nullptr;
        for (Temp& t : temps) {
            if (t.col && t.format == format && t.view == view && t.w == w && t.h == h)
                return &t;
            if (!free_slot && !t.col)
                free_slot = &t;
        }
        Temp* t = free_slot ? free_slot : &temps[0];  // more than 4 kinds: recycle
        for (ID3D11ShaderResourceView*& s : t->srv)
            if (s)
                s->Release(), s = nullptr;
        if (t->col)
            t->col->Release(), t->col = nullptr;
        if (t->row)
            t->row->Release(), t->row = nullptr;
        D3D11_TEXTURE2D_DESC d = {1, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = view;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        bool ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t->col));
        d.Width = w, d.Height = 1;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t->row)) &&
             SUCCEEDED(device->CreateShaderResourceView(t->col, &sd, &t->srv[0])) &&
             SUCCEEDED(device->CreateShaderResourceView(t->row, &sd, &t->srv[1]));
        if (!ok) {
            logf("screen_size_fix: edge pad temps (format %d, %ux%u) failed", format, w, h);
            if (t->col)
                t->col->Release(), t->col = nullptr;
            return nullptr;
        }
        t->format = format, t->view = view, t->w = w, t->h = h;
        return t;
    }

    // Pads every bound PS texture that is a screen-proportional linear depth target (R32_FLOAT / R16G16_FLOAT,
    // render-target capable), with the frame's downscale d.
    void run(ID3D11DeviceContext* ctx, game::Vec2 d) {
        game::Vec2 native = game::native_size();
        if (native.x <= 0 || native.y <= 0)
            return;
        ID3D11ShaderResourceView* srvs[16] = {};
        ctx->PSGetShaderResources(0, 16, srvs);
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        bool ready = ensure(device);
        for (ID3D11ShaderResourceView* srv : srvs) {
            if (!srv)
                continue;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd;
            srv->GetDesc(&vd);
            ID3D11Resource* res = nullptr;
            srv->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim;
            res->GetType(&dim);
            D3D11_TEXTURE2D_DESC td = {};
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            const UINT w = UINT(floorf(float(td.Width) * d.x + 0.01f)), h = UINT(floorf(float(td.Height) * d.y + 0.01f));
            const bool depth_like = vd.Format == DXGI_FORMAT_R32_FLOAT || vd.Format == DXGI_FORMAT_R16G16_FLOAT;
            const bool screen = td.Height > 0 && td.Width <= UINT(native.x) &&
                                fabsf(float(td.Width) / float(td.Height) / (native.x / native.y) - 1.0f) < 0.01f;
            if (ready && depth_like && screen && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D &&
                vd.Texture2D.MostDetailedMip == 0 && td.SampleDesc.Count == 1 &&
                (td.BindFlags & D3D11_BIND_RENDER_TARGET) && w > 0 && h > 0 && (w < td.Width || h < td.Height))
                pad(ctx, device, res, td, vd.Format, w, h);
            res->Release();
        }
        for (ID3D11ShaderResourceView* srv : srvs)
            if (srv)
                srv->Release();
        device->Release();
    }

    void pad(ID3D11DeviceContext* ctx, ID3D11Device* device, ID3D11Resource* res, const D3D11_TEXTURE2D_DESC& td,
             DXGI_FORMAT view, UINT w, UINT h) {
        Temp* t = temp(device, td.Format, view, td.Width, td.Height);
        if (!t)
            return;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = view;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(device->CreateRenderTargetView(res, &rd, &rtv)))
            return;
        D3D11_BOX col = {w - 1, 0, 0, w, h, 1}, row = {0, h - 1, 0, w, h, 1};
        ctx->CopySubresourceRegion(t->col, 0, 0, 0, 0, res, 0, &col);
        ctx->CopySubresourceRegion(t->row, 0, 0, 0, 0, res, 0, &row);
        const UINT p[4] = {w, h, 0, 0};
        ctx->UpdateSubresource(params, 0, nullptr, p, 0, 0);
        {
            StateBackup backup(ctx);
            ID3D11ShaderResourceView* null_srv[2] = {};
            ctx->PSSetShaderResources(0, 2, null_srv);  // the target may be bound as an input
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->RSSetState(nullptr);
            D3D11_VIEWPORT vp = {0, 0, float(td.Width), float(td.Height), 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetShaderResources(0, 2, t->srv);
            ctx->PSSetConstantBuffers(0, 1, &params);
            draw_fn(ctx, 3, 0);
        }
        rtv->Release();
        ++padded;
    }

    DrawFn draw_fn = nullptr;
};

static EdgePad g_edge_pad;

static void pad_depth_inputs(ID3D11DeviceContext* ctx, DrawFn original) {
    game::Vec2 d = game::current_downscale();
    if (!(d.x < 0.999f || d.y < 0.999f))
        return;
    g_edge_pad.draw_fn = original;
    g_edge_pad.run(ctx, d);
    g_status.edge_padded = g_edge_pad.padded;
}

// SSDO noise tiling: binds copies of the VS's g_ScreenSize buffer and, when it is not the ViewSpaceParams one, the
// PS's PS_ScreenSize buffer with .xy / s and .zw * s. Undone by ssdo_noise_end.
struct SsdoNoise {
    ID3D11Buffer* vs_cb = nullptr;
    ID3D11Buffer* ps_cb = nullptr;
    UINT vs_slot = ~0u, ps_slot = ~0u;
};

static void size_op(RegOp& op, UINT offset, game::Vec2 d) {
    op.off = offset;
    const float m[4] = {1.0f / d.x, 1.0f / d.y, d.x, d.y};
    memcpy(op.mul, m, sizeof(m));
}

static SsdoNoise ssdo_noise_begin(ID3D11DeviceContext* ctx, const Shader& ps, const Shader* vs, game::Vec2 d) {
    SsdoNoise n;
    if (vs && vs->kind == Shader::ScreenSizeVS) {
        ctx->VSGetConstantBuffers(vs->cb_slot, 1, &n.vs_cb);
        RegOp ops[3];
        size_op(ops[0], vs->offset, d);
        if (ID3D11Buffer* b = n.vs_cb ? g_ssdo_vs_patch.rewrite(ctx, n.vs_cb, ops) : nullptr) {
            ctx->VSSetConstantBuffers(vs->cb_slot, 1, &b);
            n.vs_slot = vs->cb_slot;
        }
    }
    if (ps.size_slot != ~0u && ps.size_slot != ps.cb_slot) {
        ctx->PSGetConstantBuffers(ps.size_slot, 1, &n.ps_cb);
        RegOp ops[3];
        size_op(ops[0], ps.size_offset, d);
        if (ID3D11Buffer* b = n.ps_cb ? g_ssdo_size_patch.rewrite(ctx, n.ps_cb, ops) : nullptr) {
            ctx->PSSetConstantBuffers(ps.size_slot, 1, &b);
            n.ps_slot = ps.size_slot;
        }
    }
    if (n.vs_slot != ~0u)
        ++g_status.ssdo_noise_fixed;
    return n;
}

static void ssdo_noise_end(ID3D11DeviceContext* ctx, SsdoNoise& n) {
    if (n.vs_slot != ~0u)
        ctx->VSSetConstantBuffers(n.vs_slot, 1, &n.vs_cb);
    if (n.ps_slot != ~0u)
        ctx->PSSetConstantBuffers(n.ps_slot, 1, &n.ps_cb);
    if (n.vs_cb)
        n.vs_cb->Release();
    if (n.ps_cb)
        n.ps_cb->Release();
}

void draw(ID3D11DeviceContext* ctx, const Shader& ps, const Shader* vs, UINT count, UINT start, DrawFn original) {
    if (!g_status.enabled || ps.kind == Shader::DofPS || ps.kind == Shader::DofGatherPS) {
        original(ctx, count, start);
        return;
    }
    if (ps.kind == Shader::ViewProjPS || (ps.kind == Shader::ViewSpacePS && !ps.radius_xy_only))
        pad_depth_inputs(ctx, original);
    if (ps.kind == Shader::ViewProjPS) {
        game::Vec2 d = game::current_downscale();
        ID3D11Buffer* cb = nullptr;
        if (d.x < 0.999f || d.y < 0.999f)
            ctx->PSGetConstantBuffers(ps.cb_slot, 1, &cb);
        ID3D11Buffer* b = cb ? g_viewproj_patch.make(ctx, cb, ps.offset, d.x, d.y) : nullptr;
        if (b) {
            ctx->PSSetConstantBuffers(ps.cb_slot, 1, &b);
            ++g_status.ps_fixed;
        }
        original(ctx, count, start);
        if (b)
            ctx->PSSetConstantBuffers(ps.cb_slot, 1, &cb);
        if (cb)
            cb->Release();
        return;
    }
    if (ps.kind == Shader::ViewSpacePS || ps.kind == Shader::ReprojPS) {
        game::Vec2 d = game::current_downscale();
        ID3D11Buffer* cb = nullptr;
        if (d.x < 0.999f || d.y < 0.999f)
            ctx->PSGetConstantBuffers(ps.cb_slot, 1, &cb);
        RegOp ops[3];
        const bool ssdo_noise = cb && ps.kind == Shader::ViewSpacePS && !ps.radius_xy_only && g_status.ssdo_noise;
        if (ps.kind == Shader::ViewSpacePS) {
            ops[0].off = ps.offset;
            const float view[4] = {1.0f / d.x, 1.0f / d.y, 1, 1};
            memcpy(ops[0].mul, view, sizeof(view));
            ops[1].off = ps.radius_offset;
            const float radius[4] = {d.x, d.y, ps.radius_xy_only ? 1.0f : d.x, ps.radius_xy_only ? 1.0f : d.x};
            memcpy(ops[1].mul, radius, sizeof(radius));
            if (ssdo_noise && ps.size_slot == ps.cb_slot)
                size_op(ops[2], ps.size_offset, d);
        } else {
            // prev = (M * (uv, 1, w)).xy / row2: input uv = uv'/s (columns x, y * 1/s), output * s (rows 0, 1).
            const float m[3][4] = {{1, d.x / d.y, d.x, d.x}, {d.y / d.x, 1, d.y, d.y}, {1 / d.x, 1 / d.y, 1, 1}};
            for (int j = 0; j < 3; ++j) {
                ops[j].off = ps.offset + 16 * j;
                memcpy(ops[j].mul, m[j], sizeof(m[j]));
            }
        }
        ID3D11Buffer* b = cb ? (ps.kind == Shader::ViewSpacePS ? g_viewspace_patch : g_reproj_patch).rewrite(ctx, cb, ops)
                             : nullptr;
        if (b) {
            ctx->PSSetConstantBuffers(ps.cb_slot, 1, &b);
            ++g_status.ps_fixed;
        }
        SsdoNoise noise;
        if (ssdo_noise)
            noise = ssdo_noise_begin(ctx, ps, vs, d);
        original(ctx, count, start);
        ssdo_noise_end(ctx, noise);
        if (b)
            ctx->PSSetConstantBuffers(ps.cb_slot, 1, &cb);
        if (cb)
            cb->Release();
        return;
    }
    if (ps.kind == Shader::LumInitialPS) {
        // Only remembered as the bloom's source. Its quad already gets sub-rect UVs from the engine (0..s, A/B diff
        // kcd_frame19102 vs 20786); patching g_VS_HPosScale too made it read the top-left s^2 (wrong exposure).
        g_lum_source = ps_resource(ctx, 0);
        original(ctx, count, start);
        return;
    }
    game::Vec2 scale = game::current_viewport_scale();
    if (scale.x >= 0.999f && scale.y >= 0.999f) {
        original(ctx, count, start);
        return;
    }
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* zmap = nullptr;
    ctx->PSGetShaderResources(ps.zmap_slot, 1, &zmap);
    ctx->PSGetConstantBuffers(ps.cb_slot, 1, &cb);
    bool patched = false;
    if (zmap && cb) {
        ID3D11Resource* res = nullptr;
        zmap->GetResource(&res);
        D3D11_TEXTURE2D_DESC td;
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
        res->Release();
        if (ID3D11Buffer* b = g_ps_patch.make(ctx, cb, ps.offset, float(td.Width), float(td.Height))) {
            ctx->PSSetConstantBuffers(ps.cb_slot, 1, &b);
            ++g_status.ps_fixed;
            patched = true;
        }
    }
    original(ctx, count, start);
    if (patched)
        ctx->PSSetConstantBuffers(ps.cb_slot, 1, &cb);
    if (cb)
        cb->Release();
    if (zmap)
        zmap->Release();
}

bool draw_dof(ID3D11DeviceContext* ctx, const Shader* ps, const Shader* vs, UINT count, UINT start, DrawFn original) {
    const bool dof = (ps && (ps->kind == Shader::DofPS || ps->kind == Shader::DofGatherPS)) ||
                     (vs && vs->kind == Shader::DofVS);
    if (!g_status.enabled || !dof || !game::inside_engine_region_tri())
        return false;
    game::Vec2 d = game::current_downscale();
    D3D11_VIEWPORT vp;
    UINT n = 1, w = 0, h = 0;
    ctx->RSGetViewports(&n, &vp);
    if (n == 0 || !target_size(ctx, &w, &h))
        return false;
    bool covers = vp.TopLeftX == 0 && vp.TopLeftY == 0 && vp.Width >= float(w) - 0.5f && vp.Height >= float(h) - 0.5f;
    game::Vec2 native = game::native_size();
    float aspect = float(w) / float(h), screen = native.y > 0 ? native.x / native.y : 0;
    if (!covers || screen <= 0 || fabsf(aspect / screen - 1.0f) >= 0.01f)
        return false;
    D3D11_VIEWPORT sub = vp;
    sub.Width = floorf(vp.Width * d.x + 0.5f);
    sub.Height = floorf(vp.Height * d.y + 0.5f);
    ctx->RSSetViewports(1, &sub);
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* b = nullptr;
    if (ps && ps->kind == Shader::DofGatherPS) {
        ctx->PSGetConstantBuffers(ps->cb_slot, 1, &cb);
        b = cb ? g_ps_patch.make(ctx, cb, ps->offset, vp.Width * d.x, vp.Height * d.y) : nullptr;
        if (b) {
            ctx->PSSetConstantBuffers(ps->cb_slot, 1, &b);
            ++g_status.ps_fixed;
        }
    }
    original(ctx, count, start);
    if (b)
        ctx->PSSetConstantBuffers(ps->cb_slot, 1, &cb);
    if (cb)
        cb->Release();
    ctx->RSSetViewports(1, &vp);
    ++g_status.dof_shrunk;
    return true;
}

static ViewProjPatch g_tiled_patch;

void dispatch(ID3D11DeviceContext* ctx, const Shader& cs, UINT x, UINT y, UINT z, DispatchFn original) {
    game::Vec2 d = game::current_downscale();
    game::Vec2 full = game::native_size();
    ID3D11Buffer* cb = nullptr;
    if (g_status.enabled && cs.kind == Shader::TiledShadingCS && (d.x < 0.999f || d.y < 0.999f) && full.x > 0 &&
        full.y > 0)
        ctx->CSGetConstantBuffers(cs.cb_slot, 1, &cb);
    ID3D11Buffer* b = nullptr;
    if (cb) {
        RegOp ops[3];
        // View rays: TR' = TR / s.x + TL * (1 - 1 / s.x), BL' likewise with s.y.
        ops[0].off = cs.tiled[0];
        ops[1].off = cs.tiled[1];
        for (int c = 0; c < 4; ++c) {
            ops[0].mul[c] = 1.0f / d.x;
            ops[1].mul[c] = 1.0f / d.y;
            ops[0].add_mul[c] = 1.0f - 1.0f / d.x;
            ops[1].add_mul[c] = 1.0f - 1.0f / d.y;
        }
        ops[0].add_off = ops[1].add_off = cs.offset;
        // Tile planes: scale (T / 2) * ProjParams.xy and centre (T / 2 - group) + ProjParams.zw, T = ceil(size / 8);
        // the sub-rect wants n = size * s / 8 tiles: xy * n / T, zw + (n - T) / 2.
        float tx = ceilf(full.x / 8), ty = ceilf(full.y / 8), nx = full.x * d.x / 8, ny = full.y * d.y / 8;
        ops[2].off = cs.tiled[2];
        const float pm[4] = {nx / tx, ny / ty, 1, 1}, pa[4] = {0, 0, (nx - tx) / 2, (ny - ty) / 2};
        memcpy(ops[2].mul, pm, sizeof(pm));
        memcpy(ops[2].add, pa, sizeof(pa));
        b = g_tiled_patch.rewrite(ctx, cb, ops);
        if (b) {
            ctx->CSSetConstantBuffers(cs.cb_slot, 1, &b);
            ++g_status.ps_fixed;
        }
    }
    original(ctx, x, y, z);
    if (b)
        ctx->CSSetConstantBuffers(cs.cb_slot, 1, &cb);
    if (cb)
        cb->Release();
}

bool draw_bloom_source(ID3D11DeviceContext* ctx, const Shader& vs, UINT count, UINT start, DrawFn original) {
    if (!g_status.enabled || vs.kind != Shader::SelfScalingVS || !g_lum_source)
        return false;
    game::Vec2 d = game::current_downscale();
    if (!(d.x < 0.999f || d.y < 0.999f) || ps_resource(ctx, 0) != g_lum_source)
        return false;
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(vs.cb_slot, 1, &cb);
    ID3D11Buffer* b = cb ? g_bloom_patch.make(ctx, cb, vs.offset, d.x, d.y) : nullptr;
    if (!b) {
        if (cb)
            cb->Release();
        return false;
    }
    ctx->VSSetConstantBuffers(vs.cb_slot, 1, &b);
    original(ctx, count, start);
    ctx->VSSetConstantBuffers(vs.cb_slot, 1, &cb);
    cb->Release();
    ++g_status.bloom_fixed;
    return true;
}

}  // namespace kc::screen_size_fix
