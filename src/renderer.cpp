#include "renderer.h"
#include "world.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

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
    ID3DBlob* psSkyBlob = nullptr;
    if (!CompileOne(shaderSource, "VSMain", "vs_4_0", &vsBlob, hwnd)) return false;
    if (!CompileOne(shaderSource, "PSMain", "ps_4_0", &psBlob, hwnd)) { vsBlob->Release(); return false; }
    if (!CompileOne(shaderSource, "PSMainUI", "ps_4_0", &psUIBlob, hwnd)) {
        vsBlob->Release(); psBlob->Release(); return false;
    }
    if (!CompileOne(shaderSource, "PSMainSky", "ps_4_0", &psSkyBlob, hwnd)) {
        vsBlob->Release(); psBlob->Release(); psUIBlob->Release(); return false;
    }
    hr = rc.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &rc.vs);
    if (FAILED(hr)) { vsBlob->Release(); psBlob->Release(); psUIBlob->Release(); psSkyBlob->Release(); return false; }
    hr = rc.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), &rc.psWorld);
    psBlob->Release();
    if (FAILED(hr)) { vsBlob->Release(); psUIBlob->Release(); psSkyBlob->Release(); return false; }
    hr = rc.device->CreatePixelShader(psUIBlob->GetBufferPointer(), psUIBlob->GetBufferSize(), &rc.psUI);
    psUIBlob->Release();
    if (FAILED(hr)) { vsBlob->Release(); psSkyBlob->Release(); return false; }
    hr = rc.device->CreatePixelShader(psSkyBlob->GetBufferPointer(), psSkyBlob->GetBufferSize(), &rc.psSky);
    psSkyBlob->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    ID3DBlob* extra = nullptr;
    if (!CompileOne(shaderSource, "PSMainWorldSky", "ps_4_0", &extra, hwnd)) {
        vsBlob->Release(); return false;
    }
    hr = rc.device->CreatePixelShader(extra->GetBufferPointer(), extra->GetBufferSize(), &rc.psWorldSky);
    extra->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }
    if (!CompileOne(shaderSource, "PSMainCelestial", "ps_4_0", &extra, hwnd)) {
        vsBlob->Release(); return false;
    }
    hr = rc.device->CreatePixelShader(extra->GetBufferPointer(), extra->GetBufferSize(), &rc.psCelestial);
    extra->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    if (!CompileOne(shaderSource, "PSMainEntity", "ps_4_0", &extra, hwnd)) {
        vsBlob->Release(); return false;
    }
    hr = rc.device->CreatePixelShader(extra->GetBufferPointer(), extra->GetBufferSize(), &rc.psEntity);
    extra->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    if (!CompileOne(shaderSource, "PSMainCrack", "ps_4_0", &extra, hwnd)) {
        vsBlob->Release(); return false;
    }
    hr = rc.device->CreatePixelShader(extra->GetBufferPointer(), extra->GetBufferSize(), &rc.psCrack);
    extra->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    if (!CompileOne(shaderSource, "PSMainWater", "ps_4_0", &extra, hwnd)) {
        vsBlob->Release(); return false;
    }
    hr = rc.device->CreatePixelShader(extra->GetBufferPointer(), extra->GetBufferSize(), &rc.psWater);
    extra->Release();
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    const D3D10_INPUT_ELEMENT_DESC elems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,       0, 28, D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D10_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = rc.device->CreateInputLayout(elems, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      &rc.layout);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    D3D10_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMFLOAT4X4) + 112;
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
    // Linear filtering for the title panorama: vanilla renders it soft/blurred.
    samp.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    hr = rc.device->CreateSamplerState(&samp, &rc.linearSampler);
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
    bd.DestBlend = D3D10_BLEND_ONE;
    hr = rc.device->CreateBlendState(&bd, &rc.additiveBlend);
    if (FAILED(hr)) return false;
    // Java crumbling overlay: multiplicative RGB (src*dst + dst*src) darkens
    // the lit block beneath the cracks; alpha is replaced by the source alpha.
    bd.SrcBlend = D3D10_BLEND_DEST_COLOR;
    bd.DestBlend = D3D10_BLEND_SRC_COLOR;
    bd.SrcBlendAlpha = D3D10_BLEND_ONE;
    bd.DestBlendAlpha = D3D10_BLEND_ZERO;
    hr = rc.device->CreateBlendState(&bd, &rc.crackBlend);
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
    rc.device->PSSetSamplers(1, 1, &rc.linearSampler);
    rc.device->VSSetConstantBuffers(0, 1, &rc.perFrameCB);
    rc.device->PSSetConstantBuffers(0, 1, &rc.perFrameCB);
    rc.device->RSSetState(rc.rasterizer);

    // Unit cube for the title panorama; PSMainSky samples the cubemap along
    // the interpolated object-space direction, so only positions matter.
    static const float C[8][3] = {
        {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
        {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1},
    };
    static const int F[36] = {
        0,1,2, 0,2,3,  5,4,7, 5,7,6,  4,0,3, 4,3,7,
        1,5,6, 1,6,2,  3,2,6, 3,6,7,  4,5,1, 4,1,0,
    };
    MeshVertex sky[36];
    for (int i = 0; i < 36; ++i) {
        MeshVertex& v = sky[i];
        v.x = C[F[i]][0]; v.y = C[F[i]][1]; v.z = C[F[i]][2];
        v.tu = v.tv = v.u = v.v = 0; v.light = 15;
        v.cr = v.cg = v.cb = 1;
    }
    D3D10_BUFFER_DESC sbd = {};
    sbd.ByteWidth = UINT(sizeof(sky));
    sbd.Usage = D3D10_USAGE_IMMUTABLE;
    sbd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    D3D10_SUBRESOURCE_DATA ssub = {};
    ssub.pSysMem = sky;
    if (FAILED(rc.device->CreateBuffer(&sbd, &ssub, &rc.skyVB))) return false;

    std::vector<MeshVertex> stars;
    stars.reserve(1500 * 6);
    uint32_t random = 10842;
    auto next = [&]() {
        random = random * 1664525u + 1013904223u;
        return float(random >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 1500; ++i) {
        const float y = next() * 2.0f - 1.0f;
        const float azimuth = next() * XM_2PI;
        const float radius = sqrtf(1.0f - y * y);
        const XMVECTOR direction = XMVectorSet(radius * cosf(azimuth), y, radius * sinf(azimuth), 0);
        const XMVECTOR reference = fabsf(y) > 0.9f ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(reference, direction));
        const XMVECTOR up = XMVector3Cross(direction, right);
        const float size = 0.15f + next() * 0.1f;
        const float rotation = next() * XM_2PI;
        const XMVECTOR r = XMVectorScale(XMVectorAdd(XMVectorScale(right, cosf(rotation)),
            XMVectorScale(up, sinf(rotation))), size);
        const XMVECTOR u = XMVectorScale(XMVectorSubtract(XMVectorScale(up, cosf(rotation)),
            XMVectorScale(right, sinf(rotation))), size);
        const XMVECTOR center = XMVectorScale(direction, 100.0f);
        const int corners[6] = {0, 1, 2, 0, 2, 3};
        const float uv[4][2] = {{0,0}, {1,0}, {1,1}, {0,1}};
        for (int corner : corners) {
            const XMVECTOR p = XMVectorAdd(center, XMVectorAdd(
                XMVectorScale(r, corner == 0 || corner == 3 ? -1.0f : 1.0f),
                XMVectorScale(u, corner < 2 ? 1.0f : -1.0f)));
            XMFLOAT3 position;
            XMStoreFloat3(&position, p);
            stars.push_back({position.x, position.y, position.z, 0, 0,
                uv[corner][0], uv[corner][1], 15, 1, 1, 1});
        }
    }
    rc.starVertexCount = UINT(stars.size());
    sbd.ByteWidth = UINT(stars.size() * sizeof(MeshVertex));
    ssub.pSysMem = stars.data();
    if (FAILED(rc.device->CreateBuffer(&sbd, &ssub, &rc.starsVB))) return false;
    sbd.ByteWidth = 6 * UINT(sizeof(MeshVertex));
    sbd.Usage = D3D10_USAGE_DYNAMIC;
    sbd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    if (FAILED(rc.device->CreateBuffer(&sbd, nullptr, &rc.celestialVB))) return false;
    return true;
}

