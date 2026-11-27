// Copies and fills for DDrawRhi (src/bild/ddrawrhi/context.cpp): a rectangle of the target, in normalized device
// coordinates, gets a rectangle of a texture (StretchRect, presenting the game's frame) or one color (Clear and
// color fill Blt with rectangles). Compiled by tools/re/gen_rhi_shaders.py into blit_shaders.inc.

cbuffer BlitConstants : register(b0)
{
    float4 dst;     // x0, y0, x1, y1 (normalized device coordinates, y up)
    float4 src;     // u0, v0, u1, v1
    float4 color;
    float depth;
    float3 pad;
};

Texture2D source : register(t0);
SamplerState samp : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut vsMain(uint id : SV_VertexID)
{
    // Two triangles: 0 1 2, 2 1 3 over the corners (x0,y0) (x1,y0) (x0,y1) (x1,y1).
    const uint c = id == 3 ? 2 : id == 4 ? 1 : id == 5 ? 3 : id;
    const bool right = (c & 1) != 0;
    const bool bottom = (c & 2) != 0;
    VSOut o;
    o.position = float4(right ? dst.z : dst.x, bottom ? dst.w : dst.y, depth, 1.0);
    o.uv = float2(right ? src.z : src.x, bottom ? src.w : src.y);
    return o;
}

float4 psCopy(VSOut i) : SV_Target
{
    return source.Sample(samp, i.uv);
}

float4 psFill(VSOut i) : SV_Target
{
    return color;
}
