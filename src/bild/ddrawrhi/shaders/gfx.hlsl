// bild_gfx.h's draws on the NVRHI backend (ddrawrhi/gfx.cpp): textured, colored triangles placed by a 4x4 matrix that
// maps the vertices (mesh units) to the render target's pixels, with per-draw texture coordinate handling. The draw's
// constants are push constants, so the draws of one texture and scissor share their pipeline state. Compiled by
// tools/re/gen_rhi_shaders.py into gfx_shaders.inc.

struct DrawConstants
{
    float4x4 toPixels;      // vertex (x, y, 0, 1) -> target pixels (divided by w)
    float4 uvRect;          // the image's rectangle in the texture: u0, v0, u1, v1
    float2 targetScale;     // 2 / width, -2 / height
    uint flags;             // see kFlip* below
    float pad;
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> c : register(b0);

static const uint kFlipU = 1;       // u -> 1 - u
static const uint kFlipV = 2;       // v -> 1 - v
static const uint kSwapUV = 4;      // u and v swapped first
static const uint kWrap = 8;        // coordinates past 0..1 repeat the image inside its rectangle
static const uint kStraight = 16;   // the vertex colors are premultiplied; drawn with straight alpha: divided by alpha
static const uint kSnap = 32;       // positions rounded to whole pixels

Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct VSIn
{
    float2 position : POSITION;
    float2 uv : TEXA;
    uint color : DIFFUSE;   // 0xAARRGGBB
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float snap(float x)
{
    return sign(x) * floor(abs(x) + 0.5);
}

VSOut vsMain(VSIn v)
{
    VSOut o;
    const float4 p = mul(c.toPixels, float4(v.position, 0.0, 1.0));
    float2 pix = p.xy / (p.w != 0.0 ? p.w : 1.0);
    if (c.flags & kSnap)
    {
        pix = float2(snap(pix.x), snap(pix.y));
    }
    pix += 0.5;     // Direct3D 9's pixel centers, as the device's pretransformed draws
    o.position = float4(pix.x * c.targetScale.x - 1.0, pix.y * c.targetScale.y + 1.0, 0.0, 1.0);
    float2 uv = (c.flags & kSwapUV) ? v.uv.yx : v.uv;
    if (c.flags & kFlipU)
    {
        uv.x = 1.0 - uv.x;
    }
    if (c.flags & kFlipV)
    {
        uv.y = 1.0 - uv.y;
    }
    o.uv = (c.flags & kWrap) ? uv : c.uvRect.xy + uv * (c.uvRect.zw - c.uvRect.xy);
    float4 color = float4((v.color >> 16) & 255, (v.color >> 8) & 255, v.color & 255, v.color >> 24) / 255.0;
    if ((c.flags & kStraight) && color.a > 0.0)
    {
        color.rgb = min(1.0, color.rgb / color.a);
    }
    o.color = color;
    return o;
}

float4 psMain(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    if (c.flags & kWrap)
    {
        uv = c.uvRect.xy + frac(uv) * (c.uvRect.zw - c.uvRect.xy);
    }
    return tex.Sample(samp, uv) * i.color;
}
