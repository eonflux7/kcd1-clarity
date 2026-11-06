#pragma once
#include <d3d11.h>

#include <algorithm>

namespace kc {

// Only for ClampFill's fullscreen draw, not for NGX or arbitrary internal passes.
// Keep resource bindings from every stage: binding the temporary RTV can silently
// unbind conflicting SRVs/UAVs even from stages the padding shader does not use.
// Plain Get/Set calls preserve the driver's NVAPI depth-bounds state.
class PaddingStateBackup {
public:
    explicit PaddingStateBackup(ID3D11DeviceContext* ctx, ID3D11Device* device = nullptr) : ctx_(ctx) {
        ID3D11Device* owned = nullptr;
        if (!device) {
            ctx->GetDevice(&owned);
            device = owned;
        }
        uav_slots_ = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT
                                                                         : D3D11_PS_CS_UAV_REGISTER_COUNT;
        if (owned) owned->Release();

        ctx->VSGetShader(&vs_.shader, vs_.inst, &vs_.count);
        ctx->PSGetShader(&ps_.shader, ps_.inst, &ps_.count);
        ctx->GSGetShader(&gs_.shader, gs_.inst, &gs_.count);
        ctx->HSGetShader(&hs_.shader, hs_.inst, &hs_.count);
        ctx->DSGetShader(&ds_.shader, ds_.inst, &ds_.count);
        ctx->VSGetShaderResources(0, kSrv, srv_[0]);
        ctx->PSGetShaderResources(0, kSrv, srv_[1]);
        ctx->GSGetShaderResources(0, kSrv, srv_[2]);
        ctx->HSGetShaderResources(0, kSrv, srv_[3]);
        ctx->DSGetShaderResources(0, kSrv, srv_[4]);
        ctx->CSGetShaderResources(0, kSrv, srv_[5]);
        for (UINT i = 0; i < 6; ++i) srv_count_[i] = bound_count(srv_[i], kSrv);
        ctx->PSGetConstantBuffers(0, 1, &ps_cb_);
        ctx->CSGetUnorderedAccessViews(0, uav_slots_, cs_uav_);
        cs_count_ = bound_count(cs_uav_, uav_slots_);
        ctx->OMGetRenderTargets(kRt, rtv_, &dsv_);
        rt_count_ = bound_count(rtv_, kRt);
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uav_slots_, om_uav_);
        om_count_ = bound_count(om_uav_, uav_slots_);
        ctx->IAGetInputLayout(&layout_);
        ctx->IAGetPrimitiveTopology(&topology_);
        ctx->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx->OMGetDepthStencilState(&depth_, &stencil_ref_);
        ctx->RSGetState(&raster_);
        ctx->RSGetViewports(&nvp_, vp_);
        ctx->RSGetScissorRects(&nscissor_, scissor_);
    }

    void unbind_inputs() const {
        ID3D11ShaderResourceView* nulls[kSrv] = {};
        if (srv_count_[0]) ctx_->VSSetShaderResources(0, srv_count_[0], nulls);
        if (srv_count_[1]) ctx_->PSSetShaderResources(0, srv_count_[1], nulls);
        if (srv_count_[2]) ctx_->GSSetShaderResources(0, srv_count_[2], nulls);
        if (srv_count_[3]) ctx_->HSSetShaderResources(0, srv_count_[3], nulls);
        if (srv_count_[4]) ctx_->DSSetShaderResources(0, srv_count_[4], nulls);
        if (srv_count_[5]) ctx_->CSSetShaderResources(0, srv_count_[5], nulls);
    }

