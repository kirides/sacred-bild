// SMAA 1x (Jimenez et al., src/ddraw9/smaa/SMAA.hlsl, MIT) for the Direct3D 9 backend (Smaa::apply): luma edge
// detection, blending weights, neighborhood blending, each a full-screen quad. Compiled per preset to vs_3_0 /
// ps_3_0 by tools/gen_shaders.py (src/ddraw9/smaa_shaders.inc); SMAA_PRESET_* comes from the command line.
//
// c0: (1 / width, 1 / height, width, height) of the frame, in both the vertex and the pixel shaders.
// Samplers: s0 color (edges, blending) / edges (weights), s1 area / blend, s2 search; their states are set by the
// caller (clamp, linear; the search texture point-sampled).

float4 g_metrics : register(c0);
#define SMAA_RT_METRICS g_metrics
#define SMAA_HLSL_3 1

#include "smaa/SMAA.hlsl"

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);

// Edge detection (luma).
void EdgeVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0, out float4 offset[3] : TEXCOORD1)
{
    SMAAEdgeDetectionVS(texcoord, offset);
}

float4 EdgePS(float2 texcoord : TEXCOORD0, float4 offset[3] : TEXCOORD1) : COLOR
{
    return float4(SMAALumaEdgeDetectionPS(texcoord, offset, s0), 0.0, 0.0);
}

// Blending weights: s0 edges, s1 area, s2 search.
void WeightVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0, out float2 pixcoord : TEXCOORD1,
    out float4 offset[3] : TEXCOORD2)
{
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}

float4 WeightPS(float2 texcoord : TEXCOORD0, float2 pixcoord : TEXCOORD1, float4 offset[3] : TEXCOORD2) : COLOR
{
    return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, s0, s1, s2, 0.0);
}

// Neighborhood blending: s0 color, s1 blending weights.
void BlendVS(inout float4 position : POSITION, inout float2 texcoord : TEXCOORD0, out float4 offset : TEXCOORD1)
{
    SMAANeighborhoodBlendingVS(texcoord, offset);
}

float4 BlendPS(float2 texcoord : TEXCOORD0, float4 offset : TEXCOORD1) : COLOR
{
    return SMAANeighborhoodBlendingPS(texcoord, offset, s0, s1);
}
