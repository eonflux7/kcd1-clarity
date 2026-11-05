#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace kc {
// Exact WaterVolume caustics PS only: its VS already scales UVs to the valid
// scene rectangle. UV * viewport size scales the integer depth lookup twice.
// Replace only PS_ScreenSize.xy with the depth allocation size. Keep normals'
// normalized UVs, world rays, light projection and all other constants intact.
class CausticsDepthPatch {
public:
    ID3D11Buffer* before_draw(ID3D11DeviceContext* ctx) {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11ShaderResourceView> srv;
        ctx->PSGetShaderResources(2, 1, &srv);
        if (!srv) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        srv->GetDesc(&vd);
        if (vd.Format != DXGI_FORMAT_R32_FLOAT || vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            vd.Texture2D.MostDetailedMip != 0)
            return nullptr;
        ComPtr<ID3D11Resource> res;
        srv->GetResource(&res);
        D3D11_RESOURCE_DIMENSION dim;
        res->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
        D3D11_TEXTURE2D_DESC td;
        static_cast<ID3D11Texture2D*>(res.Get())->GetDesc(&td);
        D3D11_VIEWPORT vp{};
        UINT n = 1;
        ctx->RSGetViewports(&n, &vp);
        if (n != 1 || vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width <= 0 || vp.Height <= 0 ||
            td.SampleDesc.Count != 1 || vp.Width > td.Width || vp.Height > td.Height ||
            (vp.Width >= td.Width && vp.Height >= td.Height))
            return nullptr;
        ComPtr<ID3D11Buffer> old;
        ctx->PSGetConstantBuffers(0, 1, &old);
        if (!old) return nullptr;
        D3D11_BUFFER_DESC bd;
        old->GetDesc(&bd);
        if (bd.ByteWidth < 104) return nullptr;
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        if (device_.Get() != device.Get()) {
            copy_.Reset();
            value_.Reset();
            device_ = device;
        }
        if (!value_) {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 16;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device_->CreateBuffer(&d, nullptr, &value_))) return nullptr;
        }
        if (!copy_ || size_ != bd.ByteWidth) {
            copy_.Reset();
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = bd.ByteWidth;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device_->CreateBuffer(&d, nullptr, &copy_))) return nullptr;
            size_ = bd.ByteWidth;
        }
        float size[4] = {float(td.Width), float(td.Height), 0, 0};
        ctx->UpdateSubresource(value_.Get(), 0, nullptr, size, 0, 0);
        ctx->CopyResource(copy_.Get(), old.Get());
        D3D11_BOX box{0, 0, 0, 8, 1, 1};
        ctx->CopySubresourceRegion(copy_.Get(), 0, 96, 0, 0, value_.Get(), 0, &box);
        ctx->PSSetConstantBuffers(0, 1, copy_.GetAddressOf());
        return old.Detach();
    }
    static void after_draw(ID3D11DeviceContext* ctx, ID3D11Buffer* old) {
        if (!old) return;
        ctx->PSSetConstantBuffers(0, 1, &old);
        old->Release();
    }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> copy_, value_;
    UINT size_ = 0;
};
}  // namespace kc
