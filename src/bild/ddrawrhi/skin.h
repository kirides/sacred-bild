#pragma once
#include "ddraw9/skin.h"

#include <cstdint>

struct IDirect3DDevice7;

// GpuSkin on the NVRHI backend: the same calls as DDraw9::Skin (ddraw9/skin.h), which forwards them here when the device
// is DDrawRhi's. A mesh's bind-pose vertices live in a device-local vertex buffer, its pieces' triangle lists in shared
// index arenas; a draw goes through the fixed-function shader's skinning mode (shaders/ff.hlsl) with the bone palette in
// a constant buffer of its own.
namespace DDrawRhi::Skin
{
    struct Mesh;
    struct Indices;

    bool available(IDirect3DDevice7* device);
    Mesh* createMesh(IDirect3DDevice7* device, const DDraw9::Skin::Vertex* vertices, uint32_t count);
    void releaseMesh(Mesh* mesh);
    Indices* createIndices(IDirect3DDevice7* device, const WORD* indices, uint32_t count);
    void releaseIndices(Indices* indices);
    bool draw(IDirect3DDevice7* device, Mesh* mesh, Indices* indices, const DDraw9::Skin::Draw& draw);
}
