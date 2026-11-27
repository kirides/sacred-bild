// SMAA 1x (Jimenez et al., src/bild/ddraw9/smaa/SMAA.hlsl, MIT) on the NVRHI backend (ddrawrhi/smaa.cpp): luma edge
// detection, blending weights, neighborhood blending, each a full-screen triangle. The same passes as the Direct3D 9
// backend's (ddraw9/smaa.hlsl) in shader model 6, with SMAA's macros mapped to textures and samplers at fixed registers.
// Compiled per preset by tools/re/gen_rhi_shaders.py (smaa_shaders.inc); SMAA_PRESET_* comes from the command line.
//
// b0: (1 / width, 1 / height, width, height) of the frame. t0 color (edges, blending) / edges (weights), t1 area / blend,
// t2 search; s0 linear clamp, s1 point clamp.

cbuffer SmaaConstants : register(b0)
{
    float4 metrics;
};
#define SMAA_RT_METRICS metrics

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);

#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

#include "../../ddraw9/smaa/SMAA.hlsl"

// A triangle covering the target; texture coordinates 0..1 over it (y down).
void fullScreen(uint id, out float4 position, out float2 texcoord)
{
    texcoord = float2((id << 1) & 2, id & 2);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

void EdgeVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0,
    out float4 offset[3] : TEXCOORD1)
{
    fullScreen(id, position, texcoord);
    SMAAEdgeDetectionVS(texcoord, offset);
}

float4 EdgePS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset[3] : TEXCOORD1) : SV_Target
{
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, t0), 0.0, 0.0);
}

void WeightVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0,
    out float2 pixcoord : TEXCOORD1, out float4 offset[3] : TEXCOORD2)
{
    fullScreen(id, position, texcoord);
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}

float4 WeightPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float2 pixcoord : TEXCOORD1,
    float4 offset[3] : TEXCOORD2) : SV_Target
{
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, t0, t1, t2, 0.0);
}

void BlendVS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0,
    out float4 offset : TEXCOORD1)
{
    fullScreen(id, position, texcoord);
    SMAANeighborhoodBlendingVS(texcoord, offset);
}

float4 BlendPS(float4 position : SV_Position, float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : SV_Target
{
    return SMAANeighborhoodBlendingPS(texcoord, offset, t0, t1);
}
