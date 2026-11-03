#include "hooks/d3d11_hooks.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <MinHook.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "game/renderer.h"
#include "renderer/canary.h"
#include "renderer/resource_slots.h"
#include "renderer/subrect_fixes.h"
#include "renderer/mip_bias.h"
#include "renderer/screen_size_fix.h"
#include "renderer/shadow_noise.h"
#include "renderer/taa_dlss.h"
#include "renderer/velocity_format.h"
#include "renderer/velocity_mask.h"

namespace kc::d3d11 {

using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                         ID3D11PixelShader**);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*,
                                               UINT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                          ID3D11VertexShader**);
using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*,
                                               UINT);
using PSSetSamplersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
using CreateTexture2DFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                       const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateSRVFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                                ID3D11ShaderResourceView**);
using CreateRTVFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                ID3D11RenderTargetView**);
using CreateComputeShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                                           ID3D11ComputeShader**);
using CSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*,
                                               UINT);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*, UINT, UINT,
                                                        ID3D11UnorderedAccessView* const*, const UINT*);

// Vtable slots (d3d11.h declaration order).
constexpr int kCreateTexture2DSlot = 5;  // ID3D11Device
constexpr int kCreateSRVSlot = 7;
constexpr int kCreateRTVSlot = 9;
constexpr int kCreateVertexShaderSlot = 12;
constexpr int kCreatePixelShaderSlot = 15;
constexpr int kCreateComputeShaderSlot = 18;
constexpr int kPSSetShaderSlot = 9;         // ID3D11DeviceContext
constexpr int kPSSetSamplersSlot = 10;
constexpr int kVSSetShaderSlot = 11;
constexpr int kDrawIndexedSlot = 12;
constexpr int kDrawSlot = 13;
constexpr int kDrawIndexedInstancedSlot = 20;
constexpr int kDrawInstancedSlot = 21;
constexpr int kOMSetTargetsAndUavsSlot = 34;
constexpr int kDispatchSlot = 41;
constexpr int kOMSetRenderTargetsSlot = 33;
constexpr int kCSSetShaderSlot = 69;

static CreatePixelShaderFn o_create_pixel_shader;
static PSSetShaderFn o_ps_set_shader;
static DrawFn o_draw;
static CreateVertexShaderFn o_create_vertex_shader;
static VSSetShaderFn o_vs_set_shader;
static PSSetSamplersFn o_ps_set_samplers;
static CreateComputeShaderFn o_create_compute_shader;
static CreateTexture2DFn o_create_texture2d;
static CreateSRVFn o_create_srv;
static CreateRTVFn o_create_rtv;
static CSSetShaderFn o_cs_set_shader;
static screen_size_fix::DispatchFn o_dispatch;
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                       ID3D11DepthStencilView*);
static OMSetRenderTargetsFn o_om_set_render_targets;
static DrawIndexedFn o_draw_indexed;
static DrawIndexedInstancedFn o_draw_indexed_instanced;
static DrawInstancedFn o_draw_instanced;
static OMSetTargetsAndUavsFn o_om_set_targets_and_uavs;

static Status g_status;
static std::mutex g_mutex;  // guards g_taa (written on shader-creating threads, read on the render thread)
static std::vector<TaaShader> g_taa;
static ID3D11PixelShader* g_bound_ps;      // render thread
static const TaaShader* g_bound_taa;       // render thread: g_bound_ps if it is a TAA shader
static const TaaShader* g_last_taa;
static std::vector<screen_size_fix::Shader> g_fix;   // guarded by g_mutex, never reallocated (reserve)
static const screen_size_fix::Shader* g_bound_fix;  // render thread
static std::vector<screen_size_fix::Shader> g_fix_vs;  // guarded by g_mutex, never reallocated (reserve)
static ID3D11VertexShader* g_bound_vs;              // render thread
static const screen_size_fix::Shader* g_bound_fix_vs;
static std::vector<screen_size_fix::Shader> g_fix_cs;  // guarded by g_mutex, never reallocated (reserve)
static ID3D11ComputeShader* g_bound_cs;             // render thread
static const screen_size_fix::Shader* g_bound_fix_cs;
static subrect_fixes::Shader g_bound_subrect;
static bool g_bound_bloom_vs;
static UINT g_ps_classes, g_vs_classes, g_cs_classes;
static uint32_t g_ps_slots = resource_slots::all, g_vs_slots = resource_slots::all, g_cs_slots = resource_slots::all;
static thread_local bool t_inside;         // our own draws do not re-enter

