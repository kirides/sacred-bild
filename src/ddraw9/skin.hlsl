// Characters skinned on the GPU (Device::drawSkinned) and lit as Direct3D 7's fixed-function pipeline lights them:
// up to 8 directional, point and spot lights in camera space, material or vertex colors, global ambient, specular
// with a local or distant viewer. Compiled to vs_2_0 by tools/gen_shaders.py (src/ddraw9/skin_shaders.inc); DIFFUSE
// selects the variant with a per-vertex diffuse color stream (FVF 0x152).
//
// Matrices are passed as the vectors the products need, so no packing convention is involved:
// - bones: 3 rows each, out = (dot(row0, v), dot(row1, v), dot(row2, v)) with v = (x, y, z, 1) (Granny's M v + t);
// - camera: the columns of world x view (row-vector convention: p' = p * M), the columns of the inverse transpose of
//   its 3x3 for normals, and the columns of the projection.

float4 g_worldView[3] : register(c0);
float4 g_projection[4] : register(c3);
float4 g_normalMatrix[3] : register(c7);
float4 g_materialDiffuse : register(c10);
float4 g_materialAmbient : register(c11);
float4 g_materialSpecular : register(c12);
float4 g_materialEmissive : register(c13);
float4 g_ambientPower : register(c14);      // rgb: D3DRENDERSTATE_AMBIENT, w: material power
float4 g_sources : register(c15);          // 1 = from the vertex diffuse color: x diffuse, y ambient, z specular, w emissive
float4 g_vertexColor : register(c16);      // without a diffuse stream: the vertices' one color (shadows), else white

// Per light, 7 registers: position (camera space) + range, direction (camera space, pointing away from the light) +
// falloff, diffuse, specular, ambient, attenuation 0-2 + 1 for point/spot lights, cos(theta/2), cos(phi/2), 1 for spot.
#define LIGHT_REGISTERS 7
float4 g_lights[8 * LIGHT_REGISTERS] : register(c17);
float4 g_bones[177] : register(c73);        // 59 bones; the compiler keeps its literals above

int g_lightCount : register(i0);
bool g_normalizeSkinned : register(b0);     // Granny normalized the skinned normals
bool g_normalizeNormals : register(b1);     // D3DRENDERSTATE_NORMALIZENORMALS
bool g_localViewer : register(b2);          // D3DRENDERSTATE_LOCALVIEWER
bool g_specular : register(b3);             // D3DRENDERSTATE_SPECULARENABLE
bool g_lighting : register(b4);             // D3DRENDERSTATE_LIGHTING

struct VsIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 bones : BLENDINDICES;            // D3DCOLOR
    float4 weights : BLENDWEIGHT;
#ifdef DIFFUSE
    float4 diffuse : COLOR0;
#endif
};

struct VsOut
{
    float4 position : POSITION;
    float4 diffuse : COLOR0;
    float4 specular : COLOR1;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
};

void addBone(int bone, float weight, float4 position, float3 normal, inout float3 p, inout float3 n)
{
    const float4 r0 = g_bones[bone];
    const float4 r1 = g_bones[bone + 1];
    const float4 r2 = g_bones[bone + 2];
    p += weight * float3(dot(r0, position), dot(r1, position), dot(r2, position));
    n += weight * float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
}

