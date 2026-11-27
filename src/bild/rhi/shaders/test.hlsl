// tools/test/rhitest: a textured triangle that moves, with its tint and position in a constant buffer. Proves that
// pipelines, constant buffers, textures and samplers reach both APIs with the same bindings
// (tools/re/gen_rhi_shaders.py -> test_shaders.inc).

cbuffer Params : register(b0)
{
    float4 tint;
    float2 offset;
    float2 pad;
};

Texture2D tex : register(t0);
SamplerState smp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vsMain(uint id : SV_VertexID)
{
    const float2 corners[3] = { float2(0.0, 0.6), float2(0.55, -0.5), float2(-0.55, -0.5) };
    const float2 uvs[3] = { float2(0.5, 0.0), float2(1.0, 1.0), float2(0.0, 1.0) };
    VSOut o;
    o.pos = float4(corners[id] + offset, 0.0, 1.0);
    o.uv = uvs[id] * 4.0;
    return o;
}

float4 psMain(VSOut i) : SV_Target
{
    return tex.Sample(smp, i.uv) * tint;
}