const Status& status() { return g_status; }
const TaaShader* last_taa_shader() { return g_last_taa; }

// Bytecode contains the string: cheap filter before reflecting.
static bool contains(const void* data, SIZE_T size, const char* s) {
    const size_t n = strlen(s);
    const auto* p = static_cast<const char*>(data);
    for (SIZE_T i = 0; i + n <= size; ++i)
        if (p[i] == s[0] && memcmp(p + i, s, n) == 0)
            return true;
    return false;
}

static bool reflect_taa(const void* code, SIZE_T size, TaaShader* out) {
    if (!contains(code, size, "mReprojection") || !contains(code, size, "PostAA_DepthTex"))
        return false;
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return false;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b;
        r->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_TEXTURE) {
            if (!strcmp(b.Name, "_tex3"))
                out->slot_velocity = b.BindPoint;
            else if (!strcmp(b.Name, "_tex4"))
                out->slot_colour = b.BindPoint;
            else if (!strcmp(b.Name, "_tex5"))
                out->slot_history = b.BindPoint;
            else if (!strcmp(b.Name, "PostAA_DepthTex"))
                out->slot_depth = b.BindPoint;
        }
    }
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC cbd;
        cb->GetDesc(&cbd);
        for (UINT v = 0; v < cbd.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd;
            var->GetDesc(&vd);
            if (strcmp(vd.Name, "mReprojection") != 0)
                continue;
            D3D11_SHADER_TYPE_DESC td;
            var->GetType()->GetDesc(&td);
            D3D11_SHADER_INPUT_BIND_DESC b;
            if (SUCCEEDED(r->GetResourceBindingDescByName(cbd.Name, &b)))
                out->cb_slot = b.BindPoint;
            out->reproj_offset = vd.StartOffset;
            out->reproj_row_major = td.Class == D3D_SVC_MATRIX_ROWS;
        }
    }
    r->Release();
    bool ok = out->slot_velocity != ~0u && out->slot_colour != ~0u && out->slot_depth != ~0u && out->cb_slot != ~0u;
    logf("d3d11: TAA shader: velocity t%u colour t%u history t%u depth t%u, mReprojection cb%u +%u (%s)%s",
         out->slot_velocity, out->slot_colour, out->slot_history, out->slot_depth, out->cb_slot, out->reproj_offset,
         out->reproj_row_major ? "row major" : "column major", ok ? "" : " INCOMPLETE, ignored");
    return ok;
}

