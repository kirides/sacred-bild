// "Sharp bilinear" texture sampling for the world ([Render] TextureFilter=Sharp), as the pixel stage of Direct3D 7's
// stages 0 and 1: texels stay crisp, and only the edge between two texels is blended, over about one screen pixel. The
// coordinate moves toward the texel center by the part of the texel that is not within one pixel of its edge; the
// sample is then bilinear. Magnified much, it is point sampling with antialiased edges; minified (a texel smaller than a
// pixel) it is plain bilinear. Compiled to ps_2_a by tools/gen_shaders.py (src/bild/ddraw9/sharp_shaders.inc): the world's
// vertices go through the fixed-function pipeline or the skinning and ground vertex shaders (vs_2_0), which cannot be
// paired with ps_3_0.
//
// Reproduces the texture stage combiners for what Device::updateSharp lets through: stages 0 and 1 (stage 2 and up
// disabled), color and alpha operation SELECTARG1, SELECTARG2, MODULATE, MODULATE2X, MODULATE4X or ADD, arguments
// TEXTURE, DIFFUSE, CURRENT (the diffuse color in stage 0, stage 0's result in stage 1) or TFACTOR, then the specular
// color if D3DRENDERSTATE_SPECULARENABLE is set. Alpha test, blending and fog are render states the pipeline applies
// after the shader (fog makes Device::updateSharp stay with the fixed-function pipeline).

float4 g_size0 : register(c0);      // texture width, height, 1 / width, 1 / height
float4 g_color0 : register(c1);     // color operation, argument 1, argument 2
float4 g_alpha0 : register(c2);     // alpha operation, argument 1, argument 2
float4 g_factor : register(c3);     // D3DRENDERSTATE_TEXTUREFACTOR
float4 g_size1 : register(c4);
float4 g_color1 : register(c5);     // operation 1 (DISABLE) = no stage 1
float4 g_alpha1 : register(c6);
float4 g_flags : register(c7);      // x: add the specular color

sampler2D g_texture0 : register(s0);
sampler2D g_texture1 : register(s1);

float4 sampleSharp(sampler2D s, float2 uv, float4 size)
{
    float2 t = uv * size.xy;
    float2 w = max(fwidth(t), 1e-4);
    float2 f = saturate((frac(t - 0.5) - 0.5) / w + 0.5);
    return tex2D(s, (floor(t - 0.5) + 0.5 + f) * size.zw);
}

float4 pick(float code, float4 tex, float4 diffuse, float4 current)
{
    float4 r = code > 0.5 ? current : diffuse;      // 0 diffuse, 1 current
    r = code > 1.5 ? tex : r;                       // 2 texture
    r = code > 2.5 ? g_factor : r;                  // 3 texture factor
    return r;
}

float4 combine(float op, float4 a, float4 b)
{
    float4 r = a;                                   // 2 SELECTARG1
    r = op > 2.5 ? b : r;                           // 3 SELECTARG2
    r = op > 3.5 ? a * b : r;                       // 4 MODULATE
    r = op > 4.5 ? a * b * 2.0 : r;                 // 5 MODULATE2X
    r = op > 5.5 ? a * b * 4.0 : r;                 // 6 MODULATE4X
    r = op > 6.5 ? a + b : r;                       // 7 ADD
    return saturate(r);
}

float4 stageResult(float4 color, float4 alpha, float4 tex, float4 diffuse, float4 current)
{
    float4 c = combine(color.x, pick(color.y, tex, diffuse, current), pick(color.z, tex, diffuse, current));
    float4 a = combine(alpha.x, pick(alpha.y, tex, diffuse, current), pick(alpha.z, tex, diffuse, current));
    return float4(c.rgb, a.a);
}

float4 main(float4 diffuse : COLOR0, float4 specular : COLOR1, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : COLOR
{
    float4 r = stageResult(g_color0, g_alpha0, sampleSharp(g_texture0, uv0, g_size0), diffuse, diffuse);
    if (g_color1.x > 1.5)
    {
        r = stageResult(g_color1, g_alpha1, sampleSharp(g_texture1, uv1, g_size1), diffuse, r);
    }
    r.rgb = saturate(r.rgb + g_flags.x * specular.rgb);
    return r;
}
