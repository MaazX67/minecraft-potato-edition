// Minecraft Potato Edition shader core.
// Compiled at boot from the embedded source: VSMain/PSMain (world), PSMainUI
// (menu/HUD), PSMainSky (title panorama cubemap). Profiles: vs_4_0 / ps_4_0 —
// feature level 10_0 hardware only.

cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
    float gBrightness;
    float gFogStart;
    float gFogEnd;
    float gStarRotation;
    float4 gSkyColorDaylight;
    float4 gFogColorStars;
    float4 gLightColorOpacity;
    float4 gTwilight;
    float4 gSunDirection;
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
    float fogDepth : TEXCOORD5;
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
    o.fogDepth = o.pos.w;
    return o;
}

// World pass: repeat the 16x16 tile across merged greedy quads.
// Quantize to texel centers within each tile at the selected mip level.
// Alpha test handles cutout blocks (leaves/glass); liquid faces keep their
// alpha and are drawn in a second blended pass.
float4 PSMain(PSIn input) : SV_TARGET
{
    float2 dx = ddx(input.uv) * 16.0f;
    float2 dy = ddy(input.uv) * 16.0f;
    float footprint2 = max(dot(dx, dx), dot(dy, dy));
    float lod = clamp(floor(0.5f * log2(max(footprint2, 1.0f)) + 0.5f), 0.0f, 4.0f);
    float scale = exp2(lod);
    float tileSize = 16.0f / scale;
    float2 atlasUV = input.tile + (floor(frac(input.uv) * tileSize) + 0.5f) * scale / 1024.0f;
    float4 tex = gAtlas.SampleLevel(gPoint, atlasUV, lod);
    clip(tex.a - 0.5f);
    float3 light = saturate(input.shade * gSkyColorDaylight.w * gLightColorOpacity.rgb + 0.02f);
    float3 dark = 1.0f - light;
    light = lerp(light, 1.0f - dark * dark * dark * dark, saturate(gBrightness));
    float3 col = tex.rgb * light * input.tint;
    float fog = 0.0f;
    if (gFogEnd > 0.0f) fog = smoothstep(gFogStart, gFogEnd, input.fogDepth);
    col = lerp(col, gFogColorStars.rgb, fog);
    return float4(col, tex.a);
}

// UI pass: plain textured quad (bound texture is a GUI sprite, not the atlas).
// frac() is a no-op for 0..1 sprite UVs and enables the tiled dirt background.
float4 PSMainUI(PSIn input) : SV_TARGET
{
    float4 tex = gAtlas.Sample(gPoint, frac(input.uv));
    return float4(tex.rgb * input.shade * input.tint, tex.a);
}

float4 PSMainEntity(PSIn input) : SV_TARGET
{
    float4 tex = gAtlas.Sample(gPoint, input.uv);
    clip(tex.a - 0.1f);
    float3 light = saturate(input.shade * gSkyColorDaylight.w * gLightColorOpacity.rgb + 0.02f);
    float3 dark = 1.0f - light;
    light = lerp(light, 1.0f - dark * dark * dark * dark, saturate(gBrightness));
    float fog = gFogEnd > 0 ? smoothstep(gFogStart, gFogEnd, input.fogDepth) : 0;
    return float4(lerp(tex.rgb * light * input.tint, gFogColorStars.rgb, fog), tex.a);
}

float4 PSMainWorldSky(PSIn input) : SV_TARGET
{
    float3 d = normalize(input.dir);
    float horizon = 1.0f - smoothstep(0.0f, 0.35f, abs(d.y));
    float3 col = lerp(gSkyColorDaylight.rgb, gFogColorStars.rgb, horizon);
    float facingSun = saturate(d.x * (gSunDirection.x >= 0.0f ? 1.0f : -1.0f));
    float glow = horizon * facingSun * facingSun * gTwilight.a;
    col = lerp(col, gTwilight.rgb, glow);
    return float4(col, 1.0f);
}

float4 PSMainCelestial(PSIn input) : SV_TARGET
{
    float4 tex = gAtlas.Sample(gPoint, input.uv);
    float3 d = normalize(input.dir);
    float height = d.x * sin(gStarRotation) + d.y * cos(gStarRotation);
    float aboveHorizon = smoothstep(0.0f, 0.02f, height);
    return float4(tex.rgb * input.shade * input.tint,
                  tex.a * gLightColorOpacity.w * aboveHorizon);
}

// Title panorama: sample the cubemap along the interpolated view direction.
float4 PSMainSky(PSIn input) : SV_TARGET
{
    float3 d = normalize(input.dir);
    return float4(gSky.Sample(gLinear, d).rgb, 1.0f);
}