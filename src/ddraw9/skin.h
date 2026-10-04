#pragma once
#include <windows.h>

#include <cstdint>

struct IDirect3DDevice7;

// Characters skinned in a vertex shader by the Direct3D 9 backend (skin.hlsl). The game side (GpuSkin) keeps each
// mesh's bind-pose vertices in a static vertex buffer and draws them with the bone matrices of the current pose, lit as
// Direct3D 7's fixed-function pipeline lit the vertices Granny skinned on the CPU.
namespace DDraw9::Skin
{
    constexpr uint32_t kMaxBones = 60;      // vertex shader constants c72..c251

    struct Vertex
    {
        float position[3];
        float normal[3];
        float uv[2];
        uint8_t bones[4];       // palette indices; unused influences have weight 0
        float weights[4];
    };

    struct Mesh;    // a static vertex buffer on the device that created it

    struct Draw
    {
        Mesh* mesh;
        const float* palette;   // per bone 12 floats: row-major 3x3 matrix, then translation (out = M v + t)
        uint32_t bones;
        uint32_t paletteId;     // the same id means the same palette values (not uploaded again); never 0
        bool normalizeSkinned;  // normalize the skinned normals, as Granny did
        const DWORD* diffuse;   // per-vertex diffuse color (FVF 0x152), `diffuseStride` bytes apart; null: none
        uint32_t diffuseStride;
        const WORD* indices;    // triangle list
        uint32_t indexCount;
    };

    // True if `device` is the backend's and it can run the skinning shader.
    bool available(IDirect3DDevice7* device);
    Mesh* createMesh(IDirect3DDevice7* device, const Vertex* vertices, uint32_t count);
    void releaseMesh(Mesh* mesh);
    // Draws with the device's current state; false (nothing drawn) if that state needs something the shader doesn't
    // do (fog, clip planes, generated texture coordinates, texture transforms, more than 8 lights).
    bool draw(IDirect3DDevice7* device, const Draw& draw);
}
