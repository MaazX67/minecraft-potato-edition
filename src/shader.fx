// Minecraft Potato Edition shader core.
// Compiled at boot from the embedded source: VSMain/PSMain (world), PSMainUI
// (menu/HUD), PSMainSky (title panorama cubemap). Profiles: vs_4_0 / ps_4_0 —
// feature level 10_0 hardware only.

cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
};

Texture2D gAtlas : register(t0);
TextureCube gSky : register(t1);
SamplerState gPoint : register(s0);
SamplerState gLinear : register(s1);

struct VSIn
{
    float3 pos   : POSITION;
    float2 tile  : TEXCOORD0;  // atlas tile origin in UV space (world only)
    float2 uv    : TEXCOORD1;  // world: block units across the merged quad; UI: 0..1
    float  light : TEXCOORD2;  // raw 0..15 light level
    float3 color : COLOR;      // vertex tint (white = untinted)
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 tile  : TEXCOORD0;
    float2 uv    : TEXCOORD1;
    float  shade : TEXCOORD2;
    float3 tint  : TEXCOORD3;
    float3 dir   : TEXCOORD4;  // object-space direction (skybox only)
};

PSIn VSMain(VSIn input)
{
    PSIn o;
    o.pos   = mul(float4(input.pos, 1.0f), gViewProj);
    o.tile  = input.tile;
    o.uv    = input.uv;
    o.shade = input.light / 15.0f;
    o.tint  = input.color;
    o.dir   = input.pos;
    return o;
}

// World pass: repeat the 16x16 tile across merged greedy quads.
// frac() gives per-block tiling; +0.5 texel keeps point sampling inside the tile.
// Alpha test handles cutout blocks (leaves/glass); liquid faces keep their
// alpha and are drawn in a second blended pass.
float4 PSMain(PSIn input) : SV_TARGET
{
    float2 atlasUV = input.tile + (frac(input.uv) * 16.0f + 0.5f) / 1024.0f;
    float4 tex = gAtlas.Sample(gPoint, atlasUV);
    clip(tex.a - 0.5f);
    return float4(tex.rgb * input.shade * input.tint, tex.a);
}

// UI pass: plain textured quad (bound texture is a GUI sprite, not the atlas).
// frac() is a no-op for 0..1 sprite UVs and enables the tiled dirt background.
float4 PSMainUI(PSIn input) : SV_TARGET
{
    float4 tex = gAtlas.Sample(gPoint, frac(input.uv));
    return float4(tex.rgb * input.shade * input.tint, tex.a);
}

// Title panorama: sample the cubemap along the interpolated view direction.
float4 PSMainSky(PSIn input) : SV_TARGET
{
    float3 d = normalize(input.dir);
    return float4(gSky.Sample(gLinear, d).rgb, 1.0f);
}