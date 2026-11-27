// Direct3D 7's fixed-function pipeline for DDrawRhi (src/bild/ddrawrhi/device.cpp): one vertex and one pixel shader for
// every draw the game makes, driven by constants the device fills from the Direct3D 7 state. The rules followed are
// Direct3D 9's (what the D3D9 backend gets from the driver): pixel centers on integer coordinates, lighting in camera
// space, material sources, the texture stage combiners, the alpha test.
// Compiled by tools/re/gen_rhi_shaders.py into ff_shaders.inc.

#define FLAG_RHW            0x001   // pretransformed (XYZRHW) vertices
#define FLAG_LIGHTING       0x002
#define FLAG_NORMAL         0x004   // the vertex format has a normal
#define FLAG_DIFFUSE        0x008   // ... a diffuse color
#define FLAG_SPECULAR       0x010   // ... a specular color
#define FLAG_NORMALIZE      0x020   // NORMALIZENORMALS
#define FLAG_LOCALVIEWER    0x040
#define FLAG_SPECULARON     0x080   // SPECULARENABLE
#define FLAG_COLORVERTEX    0x100
#define FLAG_GROUND         0x200   // GroundMesh's quads (ddrawrhi/ground.h): sector space, placed by groundTransform

struct Light
{
    float4 diffuse;
    float4 specular;
    float4 ambient;
    float4 position;        // camera space; w: type (1 point, 2 spot, 3 directional)
    float4 direction;       // camera space, normalized, pointing away from the light
    float4 attenuation;     // range, att0, att1, att2
    float4 spot;            // falloff, cos(theta / 2), cos(phi / 2), unused
};

cbuffer VertexConstants : register(b0)
{
    row_major float4x4 worldView;
    row_major float4x4 projection;
    row_major float4x4 normalMatrix;     // inverse transpose of worldView (upper 3x3 used)
    row_major float4x4 texTransform[2];
    float4 target;              // render target width, height, 1 / width, 1 / height
    float4 materialDiffuse;
    float4 materialAmbient;
    float4 materialSpecular;
    float4 materialEmissive;
    float4 ambient;             // AMBIENT render state
    float power;
    uint vsFlags;
    uint materialSources;       // 2 bits each: diffuse, specular, ambient, emissive (0 material, 1 color 1, 2 color 2)
    uint lightCount;
    uint4 texCoords;            // per stage: coordinate set (bits 0-15), transform count (bits 16-23), projected (bit 24)
    Light lights[8];
    float4 groundTransform;     // FLAG_GROUND: target pixels = position * x + (y, z); depth w
};

cbuffer PixelConstants : register(b1)
{
    uint4 stage0;               // COLOROP, COLORARG1, COLORARG2, ALPHAOP
    uint4 stage0b;              // ALPHAARG1, ALPHAARG2, texture bound, unused
    uint4 stage1;
    uint4 stage1b;
    float4 textureFactor;
    uint alphaFunc;             // 0: alpha test off, else D3DCMP_*
    float alphaRef;             // 0..255
    uint psFlags;               // FLAG_SPECULARON
    uint pad;
};

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState samp0 : register(s0);
SamplerState samp1 : register(s1);

// Semantics without digits: NVRHI's Direct3D 12 input layouts number a name's elements from 0. The order is the
// Vulkan locations' (device.cpp lists the attributes in this order).
struct VSIn
{
    float4 position : POSITION;
    float3 normal : NORMAL;
    uint diffuse : DIFFUSE;
    uint specular : SPECULAR;
    float4 tc0 : TEXA;
    float4 tc1 : TEXB;
    float4 tc2 : TEXC;
    float4 tc3 : TEXD;
};