void ShutdownRenderer(RenderContext& rc) {
    for (auto& moon : rc.moonSRV) SafeRelease(moon);
    SafeRelease(rc.sunSRV);
    SafeRelease(rc.celestialVB);
    SafeRelease(rc.starsVB);
    SafeRelease(rc.additiveBlend);
    SafeRelease(rc.psCelestial);
    SafeRelease(rc.psEntity);
    SafeRelease(rc.psCrack);
    SafeRelease(rc.psWater);
    SafeRelease(rc.psWorldSky);
    SafeRelease(rc.skyVB);
    SafeRelease(rc.panoramaSRV);
    SafeRelease(rc.psSky);
    SafeRelease(rc.atlasSRV);
    SafeRelease(rc.rasterizer);
    SafeRelease(rc.depthReadOnly);
    SafeRelease(rc.depthOff);
    SafeRelease(rc.depthOn);
    SafeRelease(rc.alphaBlend);
    SafeRelease(rc.crackBlend);
    SafeRelease(rc.linearSampler);
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

ID3D10ShaderResourceView* CreateBlockAtlasSRV(RenderContext& rc, const uint8_t* rgba, int w, int h) {
    if (!rgba || w != ATLAS_SIZE || h != ATLAS_SIZE) return nullptr;
    constexpr int mipCount = 5;
    std::vector<uint8_t> mips[mipCount];
    D3D10_SUBRESOURCE_DATA sub[mipCount] = {};
    sub[0].pSysMem = rgba;
    sub[0].SysMemPitch = UINT(w * 4);
    for (int level = 1; level < mipCount; ++level) {
        const int width = w >> level, height = h >> level;
        const int sourceWidth = w >> (level - 1);
        const uint8_t* source = level == 1 ? rgba : mips[level - 1].data();
        mips[level].resize(size_t(width) * height * 4);
        // Aligned 2x2 footprints stay inside each tile, ending at one texel per tile.
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t src = (size_t(y * 2) * sourceWidth + x * 2) * 4;
                const size_t dst = (size_t(y) * width + x) * 4;
                for (int channel = 0; channel < 4; ++channel) {
                    const unsigned sum = source[src + channel] + source[src + 4 + channel] +
                        source[src + size_t(sourceWidth) * 4 + channel] +
                        source[src + size_t(sourceWidth) * 4 + 4 + channel];
                    mips[level][dst + channel] = uint8_t((sum + 2) / 4);
                }
            }
        }
        sub[level].pSysMem = mips[level].data();
        sub[level].SysMemPitch = UINT(width * 4);
    }
    D3D10_TEXTURE2D_DESC td = {};
    td.Width = UINT(w);
    td.Height = UINT(h);
    td.MipLevels = mipCount;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_IMMUTABLE;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    ID3D10Texture2D* tex = nullptr;
    if (FAILED(rc.device->CreateTexture2D(&td, sub, &tex))) return nullptr;
    ID3D10ShaderResourceView* srv = nullptr;
    const HRESULT hr = rc.device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return SUCCEEDED(hr) ? srv : nullptr;
}

