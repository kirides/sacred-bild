// SacredBild's own 2D drawing on the NVRHI backend (ddrawrhi/context.cpp drawUi): the ImGui screens and the controller
// prompts, textured colored triangles in target pixels. Compiled by tools/re/gen_rhi_shaders.py into ui_shaders.inc.

cbuffer UiConstants : register(b0)
{
    float2 scale;           // 2 / target size, -2 / target height
    float2 pixelOffset;     // added to positions: 0.5 for Direct3D 9 style coordinates (the prompts), 0 for ImGui's
    uint colorOrder;        // 0: 0xAARRGGBB (Direct3D colors), 1: 0xAABBGGRR (ImGui's)
    float3 pad;
};

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct VSIn
{
    float2 position : POSITION;
    float2 uv : TEXA;
    uint color : DIFFUSE;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut vsMain(VSIn v)
{
    VSOut o;
    const float2 p = v.position + pixelOffset;
    o.position = float4(p.x * scale.x - 1.0, p.y * scale.y + 1.0, 0.0, 1.0);
    o.uv = v.uv;
    const float4 c = float4((v.color >> 16) & 255, (v.color >> 8) & 255, v.color & 255, v.color >> 24) / 255.0;
    o.color = colorOrder != 0 ? c.bgra : c;
    return o;
}

float4 psMain(VSOut i) : SV_Target
{
    return tex.Sample(samp, i.uv) * i.color;
}