struct VSOut
{
    float4 position : SV_Position;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

float4 unpackColor(uint c)
{
    return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;
}

float4 materialColor(uint source, float4 material, float4 color1, float4 color2)
{
    if ((vsFlags & FLAG_COLORVERTEX) != 0)
    {
        if (source == 1 && (vsFlags & FLAG_DIFFUSE) != 0)
            return color1;
        if (source == 2 && (vsFlags & FLAG_SPECULAR) != 0)
            return color2;
    }
    return material;
}

float2 texCoord(uint stage, VSIn v)
{
    const uint info = texCoords[stage];
    const uint set = info & 0xFFFF;
    float4 tc = set == 0 ? v.tc0 : set == 1 ? v.tc1 : set == 2 ? v.tc2 : v.tc3;
    if (((info >> 16) & 0xFF) != 0)
    {
        // Two coordinates go in as (u, v, 1, 0): the matrix's third row is the translation, as in Direct3D 9.
        float4 t = mul(float4(tc.xy, 1.0, 0.0), texTransform[stage]);
        if (((info >> 24) & 1) != 0 && t.w != 0.0)
            t.xy /= t.w;
        return t.xy;
    }
    return tc.xy;
}

VSOut vsMain(VSIn v)
{
    VSOut o;
    const float4 color1 = (vsFlags & FLAG_DIFFUSE) != 0 ? unpackColor(v.diffuse) : float4(1, 1, 1, 1);
    const float4 color2 = (vsFlags & FLAG_SPECULAR) != 0 ? unpackColor(v.specular) : float4(0, 0, 0, 0);

    if ((vsFlags & FLAG_RHW) != 0)
    {
        // Screen pixels of the render target, Direct3D 9's pixel centers (integer coordinates) moved to the half pixel
        // of today's APIs; w = 1 / rhw keeps texturing perspective correct.
        float4 p = v.position;
        if ((vsFlags & FLAG_GROUND) != 0)
            p = float4(v.position.xy * groundTransform.x + groundTransform.yz, groundTransform.w, 1.0);
        const float w = p.w != 0.0 ? 1.0 / p.w : 1.0;
        const float x = (p.x + 0.5) * target.z * 2.0 - 1.0;
        const float y = 1.0 - (p.y + 0.5) * target.w * 2.0;
        o.position = float4(x * w, y * w, p.z * w, w);
        o.diffuse = color1;
        o.specular = color2;
    }
    else
    {
        const float4 viewPos = mul(float4(v.position.xyz, 1.0), worldView);
        o.position = mul(viewPos, projection);
        if ((vsFlags & FLAG_LIGHTING) != 0)
        {
            const float4 mDiffuse = materialColor(materialSources & 3, materialDiffuse, color1, color2);
            const float4 mSpecular = materialColor((materialSources >> 2) & 3, materialSpecular, color1, color2);
            const float4 mAmbient = materialColor((materialSources >> 4) & 3, materialAmbient, color1, color2);
            const float4 mEmissive = materialColor((materialSources >> 6) & 3, materialEmissive, color1, color2);
            float3 n = float3(0, 0, 0);
            if ((vsFlags & FLAG_NORMAL) != 0)
            {
                n = mul(float4(v.normal, 0.0), normalMatrix).xyz;
                if ((vsFlags & FLAG_NORMALIZE) != 0 && dot(n, n) > 0.0)
                    n = normalize(n);
            }
            const float3 p = viewPos.xyz;
            const float3 eye = (vsFlags & FLAG_LOCALVIEWER) != 0 ? -normalize(p) : float3(0, 0, -1);
            float3 diffuse = float3(0, 0, 0), specular = float3(0, 0, 0), ambientSum = float3(0, 0, 0);
            for (uint i = 0; i < lightCount; ++i)
            {
                const Light l = lights[i];
                const uint type = (uint)l.position.w;
                float3 toLight;
                float atten = 1.0;
                if (type == 3)
                {
                    toLight = -l.direction.xyz;
                }
                else
                {
                    const float3 d = l.position.xyz - p;
                    const float dist = length(d);
                    if (dist > l.attenuation.x)
                        continue;
                    toLight = dist > 0.0 ? d / dist : float3(0, 0, 1);
                    const float denom = l.attenuation.y + l.attenuation.z * dist + l.attenuation.w * dist * dist;
                    atten = denom > 0.0 ? 1.0 / denom : 1.0;
                    if (type == 2)
                    {
                        const float rho = dot(-toLight, l.direction.xyz);
                        if (rho <= l.spot.z)
                            atten = 0.0;
                        else if (rho <= l.spot.y)
                        {
                            const float range = l.spot.y - l.spot.z;
                            atten *= range > 0.0 ? pow(saturate((rho - l.spot.z) / range), l.spot.x) : 1.0;
                        }
                    }
                }
                ambientSum += l.ambient.rgb * atten;
                const float ndotl = dot(n, toLight);
                if (ndotl > 0.0)
                {
                    diffuse += l.diffuse.rgb * ndotl * atten;
                    if ((vsFlags & FLAG_SPECULARON) != 0 && power > 0.0)
                    {
                        const float3 h = normalize(toLight + eye);
                        specular += l.specular.rgb * pow(saturate(dot(n, h)), power) * atten;
                    }
                }
            }
            o.diffuse.rgb = saturate(mEmissive.rgb + ambient.rgb * mAmbient.rgb + ambientSum * mAmbient.rgb + diffuse * mDiffuse.rgb);
            o.diffuse.a = saturate(mDiffuse.a);
            o.specular = float4(saturate(specular * mSpecular.rgb), 0.0);
        }
        else
        {
            o.diffuse = color1;
            o.specular = color2;
        }
    }
    o.uv0 = texCoord(0, v);
    o.uv1 = texCoord(1, v);
    return o;
}

// D3DTA_*: 0 diffuse, 1 current, 2 texture, 3 texture factor, 4 specular; 0x10 complement, 0x20 alpha replicate.
float4 argument(uint a, float4 current, float4 diffuse, float4 specular, float4 texel)
{
    float4 x;
    switch (a & 0xF)
    {
    case 0: x = diffuse; break;
    case 1: x = current; break;
    case 2: x = texel; break;
    case 3: x = textureFactor; break;
    case 4: x = specular; break;
    default: x = current; break;
    }
    if ((a & 0x10) != 0)
        x = 1.0 - x;
    if ((a & 0x20) != 0)
        x = x.aaaa;
    return x;
}

// D3DTOP_* on one channel set (color or alpha); both arguments already chosen.
float4 operation(uint op, float4 a1, float4 a2, float4 current, float4 diffuse, float4 texel)
{
    switch (op)
    {
    case 2: return a1;                                      // SELECTARG1
    case 3: return a2;                                      // SELECTARG2
    case 4: return a1 * a2;                                 // MODULATE
    case 5: return saturate(a1 * a2 * 2.0);                 // MODULATE2X
    case 6: return saturate(a1 * a2 * 4.0);                 // MODULATE4X
    case 7: return saturate(a1 + a2);                       // ADD
    case 8: return saturate(a1 + a2 - 0.5);                 // ADDSIGNED
    case 9: return saturate((a1 + a2 - 0.5) * 2.0);         // ADDSIGNED2X
    case 10: return saturate(a1 - a2);                      // SUBTRACT
    case 11: return saturate(a1 + a2 - a1 * a2);            // ADDSMOOTH
    case 12: return lerp(a2, a1, diffuse.a);                // BLENDDIFFUSEALPHA
    case 13: return lerp(a2, a1, texel.a);                  // BLENDTEXTUREALPHA
    case 14: return lerp(a2, a1, textureFactor.a);          // BLENDFACTORALPHA
    case 15: return saturate(a1 + a2 * (1.0 - texel.a));    // BLENDTEXTUREALPHAPM
    case 16: return lerp(a2, a1, current.a);                // BLENDCURRENTALPHA
    case 18: return saturate(a1 + a1.a * a2);               // MODULATEALPHA_ADDCOLOR
    case 19: return saturate(a1 * a2 + a1.a);               // MODULATECOLOR_ADDALPHA
    case 20: return saturate((1.0 - a1.a) * a2 + a1);       // MODULATEINVALPHA_ADDCOLOR
    case 21: return saturate((1.0 - a1) * a2 + a1.a);       // MODULATEINVCOLOR_ADDALPHA
    case 24: return saturate(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4.0).xxxx;  // DOTPRODUCT3
    default: return a1;                                     // PREMODULATE, BUMPENVMAP*: not done
    }
}

// One texture stage: COLOROP 1 (DISABLE) ends the cascade, the caller checks.
float4 stage(uint4 s, uint4 sb, float4 current, float4 diffuse, float4 specular, float4 texel)
{
    const float4 c1 = argument(s.y, current, diffuse, specular, texel);
    const float4 c2 = argument(s.z, current, diffuse, specular, texel);
    const float4 color = operation(s.x, c1, c2, current, diffuse, texel);
    float alpha = current.a;
    if (s.w > 1)
    {
        const float4 a1 = argument(sb.x, current, diffuse, specular, texel);
        const float4 a2 = argument(sb.y, current, diffuse, specular, texel);
        alpha = operation(s.w, a1.aaaa, a2.aaaa, current, diffuse, texel).a;
    }
    return float4(color.rgb, alpha);
}

bool alphaPasses(float alpha)
{
    const float a = round(saturate(alpha) * 255.0);
    switch (alphaFunc)
    {
    case 1: return false;                   // NEVER
    case 2: return a < alphaRef;            // LESS
    case 3: return a == alphaRef;           // EQUAL
    case 4: return a <= alphaRef;           // LESSEQUAL
    case 5: return a > alphaRef;            // GREATER
    case 6: return a != alphaRef;           // NOTEQUAL
    case 7: return a >= alphaRef;           // GREATEREQUAL
    default: return true;                   // 0 (test off), ALWAYS
    }
}

float4 psMain(VSOut i) : SV_Target
{
    float4 current = i.diffuse;
    if (stage0.x > 1)
    {
        const float4 texel0 = stage0b.z != 0 ? tex0.Sample(samp0, i.uv0) : float4(1, 1, 1, 1);
        current = stage(stage0, stage0b, current, i.diffuse, i.specular, texel0);
        if (stage1.x > 1)
        {
            const float4 texel1 = stage1b.z != 0 ? tex1.Sample(samp1, i.uv1) : float4(1, 1, 1, 1);
            current = stage(stage1, stage1b, current, i.diffuse, i.specular, texel1);
        }
    }
    if ((psFlags & FLAG_SPECULARON) != 0)
        current.rgb = saturate(current.rgb + i.specular.rgb);
    if (!alphaPasses(current.a))
        discard;
    return current;
}