// Direct3D 7's fixed-function lighting of a camera-space vertex.
void light(float3 n, float3 pc, float4 vertexColor, out float4 diffuseOut, out float4 specularOut)
{
    float3 nc = float3(dot(n, g_normalMatrix[0].xyz), dot(n, g_normalMatrix[1].xyz), dot(n, g_normalMatrix[2].xyz));
    if (g_normalizeNormals)
    {
        nc = normalize(nc);
    }
    float3 toEye = float3(0, 0, -1);
    if (g_localViewer)
    {
        toEye = normalize(-pc);
    }

    float3 ambient = 0;
    float3 diffuse = 0;
    float3 specular = 0;
    for (int i = 0; i < g_lightCount; ++i)
    {
        const float4 lightPosition = g_lights[i * LIGHT_REGISTERS];
        const float4 lightDirection = g_lights[i * LIGHT_REGISTERS + 1];
        const float4 lightDiffuse = g_lights[i * LIGHT_REGISTERS + 2];
        const float4 lightSpecular = g_lights[i * LIGHT_REGISTERS + 3];
        const float4 lightAmbient = g_lights[i * LIGHT_REGISTERS + 4];
        const float4 attenuation = g_lights[i * LIGHT_REGISTERS + 5];
        const float4 spotCone = g_lights[i * LIGHT_REGISTERS + 6];

        // Direction to the light, attenuation (point and spot lights: 0 beyond the range) and spot cone.
        const float3 toLight = lightPosition.xyz - pc;
        const float distance = length(toLight);
        const float3 l = lerp(-lightDirection.xyz, toLight / max(distance, 1e-6), attenuation.w);
        const float pointAttenuation = (distance <= lightPosition.w ? 1.0 : 0.0) /
            max(attenuation.x + attenuation.y * distance + attenuation.z * distance * distance, 1e-6);
        const float rho = dot(-l, lightDirection.xyz);
        const float cone = saturate((rho - spotCone.y) / max(spotCone.x - spotCone.y, 1e-6));
        const float spot = lerp(1.0, pow(max(cone, 1e-6), lightDirection.w) * (rho > spotCone.y ? 1.0 : 0.0),
            spotCone.z);
        const float scale = lerp(1.0, pointAttenuation, attenuation.w) * spot;

        const float nDotL = dot(nc, l);
        ambient += scale * lightAmbient.rgb;
        diffuse += scale * max(nDotL, 0.0) * lightDiffuse.rgb;
        const float nDotH = dot(nc, normalize(toEye + l));
        const float lit = (nDotL > 0.0 ? 1.0 : 0.0) * (nDotH > 0.0 ? 1.0 : 0.0);
        specular += scale * lit * pow(max(nDotH, 1e-6), g_ambientPower.w) * lightSpecular.rgb;
    }

    const float4 cd = lerp(g_materialDiffuse, vertexColor, g_sources.x);
    const float4 ca = lerp(g_materialAmbient, vertexColor, g_sources.y);
    const float4 cs = lerp(g_materialSpecular, vertexColor, g_sources.z);
    const float4 ce = lerp(g_materialEmissive, vertexColor, g_sources.w);
    diffuseOut = saturate(float4(ce.rgb + ca.rgb * (g_ambientPower.rgb + ambient) + cd.rgb * diffuse, cd.a));
    specularOut = 0;
    if (g_specular)
    {
        specularOut = saturate(float4(cs.rgb * specular, 0.0));
    }
}

VsOut main(VsIn v)
{
    VsOut o;

    // Skinning: up to four bones, unused ones with weight 0.
    const int4 bones = D3DCOLORtoUBYTE4(v.bones) * 3;
    const float4 position = float4(v.position, 1.0);
    float3 p = 0;
    float3 n = 0;
    addBone(bones.x, v.weights.x, position, v.normal, p, n);
    addBone(bones.y, v.weights.y, position, v.normal, p, n);
    addBone(bones.z, v.weights.z, position, v.normal, p, n);
    addBone(bones.w, v.weights.w, position, v.normal, p, n);
    if (g_normalizeSkinned)
    {
        n = normalize(n);
    }

    // Camera space and projection.
    const float4 world = float4(p, 1.0);
    const float3 pc = float3(dot(world, g_worldView[0]), dot(world, g_worldView[1]), dot(world, g_worldView[2]));
    const float4 pc4 = float4(pc, 1.0);
    o.position = float4(dot(pc4, g_projection[0]), dot(pc4, g_projection[1]), dot(pc4, g_projection[2]),
        dot(pc4, g_projection[3]));
    o.uv0 = v.uv;
    o.uv1 = v.uv;
    o.uv2 = v.uv;
    o.uv3 = v.uv;

#ifdef DIFFUSE
    const float4 vertexColor = v.diffuse;
#else
    const float4 vertexColor = g_vertexColor;
#endif
    o.diffuse = vertexColor;
    o.specular = 0;
    if (g_lighting)
    {
        light(n, pc, vertexColor, o.diffuse, o.specular);
    }
    return o;
}
