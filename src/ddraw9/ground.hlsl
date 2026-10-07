// The ground's tiles from static vertex buffers (Device::drawGround): positions in a sector's own isometric space,
// moved and scaled onto the screen as the game's pretransformed quads would have been, colors and texture coordinates
// as they are. The fixed-function pixel pipeline does the rest with the texture stages the game set. Compiled to
// vs_2_0 by tools/gen_shaders.py (src/ddraw9/ground_shaders.inc).
//
// Registers c250-c251 lie above the skinning shader's constants, so neither invalidates the other's.

float4 g_transform : register(c250);    // clip = position * xy + zw
float4 g_depth : register(c251);        // x: clip-space z

struct VsIn
{
    float2 position : POSITION;
    float4 diffuse : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

struct VsOut
{
    float4 position : POSITION;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

VsOut main(VsIn v)
{
    VsOut o;
    o.position = float4(v.position * g_transform.xy + g_transform.zw, g_depth.x, 1.0);
    o.diffuse = v.diffuse;
    o.specular = float4(0, 0, 0, 0);
    o.uv0 = v.uv0;
    o.uv1 = v.uv1;
    return o;
}
