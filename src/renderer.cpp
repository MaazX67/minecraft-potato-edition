#include "renderer.h"
#include <d3dcompiler.h>
#include <cstring>

namespace potato {

using namespace DirectX;

template <typename T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static bool CompileOne(const char* src, const char* entry, const char* profile,
                       ID3DBlob** out, HWND hwnd) {
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(src, strlen(src), "shader.fx", nullptr, nullptr,
                                  entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
            MessageBoxA(hwnd, static_cast<const char*>(errors->GetBufferPointer()),
                        "Shader compile failed", MB_ICONERROR);
            errors->Release();
        }
        return false;
    }
    if (errors) errors->Release();
    return true;
}

bool InitRenderer(HWND hwnd, RenderContext& rc, const char* shaderSource) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = SCREEN_W;
    sd.BufferDesc.Height = SCREEN_H;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // vsync off: Present(0, 0)

    HRESULT hr = D3D10CreateDeviceAndSwapChain(
        nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_SDK_VERSION,
        &sd, &rc.swapChain, &rc.device);
    if (FAILED(hr)) {
        MessageBoxA(hwnd, "D3D10 hardware device creation failed (feature level 10_0 required).",
                    "Potato Edition", MB_ICONERROR);
        return false;
    }

    ID3D10Texture2D* backBuffer = nullptr;
    hr = rc.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    hr = rc.device->CreateRenderTargetView(backBuffer, nullptr, &rc.rtv);
    backBuffer->Release();
    if (FAILED(hr)) return false;

    D3D10_TEXTURE2D_DESC dd = {};
    dd.Width = SCREEN_W;
    dd.Height = SCREEN_H;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D10_USAGE_DEFAULT;
    dd.BindFlags = D3D10_BIND_DEPTH_STENCIL;
    hr = rc.device->CreateTexture2D(&dd, nullptr, &rc.depthTex);
    if (FAILED(hr)) return false;
    hr = rc.device->CreateDepthStencilView(rc.depthTex, nullptr, &rc.dsv);
    if (FAILED(hr)) return false;

    rc.device->OMSetRenderTargets(1, &rc.rtv, rc.dsv);
    const D3D10_VIEWPORT vp = {0, 0, SCREEN_W, SCREEN_H, 0.0f, 1.0f};
    rc.device->RSSetViewports(1, &vp);

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* psUIBlob = nullptr;
    if (!CompileOne(shaderSource, "VSMain", "vs_4_0", &vsBlob, hwnd)) return false;
    if (!CompileOne(shaderSource, "PSMain", "ps_4_0", &psBlob, hwnd)) { vsBlob->Release(); return false; }
    if (!CompileOne(shaderSource, "PSMainUI", "ps_4_0", &psUIBlob, hwnd)) {
        vsBlob->Release(); psBlob->Release(); return false;
    }
    hr = rc.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &rc.vs);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); psUIBlob->Release(); return false; }
    hr = rc.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), &rc.psWorld);
    psBlob->Release();
    if (FAILED(hr)) { vsBlob->Release(); psUIBlob->Release(); return false; }
    hr = rc.device->CreatePixelShader(psUIBlob->GetBufferPointer(), psUIBlob->GetBufferSize(), &rc.psUI);
    psUIBlob->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    const D3D10_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,       0, 28, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = rc.device->CreateInputLayout(elems, 4, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      &rc.layout);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    D3D10_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4);
    cbd.Usage = D3D10_USAGE_DYNAMIC;
    cbd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    hr = rc.device->CreateBuffer(&cbd, nullptr, &rc.perFrameCB);
    if (FAILED(hr)) return false;

    D3D10_SAMPLER_DESC samp = {};
    samp.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D10_COMPARISON_NEVER;
    samp.MinLOD = 0;
    samp.MaxLOD = D3D10_FLOAT32_MAX;
    hr = rc.device->CreateSamplerState(&samp, &rc.pointSampler);
    if (FAILED(hr)) return false;

    D3D10_BLEND_DESC bd = {};
    bd.BlendEnable[0] = TRUE;
    bd.SrcBlend = D3D10_BLEND_SRC_ALPHA;
    bd.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOp = D3D10_BLEND_OP_ADD;
    bd.SrcBlendAlpha = D3D10_BLEND_ONE;
    bd.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    hr = rc.device->CreateBlendState(&bd, &rc.alphaBlend);
    if (FAILED(hr)) return false;

    D3D10_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D10_COMPARISON_LESS;
    hr = rc.device->CreateDepthStencilState(&dsd, &rc.depthOn);
    if (FAILED(hr)) return false;
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ZERO;
    hr = rc.device->CreateDepthStencilState(&dsd, &rc.depthOff);
    if (FAILED(hr)) return false;
    dsd.DepthEnable = TRUE; // particles: occlude behind terrain, never write depth
    hr = rc.device->CreateDepthStencilState(&dsd, &rc.depthReadOnly);
    if (FAILED(hr)) return false;

    D3D10_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D10_FILL_SOLID;
    rd.CullMode = D3D10_CULL_NONE; // correctness first; greedy meshing keeps tri counts low
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    hr = rc.device->CreateRasterizerState(&rd, &rc.rasterizer);
    if (FAILED(hr)) return false;

    rc.device->IASetInputLayout(rc.layout);
    rc.device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    rc.device->VSSetShader(rc.vs);
    rc.device->PSSetSamplers(0, 1, &rc.pointSampler);
    rc.device->VSSetConstantBuffers(0, 1, &rc.perFrameCB);
    rc.device->RSSetState(rc.rasterizer);
    return true;
}