bool CreatePanorama(RenderContext& rc, AssetStore& store) {
    // D3D10 cube array order is +X,-X,+Y,-Y,+Z,-Z. Vanilla's PanoramaRenderer
    // rotations map panorama_0..5 onto -X,+X,-Y,+Y,+Z,-Z respectively.
    static const int FACE_SRC[6] = {1, 0, 3, 2, 4, 5};
    std::vector<uint8_t> faces[6];
    int size = 0;
    for (int i = 0; i < 6; ++i) {
        char name[96];
        snprintf(name, sizeof(name),
                 "minecraft/textures/gui/title/background/panorama_%d.png", FACE_SRC[i]);
        std::vector<uint8_t> bytes, px;
        int w = 0, h = 0;
        if (!store.ReadFile(name, bytes) || !DecodePNGAny(bytes, px, w, h) || w != h || w == 0)
            return false;
        if (size == 0) size = w;
        if (w != size) {
            std::vector<uint8_t> rs(size_t(size) * size * 4);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    memcpy(&rs[(size_t(y) * size + x) * 4],
                           &px[(size_t(y * h / size) * w + x * w / size) * 4], 4);
            px.swap(rs);
        }
        faces[i] = std::move(px);
    }
    D3D10_TEXTURE2D_DESC td = {};
    td.Width = td.Height = UINT(size);
    td.MipLevels = 1;
    td.ArraySize = 6;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_IMMUTABLE;
    td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D10_RESOURCE_MISC_TEXTURECUBE;
    D3D10_SUBRESOURCE_DATA sub[6] = {};
    for (int i = 0; i < 6; ++i) {
        sub[i].pSysMem = faces[i].data();
        sub[i].SysMemPitch = UINT(size * 4);
    }
    ID3D10Texture2D* tex = nullptr;
    if (FAILED(rc.device->CreateTexture2D(&td, sub, &tex))) return false;
    D3D10_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = td.Format;
    sv.ViewDimension = D3D10_SRV_DIMENSION_TEXTURECUBE;
    sv.TextureCube.MostDetailedMip = 0;
    sv.TextureCube.MipLevels = 1;
    const HRESULT hr = rc.device->CreateShaderResourceView(tex, &sv, &rc.panoramaSRV);
    tex->Release();
    return SUCCEEDED(hr);
}