    ~PaddingStateBackup() {
        // Drop the padding RTV before restoring any engine read bindings.
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        UINT keep[D3D11_1_UAV_SLOT_COUNT];
        std::fill_n(keep, uav_slots_, ~0u);
        if (om_count_ > rt_count_)
            ctx_->OMSetRenderTargetsAndUnorderedAccessViews(rt_count_, rtv_, dsv_, rt_count_,
                                                            om_count_ - rt_count_, om_uav_ + rt_count_, keep);
        else
            ctx_->OMSetRenderTargets(rt_count_, rtv_, dsv_);
        if (cs_count_) ctx_->CSSetUnorderedAccessViews(0, cs_count_, cs_uav_, keep);
        if (srv_count_[0]) ctx_->VSSetShaderResources(0, srv_count_[0], srv_[0]);
        // Padding installs t0 and t1 even if both engine slots were null.
        ctx_->PSSetShaderResources(0, std::max(2u, srv_count_[1]), srv_[1]);
        if (srv_count_[2]) ctx_->GSSetShaderResources(0, srv_count_[2], srv_[2]);
        if (srv_count_[3]) ctx_->HSSetShaderResources(0, srv_count_[3], srv_[3]);
        if (srv_count_[4]) ctx_->DSSetShaderResources(0, srv_count_[4], srv_[4]);
        if (srv_count_[5]) ctx_->CSSetShaderResources(0, srv_count_[5], srv_[5]);
        ctx_->VSSetShader(vs_.shader, vs_.inst, vs_.count);
        ctx_->PSSetShader(ps_.shader, ps_.inst, ps_.count);
        ctx_->GSSetShader(gs_.shader, gs_.inst, gs_.count);
        ctx_->HSSetShader(hs_.shader, hs_.inst, hs_.count);
        ctx_->DSSetShader(ds_.shader, ds_.inst, ds_.count);
        ctx_->PSSetConstantBuffers(0, 1, &ps_cb_);
        ctx_->IASetInputLayout(layout_);
        ctx_->IASetPrimitiveTopology(topology_);
        ctx_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
        ctx_->OMSetDepthStencilState(depth_, stencil_ref_);
        ctx_->RSSetState(raster_);
        ctx_->RSSetViewports(nvp_, vp_);
        ctx_->RSSetScissorRects(nscissor_, scissor_);

        vs_.release();
        ps_.release();
        gs_.release();
        hs_.release();
        ds_.release();
        for (UINT i = 0; i < 6; ++i) release(srv_[i], srv_count_[i]);
        release(cs_uav_, cs_count_);
        release(om_uav_, om_count_);
        release(rtv_, rt_count_);
        drop(ps_cb_);
        drop(dsv_);
        drop(layout_);
        drop(blend_);
        drop(depth_);
        drop(raster_);
    }

    PaddingStateBackup(const PaddingStateBackup&) = delete;
    PaddingStateBackup& operator=(const PaddingStateBackup&) = delete;

private:
    static constexpr UINT kSrv = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    static constexpr UINT kRt = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    static constexpr UINT kVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    template <class T>
    static void drop(T* p) {
        if (p) p->Release();
    }
    template <class T>
    static void release(T** p, UINT n) {
        for (UINT i = 0; i < n; ++i) drop(p[i]);
    }
    template <class T>
    static UINT bound_count(T** p, UINT n) {
        while (n && !p[n - 1]) --n;
        return n;
    }
    template <class T>
    struct Shader {
        T* shader;
        ID3D11ClassInstance* inst[256];
        UINT count = 256;
        void release() {
            drop(shader);
            PaddingStateBackup::release(inst, count);
        }
    };

    ID3D11DeviceContext* ctx_;
    UINT uav_slots_, cs_count_, om_count_, rt_count_, srv_count_[6];
    Shader<ID3D11VertexShader> vs_;
    Shader<ID3D11PixelShader> ps_;
    Shader<ID3D11GeometryShader> gs_;
    Shader<ID3D11HullShader> hs_;
    Shader<ID3D11DomainShader> ds_;
    // Get calls fill these arrays. No need to clear unused class-instance capacity.
    ID3D11ShaderResourceView* srv_[6][kSrv];
    ID3D11UnorderedAccessView* cs_uav_[D3D11_1_UAV_SLOT_COUNT];
    ID3D11UnorderedAccessView* om_uav_[D3D11_1_UAV_SLOT_COUNT];
    ID3D11Buffer* ps_cb_;
    ID3D11InputLayout* layout_;
    D3D11_PRIMITIVE_TOPOLOGY topology_;
    ID3D11RenderTargetView* rtv_[kRt];
    ID3D11DepthStencilView* dsv_;
    ID3D11BlendState* blend_;
    float blend_factor_[4];
    UINT sample_mask_;
    ID3D11DepthStencilState* depth_;
    UINT stencil_ref_;
    ID3D11RasterizerState* raster_;
    D3D11_VIEWPORT vp_[kVp];
    UINT nvp_ = kVp;
    D3D11_RECT scissor_[kVp];
    UINT nscissor_ = kVp;
};

}  // namespace kc
