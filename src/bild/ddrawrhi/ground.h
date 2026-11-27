#pragma once
#include "ddraw9/ground.h"

#include <cstdint>

struct IDirect3DDevice7;

// GroundMesh on the NVRHI backend: the same calls as DDraw9::Ground (ddraw9/ground.h), which forwards them here when the
// device is DDrawRhi's. A sector's quads live in a device-local vertex buffer, appended to with copies recorded into the
// frame; a draw goes through the fixed-function shader's ground mode (shaders/ff.hlsl), which places the quads on the
// screen as the game's pretransformed ones and then treats them as those.
namespace DDrawRhi::Ground
{
    struct Mesh;

    bool available(IDirect3DDevice7* device);
    Mesh* createMesh(IDirect3DDevice7* device, uint32_t capacity);
    void releaseMesh(Mesh* mesh);
    bool write(IDirect3DDevice7* device, Mesh* mesh, uint32_t first, const DDraw9::Ground::Vertex* vertices, uint32_t count);
    bool draw(IDirect3DDevice7* device, Mesh* mesh, const DDraw9::Ground::Draw& draw);
}
