#pragma once
#include "assets.h"
#include "engine.h"
#include <objbase.h>
#include <d3d10.h>

namespace potato {

constexpr int SCREEN_W = 1366;
constexpr int SCREEN_H = 768;

struct RenderContext {
    ID3D10Device* device = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D10RenderTargetView* rtv = nullptr;
    ID3D10Texture2D* depthTex = nullptr;
    ID3D10DepthStencilView* dsv = nullptr;
    ID3D10VertexShader* vs = nullptr;
    ID3D10PixelShader* psWorld = nullptr;
    ID3D10PixelShader* psUI = nullptr;
    ID3D10PixelShader* psCrack = nullptr;
    ID3D10InputLayout* layout = nullptr;
    ID3D10Buffer* perFrameCB = nullptr;
    ID3D10SamplerState* pointSampler = nullptr;
    ID3D10SamplerState* linearSampler = nullptr; // panorama soft filtering
    ID3D10BlendState* alphaBlend = nullptr;
    ID3D10BlendState* crackBlend = nullptr; // multiplicative: DST_COLOR, SRC_COLOR
    ID3D10DepthStencilState* depthOn = nullptr;
    ID3D10DepthStencilState* depthOff = nullptr;
    ID3D10DepthStencilState* depthReadOnly = nullptr; // particles: test on, write off
    ID3D10RasterizerState* rasterizer = nullptr;
    ID3D10ShaderResourceView* atlasSRV = nullptr;
    // Title-screen panorama cubemap (optional; null falls back to dirt).
    ID3D10PixelShader* psSky = nullptr;
    ID3D10ShaderResourceView* panoramaSRV = nullptr;
    ID3D10Buffer* skyVB = nullptr;
    ID3D10PixelShader* psWorldSky = nullptr;
    ID3D10PixelShader* psCelestial = nullptr;
    ID3D10PixelShader* psEntity = nullptr;
    ID3D10BlendState* additiveBlend = nullptr;
    ID3D10Buffer* starsVB = nullptr;
    UINT starVertexCount = 0;
    ID3D10Buffer* celestialVB = nullptr;
    ID3D10ShaderResourceView* sunSRV = nullptr;
    ID3D10ShaderResourceView* moonSRV[8] = {};
};

bool InitRenderer(HWND hwnd, RenderContext& rc, const char* shaderSource);
void ShutdownRenderer(RenderContext& rc);
ID3D10ShaderResourceView* CreateTextureSRV(RenderContext& rc, const uint8_t* rgba, int w, int h);
ID3D10ShaderResourceView* CreateBlockAtlasSRV(RenderContext& rc, const uint8_t* rgba, int w, int h);
// Loads gui/title/background/panorama_0..5 into a TextureCube. False if missing.
bool CreatePanorama(RenderContext& rc, AssetStore& store);
bool CreateCelestials(RenderContext& rc, AssetStore& store);
void SetViewProj(RenderContext& rc, const DirectX::XMMATRIX& m, float fogEnd = 0.0f,
                 const SkyState* sky = nullptr, float celestialOpacity = 1.0f,
                 float starRotation = 0.0f);
void SetBrightness(RenderContext& rc, float b);

} // namespace potato