static HRESULT STDMETHODCALLTYPE hk_create_pixel_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                                        ID3D11ClassLinkage* linkage, ID3D11PixelShader** out) {
    HRESULT hr = o_create_pixel_shader(device, code, size, linkage, out);
    if (SUCCEEDED(hr) && out && *out && !t_inside) {
        resource_slots::tag(*out, code, size);
        {
            ScopedInside inside;
            subrect_fixes::on_create_ps(device, code, size, *out);
        }
        screen_size_fix::Shader f;
        if (screen_size_fix::reflect_ps(code, size, &f)) {
            f.shader = *out;
            std::lock_guard lock(g_mutex);
            g_fix.reserve(256);
            if (g_fix.size() < g_fix.capacity())
                g_fix.push_back(f);
        }
        TaaShader t;
        if (reflect_taa(code, size, &t)) {
            t.shader = *out;
            std::lock_guard lock(g_mutex);
            g_taa.reserve(64);  // pointers into g_taa are handed out; never reallocate below 64 shaders
            if (g_taa.size() < g_taa.capacity()) {
                g_taa.push_back(t);
                ++g_status.taa_shaders_seen;
            }
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_vertex_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                                         ID3D11ClassLinkage* linkage, ID3D11VertexShader** out) {
    HRESULT hr = o_create_vertex_shader(device, code, size, linkage, out);
    if (SUCCEEDED(hr) && out && *out && !t_inside) {
        resource_slots::tag(*out, code, size);
        subrect_fixes::on_create_vs(code, size, *out);
        screen_size_fix::Shader f;
        if (screen_size_fix::reflect_vs(code, size, &f)) {
            f.shader = *out;
            std::lock_guard lock(g_mutex);
            g_fix_vs.reserve(256);
            if (g_fix_vs.size() < g_fix_vs.capacity())
                g_fix_vs.push_back(f);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_texture2d(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* desc,
                                                     const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC changed;
    if (!t_inside && velocity_format::adjust_texture(desc, &changed))
        return o_create_texture2d(device, &changed, init, out);
    if (!t_inside && out && shadow_noise::adjust_texture(desc, init, &changed)) {
        HRESULT hr = o_create_texture2d(device, &changed, init, out);
        if (SUCCEEDED(hr) && *out)
            shadow_noise::on_created(*out);
        return hr;
    }
    return o_create_texture2d(device, desc, init, out);
}

static HRESULT STDMETHODCALLTYPE hk_create_srv(ID3D11Device* device, ID3D11Resource* res,
                                               const D3D11_SHADER_RESOURCE_VIEW_DESC* desc,
                                               ID3D11ShaderResourceView** out) {
    if (desc && velocity_format::view_format(res, desc->Format) != desc->Format) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = *desc;
        d.Format = velocity_format::view_format(res, desc->Format);
        return o_create_srv(device, res, &d, out);
    }
    return o_create_srv(device, res, desc, out);
}

static HRESULT STDMETHODCALLTYPE hk_create_rtv(ID3D11Device* device, ID3D11Resource* res,
                                               const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out) {
    if (desc && velocity_format::view_format(res, desc->Format) != desc->Format) {
        D3D11_RENDER_TARGET_VIEW_DESC d = *desc;
        d.Format = velocity_format::view_format(res, desc->Format);
        return o_create_rtv(device, res, &d, out);
    }
    return o_create_rtv(device, res, desc, out);
}

static HRESULT STDMETHODCALLTYPE hk_create_compute_shader(ID3D11Device* device, const void* code, SIZE_T size,
                                                          ID3D11ClassLinkage* linkage, ID3D11ComputeShader** out) {
    HRESULT hr = o_create_compute_shader(device, code, size, linkage, out);
    if (SUCCEEDED(hr) && out && *out && !t_inside) {
        resource_slots::tag(*out, code, size);
        screen_size_fix::Shader f;
        if (screen_size_fix::reflect_cs(code, size, &f)) {
            f.shader = *out;
            std::lock_guard lock(g_mutex);
            g_fix_cs.reserve(16);
            if (g_fix_cs.size() < g_fix_cs.capacity())
                g_fix_cs.push_back(f);
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE hk_cs_set_shader(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs,
                                               ID3D11ClassInstance* const* inst, UINT n) {
    if ((cs != g_bound_cs || n != g_cs_classes) && !t_inside) {
        g_bound_cs = cs;
        g_cs_classes = n;
        g_cs_slots = resource_slots::get(cs, n);
        g_bound_fix_cs = nullptr;
        if (cs) {
            std::lock_guard lock(g_mutex);
            for (const screen_size_fix::Shader& f : g_fix_cs)
                if (f.shader == cs) {
                    g_bound_fix_cs = &f;
                    break;
                }
        }
    }
    o_cs_set_shader(ctx, cs, inst, n);
}

static void STDMETHODCALLTYPE hk_om_set_render_targets(ID3D11DeviceContext* ctx, UINT n,
                                                      ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!t_inside) {
        velocity_mask::on_set_targets(ctx, n, rtvs, dsv, o_draw);
        canary::on_set_targets(n, rtvs, dsv);
    }
    o_om_set_render_targets(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE hk_om_set_targets_and_uavs(ID3D11DeviceContext* ctx, UINT n,
                                                         ID3D11RenderTargetView* const* rtvs,
                                                         ID3D11DepthStencilView* dsv, UINT uav_start, UINT nuav,
                                                         ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (!t_inside) {
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            canary::on_set_targets(n, rtvs, dsv);
        if (nuav != D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
            canary::on_set_uavs(nuav, uavs);
    }
    o_om_set_targets_and_uavs(ctx, n, rtvs, dsv, uav_start, nuav, uavs, counts);
}

// Keep replacements scoped across every return in Draw and all indexed/instanced paths.
// ScopedInside keeps our temporary PS bindings out of the engine binding cache.
class SubrectDraw {
  public:
    explicit SubrectDraw(ID3D11DeviceContext* ctx) : ctx_(ctx), active_(!t_inside) {
        if (!active_)
            return;
        water_ = g_bound_subrect.kind == subrect_fixes::Kind::WaterReflection;
        bloom_ = g_bound_subrect.kind == subrect_fixes::Kind::Bloom && g_bound_bloom_vs;
        ScopedInside inside;
        state_ = subrect_fixes::before_draw(ctx, g_bound_subrect, g_bound_ps);
    }
    ~SubrectDraw() {
        if (!active_)
            return;
        ScopedInside inside;
        subrect_fixes::after_draw(ctx_, state_);
        if (bloom_)
            canary::preserve_full_target(ctx_);
        if (water_)
            canary::record_bottom_target(ctx_);
    }
    SubrectDraw(const SubrectDraw&) = delete;
    SubrectDraw& operator=(const SubrectDraw&) = delete;

  private:
    ID3D11DeviceContext* ctx_;
    bool active_, bloom_ = false, water_ = false;
    subrect_fixes::DrawState state_;
};

// Pad inputs before the engine draw, including draws with no other renderer fix.
static void canary_before_draw(ID3D11DeviceContext* ctx) {
    if (t_inside)
        return;
    t_inside = true;
    uint32_t ps_slots = g_ps_slots;
    // DLSS consumes the reflected TAA inputs even if this engine variant does not.
    if (g_bound_taa)
        for (UINT slot : {g_bound_taa->slot_colour, g_bound_taa->slot_depth, g_bound_taa->slot_velocity})
            if (slot < 32) ps_slots |= uint32_t(1) << slot;
    canary::before_draw(ctx, o_draw, g_vs_slots, ps_slots);
    t_inside = false;
}

static void STDMETHODCALLTYPE hk_draw_indexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base) {
    canary_before_draw(ctx);
    SubrectDraw subrect(ctx);
    o_draw_indexed(ctx, count, start, base);
}

static void STDMETHODCALLTYPE hk_draw_indexed_instanced(ID3D11DeviceContext* ctx, UINT count, UINT instances,
                                                        UINT start, INT base, UINT start_instance) {
    canary_before_draw(ctx);
    SubrectDraw subrect(ctx);
    o_draw_indexed_instanced(ctx, count, instances, start, base, start_instance);
}

static void STDMETHODCALLTYPE hk_draw_instanced(ID3D11DeviceContext* ctx, UINT count, UINT instances, UINT start,
                                                UINT start_instance) {
    canary_before_draw(ctx);
    SubrectDraw subrect(ctx);
    o_draw_instanced(ctx, count, instances, start, start_instance);
}

static void STDMETHODCALLTYPE hk_dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    if (t_inside) {
        o_dispatch(ctx, x, y, z);
        return;
    }
    t_inside = true;
    canary::before_dispatch(ctx, o_draw, g_cs_slots);
    if (g_bound_fix_cs)
        screen_size_fix::dispatch(ctx, *g_bound_fix_cs, x, y, z, o_dispatch);
    else
        o_dispatch(ctx, x, y, z);
    canary::after_dispatch(ctx);
    t_inside = false;
}

static void STDMETHODCALLTYPE hk_vs_set_shader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                                               ID3D11ClassInstance* const* inst, UINT n) {
    if ((vs != g_bound_vs || n != g_vs_classes) && !t_inside) {
        g_bound_vs = vs;
        g_vs_classes = n;
        g_vs_slots = resource_slots::get(vs, n);
        g_bound_bloom_vs = subrect_fixes::is_bloom_vs(vs);
        g_bound_fix_vs = nullptr;
        if (vs) {
            std::lock_guard lock(g_mutex);
            for (const screen_size_fix::Shader& f : g_fix_vs)
                if (f.shader == vs) {
                    g_bound_fix_vs = &f;
                    break;
                }
        }
    }
    o_vs_set_shader(ctx, vs, inst, n);
}

static void STDMETHODCALLTYPE hk_ps_set_shader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                                               ID3D11ClassInstance* const* inst, UINT n) {
    if ((ps != g_bound_ps || n != g_ps_classes) && !t_inside) {
        g_bound_ps = ps;
        g_ps_classes = n;
        g_ps_slots = resource_slots::get(ps, n);
        g_bound_subrect = subrect_fixes::find_ps(ps);
        g_bound_taa = nullptr;
        g_bound_fix = nullptr;
        if (ps) {
            std::lock_guard lock(g_mutex);
            for (const TaaShader& t : g_taa)
                if (t.shader == ps) {
                    g_bound_taa = &t;
                    break;
                }
            for (const screen_size_fix::Shader& f : g_fix)
                if (f.shader == ps) {
                    g_bound_fix = &f;
                    break;
                }
        }
    }
    o_ps_set_shader(ctx, ps, inst, n);
}

static void STDMETHODCALLTYPE hk_ps_set_samplers(ID3D11DeviceContext* ctx, UINT start, UINT n,
                                                  ID3D11SamplerState* const* samplers) {
    ID3D11SamplerState* biased[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    if (!t_inside && mip_bias::remap(ctx, n, samplers, biased))
        return o_ps_set_samplers(ctx, start, n, biased);
    o_ps_set_samplers(ctx, start, n, samplers);
}

static void STDMETHODCALLTYPE hk_draw(ID3D11DeviceContext* ctx, UINT count, UINT start) {
    canary_before_draw(ctx);  // also the TAA draw's inputs (our DLSS path reads them too)
    SubrectDraw subrect(ctx);
    if (g_bound_taa && !t_inside)
        canary::on_taa_draw();
    if (g_bound_taa && !t_inside) {
        ++g_status.taa_draws;
        g_last_taa = g_bound_taa;
        t_inside = true;
        bool replaced = taa_dlss::on_taa_draw(ctx, *g_bound_taa);
        t_inside = false;
        if (replaced) {
            ++g_status.taa_replaced;
            return;
        }
    }
    if (!t_inside && game::inside_engine_region_tri()) {
        t_inside = true;
        bool drawn = screen_size_fix::draw_dof(ctx, g_bound_fix, g_bound_fix_vs, count, start, o_draw);
        t_inside = false;
        if (drawn)
            return;
    }
    if (!t_inside) {
        switch (screen_size_fix::scaled_tri_action(ctx, g_bound_fix_vs)) {
        case screen_size_fix::TriAction::Redraw:
            game::redo_fullscreen_tri_unscaled();
            return;
        case screen_size_fix::TriAction::Shrink: {
            D3D11_VIEWPORT vp;
            UINT n = 1;
            ctx->RSGetViewports(&n, &vp);
            game::Vec2 s = game::current_viewport_scale();
            D3D11_VIEWPORT sub = vp;
            sub.Width = floorf(vp.Width * s.x + 0.5f);
            sub.Height = floorf(vp.Height * s.y + 0.5f);
            ctx->RSSetViewports(1, &sub);
            if (g_bound_fix) {
                t_inside = true;
                screen_size_fix::draw(ctx, *g_bound_fix, g_bound_fix_vs, count, start, o_draw);
                t_inside = false;
            } else {
                o_draw(ctx, count, start);
            }
            ctx->RSSetViewports(1, &vp);
            return;
        }
        case screen_size_fix::TriAction::Draw:
            break;
        }
    }
    if (!g_bound_fix && g_bound_fix_vs && !t_inside) {
        t_inside = true;
        bool drawn = screen_size_fix::draw_bloom_source(ctx, *g_bound_fix_vs, count, start, o_draw);
        t_inside = false;
        if (drawn)
            return;
    }
    if (g_bound_fix && !t_inside) {
        t_inside = true;
        screen_size_fix::draw(ctx, *g_bound_fix, g_bound_fix_vs, count, start, o_draw);
        t_inside = false;
        return;
    }
    o_draw(ctx, count, start);
}

ScopedInside::ScopedInside() { t_inside = true; }
ScopedInside::~ScopedInside() { t_inside = false; }

bool install() {
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDevice"));
    if (!create)
        return false;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    // Same flags as the game (0), so the implementation behind the vtables is the one the game gets.
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                        nullptr, &ctx);
    if (FAILED(hr)) {
        logf("d3d11: dummy device failed 0x%08lx", hr);
        return false;
    }
    void** dvt = *reinterpret_cast<void***>(device);
    void** cvt = *reinterpret_cast<void***>(ctx);
    MH_STATUS s1 = MH_CreateHook(dvt[kCreatePixelShaderSlot], &hk_create_pixel_shader,
                                 reinterpret_cast<void**>(&o_create_pixel_shader));
    MH_STATUS s2 = MH_CreateHook(cvt[kPSSetShaderSlot], &hk_ps_set_shader, reinterpret_cast<void**>(&o_ps_set_shader));
    MH_STATUS s3 = MH_CreateHook(cvt[kDrawSlot], &hk_draw, reinterpret_cast<void**>(&o_draw));
    MH_STATUS s5 = MH_CreateHook(dvt[kCreateVertexShaderSlot], &hk_create_vertex_shader,
                                 reinterpret_cast<void**>(&o_create_vertex_shader));
    MH_STATUS s6 = MH_CreateHook(cvt[kVSSetShaderSlot], &hk_vs_set_shader, reinterpret_cast<void**>(&o_vs_set_shader));
    MH_STATUS s7 = MH_CreateHook(cvt[kPSSetSamplersSlot], &hk_ps_set_samplers,
                                 reinterpret_cast<void**>(&o_ps_set_samplers));
    MH_STATUS s8 = MH_CreateHook(dvt[kCreateComputeShaderSlot], &hk_create_compute_shader,
                                 reinterpret_cast<void**>(&o_create_compute_shader));
    MH_STATUS s9 = MH_CreateHook(cvt[kCSSetShaderSlot], &hk_cs_set_shader, reinterpret_cast<void**>(&o_cs_set_shader));
    MH_STATUS s10 = MH_CreateHook(cvt[kDispatchSlot], &hk_dispatch, reinterpret_cast<void**>(&o_dispatch));
    MH_STATUS s11 = MH_CreateHook(dvt[kCreateTexture2DSlot], &hk_create_texture2d,
                                  reinterpret_cast<void**>(&o_create_texture2d));
    MH_STATUS s12 = MH_CreateHook(dvt[kCreateSRVSlot], &hk_create_srv, reinterpret_cast<void**>(&o_create_srv));
    MH_STATUS s13 = MH_CreateHook(dvt[kCreateRTVSlot], &hk_create_rtv, reinterpret_cast<void**>(&o_create_rtv));
    MH_STATUS s14 = MH_CreateHook(cvt[kOMSetRenderTargetsSlot], &hk_om_set_render_targets,
                                  reinterpret_cast<void**>(&o_om_set_render_targets));
    MH_STATUS s15 = MH_CreateHook(cvt[kDrawIndexedSlot], &hk_draw_indexed, reinterpret_cast<void**>(&o_draw_indexed));
    MH_STATUS s16 = MH_CreateHook(cvt[kDrawIndexedInstancedSlot], &hk_draw_indexed_instanced,
                                  reinterpret_cast<void**>(&o_draw_indexed_instanced));
    MH_STATUS s17 = MH_CreateHook(cvt[kDrawInstancedSlot], &hk_draw_instanced,
                                  reinterpret_cast<void**>(&o_draw_instanced));
    MH_STATUS s18 = MH_CreateHook(cvt[kOMSetTargetsAndUavsSlot], &hk_om_set_targets_and_uavs,
                                  reinterpret_cast<void**>(&o_om_set_targets_and_uavs));
    MH_STATUS s4 = MH_EnableHook(MH_ALL_HOOKS);
    ctx->Release();
    device->Release();
    logf("d3d11: hooks CreatePixelShader %s, PSSetShader %s, Draw %s, CreateVertexShader %s, VSSetShader %s, "
         "PSSetSamplers %s, CreateComputeShader %s, CSSetShader %s, Dispatch %s, CreateTexture2D %s, "
         "CreateShaderResourceView %s, CreateRenderTargetView %s, OMSetRenderTargets %s, DrawIndexed %s, "
         "DrawIndexedInstanced %s, DrawInstanced %s, OMSetRenderTargetsAndUnorderedAccessViews %s, enable %s",
         MH_StatusToString(s1),
         MH_StatusToString(s2), MH_StatusToString(s3), MH_StatusToString(s5), MH_StatusToString(s6),
         MH_StatusToString(s7), MH_StatusToString(s8), MH_StatusToString(s9), MH_StatusToString(s10),
         MH_StatusToString(s11), MH_StatusToString(s12), MH_StatusToString(s13), MH_StatusToString(s14), MH_StatusToString(s15),
         MH_StatusToString(s16), MH_StatusToString(s17), MH_StatusToString(s18), MH_StatusToString(s4));
    g_status.installed = s1 == MH_OK && s2 == MH_OK && s3 == MH_OK && s4 == MH_OK && s5 == MH_OK && s6 == MH_OK &&
                         s7 == MH_OK && s8 == MH_OK && s9 == MH_OK && s10 == MH_OK && s11 == MH_OK && s12 == MH_OK &&
                         s13 == MH_OK && s14 == MH_OK && s15 == MH_OK && s16 == MH_OK && s17 == MH_OK &&
                         s18 == MH_OK;
    return g_status.installed;
}

}  // namespace kc::d3d11