void ShutdownRenderer(RenderContext& rc) {
    SafeRelease(rc.atlasSRV);
    SafeRelease(rc.rasterizer);
    SafeRelease(rc.depthReadOnly);
    SafeRelease(rc.depthOff);
    SafeRelease(rc.depthOn);
    SafeRelease(rc.alphaBlend);
    SafeRelease(rc.pointSampler);
    SafeRelease(rc.perFrameCB);
    SafeRelease(rc.layout);
    SafeRelease(rc.psUI);
    SafeRelease(rc.psWorld);
    SafeRelease(rc.vs);
    SafeRelease(rc.dsv);
    SafeRelease(rc.depthTex);
    SafeRelease(rc.rtv);
    SafeRelease(rc.swapChain);
    SafeRelease(rc.device);
}

ID3D10ShaderResourceView* CreateTextureSRV(RenderContext& rc, const uint8_t* rgba, int w, int h) {
    D3D10_TEXTURE2D_DESC td = {};
    td.Width = UINT(w);
    td.Height = UINT(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_IMMUTABLE;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    D3D10_SUBRESOURCE_DATA sub = {};
    sub.pSysMem = rgba;
    sub.SysMemPitch = UINT(w * 4);
    ID3D10Texture2D* tex = nullptr;
    if (FAILED(rc.device->CreateTexture2D(&td, &sub, &tex))) return nullptr;
    ID3D10ShaderResourceView* srv = nullptr;
    if (FAILED(rc.device->CreateShaderResourceView(tex, nullptr, &srv))) {
        tex->Release();
        return nullptr;
    }
    tex->Release(); // SRV holds the reference
    return srv;
}

void SetViewProj(RenderContext& rc, const XMMATRIX& m) {
    void* data = nullptr;
    if (SUCCEEDED(rc.perFrameCB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) {
        XMStoreFloat4x4(static_cast<XMFLOAT4X4*>(data), m);
        rc.perFrameCB->Unmap();
    }
}

void ExtractFrustum(const XMMATRIX& vp, Frustum& f) {
    // Row-vector convention (v' = v * M): clip planes from matrix rows.
    const XMVECTOR r0 = vp.r[0], r1 = vp.r[1], r2 = vp.r[2], r3 = vp.r[3];
    XMVECTOR p[6] = {
        XMVectorAdd(r3, r0),        // left
        XMVectorSubtract(r3, r0),   // right
        XMVectorAdd(r3, r1),        // bottom
        XMVectorSubtract(r3, r1),   // top
        r2,                         // near (RH: z' >= 0)
        XMVectorSubtract(r3, r2),   // far
    };
    for (int i = 0; i < 6; ++i) {
        const XMVECTOR len = XMVector3Length(p[i]);
        XMStoreFloat4(&f.planes[i], XMVectorDivide(p[i], len));
    }
}

bool AABBInFrustum(const Frustum& f, float minX, float minY, float minZ,
                   float maxX, float maxY, float maxZ) {
    for (int i = 0; i < 6; ++i) {
        const XMFLOAT4& pl = f.planes[i];
        const float px = pl.x >= 0 ? maxX : minX;
        const float py = pl.y >= 0 ? maxY : minY;
        const float pz = pl.z >= 0 ? maxZ : minZ;
        if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0) return false;
    }
    return true;
}

} // namespace potato