bool CreateCelestials(RenderContext& rc, AssetStore& store) {
    auto load = [&](const std::string& path) {
        std::vector<uint8_t> bytes, pixels;
        int w = 0, h = 0;
        if (!store.ReadFile(path, bytes) || !DecodePNGAny(bytes, pixels, w, h))
            return static_cast<ID3D10ShaderResourceView*>(nullptr);
        return CreateTextureSRV(rc, pixels.data(), w, h);
    };
    const std::string root = "minecraft/textures/environment/celestial/";
    rc.sunSRV = load(root + "sun.png");
    const char* phases[8] = {"full_moon", "waning_gibbous", "third_quarter", "waning_crescent",
        "new_moon", "waxing_crescent", "first_quarter", "waxing_gibbous"};
    bool loaded = rc.sunSRV != nullptr;
    for (int i = 0; i < 8; ++i) {
        rc.moonSRV[i] = load(root + "moon/" + phases[i] + ".png");
        loaded = loaded && rc.moonSRV[i] != nullptr;
    }
    return loaded;
}

static float gBrightnessVal = 0.5f;

void SetBrightness(RenderContext& rc, float b) { gBrightnessVal = b; }

void SetViewProj(RenderContext& rc, const XMMATRIX& m, float fogEnd,
                 const SkyState* sky, float celestialOpacity, float starRotation,
                 float waterTime) {
    void* data = nullptr;
    if (SUCCEEDED(rc.perFrameCB->Map(D3D10_MAP_WRITE_DISCARD, 0, &data))) {
        struct PerFrame {
            XMFLOAT4X4 vp;
            float brightness, fogStart, fogEnd, starRotation;
            XMFLOAT4 skyColorDaylight, fogColorStars, lightColorOpacity, twilight, sunDirection;
            float waterTime;
            float waterPad[3]; // D3D10 cbuffer size must be a multiple of 16 bytes
        };
        static_assert(sizeof(PerFrame) == 176, "Shader constant-buffer layout");
        PerFrame* pf = static_cast<PerFrame*>(data);
        const SkyState state = sky ? *sky : SkyState{};
        XMStoreFloat4x4(&pf->vp, m);
        pf->brightness = gBrightnessVal;
        pf->fogStart = 0.65f * fogEnd;
        pf->fogEnd = 0.9f * fogEnd;
        pf->starRotation = starRotation;
        pf->skyColorDaylight = {state.skyColor.x, state.skyColor.y, state.skyColor.z, state.daylight};
        pf->fogColorStars = {state.fogColor.x, state.fogColor.y, state.fogColor.z, state.stars};
        pf->lightColorOpacity = {state.lightColor.x, state.lightColor.y, state.lightColor.z, celestialOpacity};
        pf->twilight = state.twilight;
        pf->sunDirection = {-sinf(state.sunAngle), cosf(state.sunAngle), 0, 0};
        pf->waterTime = waterTime;
        rc.perFrameCB->Unmap();
    }
}

} // namespace potato