#pragma once
#include <windows.h>

#include <cstdint>

struct IDirect3DDevice7;

// The ground drawn by the Direct3D 9 backend from static vertex buffers (ground.hlsl). The game side (GroundMesh) keeps
// each sector's tiles in buffers of its own, in the sector's isometric space; a draw places them on the screen the way
// the game placed its pretransformed quads, with the texture stages and render states the game set.
namespace DDraw9::Ground
{
    // One corner of a tile quad. Quads are 4 consecutive vertices (left, top, bottom, right), drawn as the triangles
    // 0 1 2 and 2 1 3 like the game's quad batcher.
    struct Vertex
    {
        float x, y;         // sector space, unzoomed pixels
        DWORD diffuse;
        float u0, v0;
        float u1, v1;
    };

    struct Mesh;    // a vertex buffer on the device that created it, appended to as tiles come into view

    struct Draw
    {
        Mesh* mesh;
        uint32_t firstQuad;
        uint32_t quads;
        // screen = position * scale + offset, in render target pixels (as XYZRHW vertices); z as the game's.
        float scale;
        float offsetX, offsetY;
        float z;
    };

    // True if `device` is the backend's and it can run the shader.
    bool available(IDirect3DDevice7* device);
    // A buffer for up to `capacity` quads.
    Mesh* createMesh(IDirect3DDevice7* device, uint32_t capacity);
    void releaseMesh(Mesh* mesh);
    uint32_t capacity(const Mesh* mesh);
    // Writes quads [first, first + count) (4 vertices each); they must not be in use by a draw of this frame.
    bool write(IDirect3DDevice7* device, Mesh* mesh, uint32_t first, const Vertex* vertices, uint32_t count);
    // Draws with the device's current state; false (nothing drawn) if that state needs something the shader doesn't do
    // (fog, generated texture coordinates, texture transforms).
    bool draw(IDirect3DDevice7* device, const Draw& draw);
}
